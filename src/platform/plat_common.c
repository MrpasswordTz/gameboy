/* =============================================================================
 *  plat_common.c — Host helpers shared by every video backend.
 *
 *    * plat_auto_backend() — pick SDL2, else X11, else nothing.
 *    * plat_now_us()       — monotonic microsecond clock.
 *    * plat_sleep_us()     — EINTR-safe sleep.
 *    * plat_write_png()    — a complete, dependency-free PNG encoder.
 *
 *  The PNG writer deliberately uses zlib "stored" (uncompressed) deflate
 *  blocks: that is a fully conformant zlib stream that costs us no compression
 *  code at all, and every real PNG decoder accepts it. CRC-32 and Adler-32 are
 *  implemented here from scratch.
 * ========================================================================== */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gb.h"
#include "platform.h"

/* ===========================================================================
 *  Backend selection
 * ======================================================================== */

/* The Makefile drops plat_sdl.c / plat_x11.c from the build when the matching
 * library is missing, so the getters may legitimately not exist at link time.
 * Re-declaring them as weak references lets this file link in every possible
 * configuration: an absent symbol resolves to NULL and we simply skip it.    */
#if defined(__GNUC__) || defined(__clang__)
#  define GB_WEAK __attribute__((weak))
#else
#  define GB_WEAK
#endif

GB_WEAK const PlatformBackend *plat_sdl_backend(void);
GB_WEAK const PlatformBackend *plat_x11_backend(void);

typedef const PlatformBackend *(*BackendGetter)(void);

/* A backend is only usable if it can actually be brought up and drawn to. */
static bool backend_usable(const PlatformBackend *b)
{
    return b && b->init && b->present && b->poll && b->shutdown;
}

static const PlatformBackend *try_getter(BackendGetter get)
{
    const PlatformBackend *b;

    if (!get)
        return NULL;
    b = get();
    return backend_usable(b) ? b : NULL;
}

const PlatformBackend *plat_auto_backend(void)
{
    /* Taking the address into a variable first keeps -Waddress quiet and works
     * identically for weak (possibly NULL) and ordinary symbols.             */
    BackendGetter sdl = plat_sdl_backend;
    BackendGetter x11 = plat_x11_backend;
    const PlatformBackend *b;

    b = try_getter(sdl);
    if (b)
        return b;
    b = try_getter(x11);
    if (b)
        return b;
    return NULL;
}

/* ===========================================================================
 *  Time
 * ======================================================================== */

u64 plat_now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        /* Should never happen; degrade to a coarse but monotonic-ish clock so
         * callers that diff timestamps keep working instead of dividing by 0. */
        return (u64)time(NULL) * 1000000u;
    }
    return (u64)ts.tv_sec * 1000000u + (u64)(ts.tv_nsec / 1000);
}

/* Absurd sleeps can overflow a 32-bit time_t and would hang the frontend
 * anyway; 1000 s is several orders of magnitude beyond any legitimate use. */
#define PLAT_SLEEP_MAX_US ((u64)1000000000u)

void plat_sleep_us(u64 us)
{
    struct timespec req, rem;

    if (us == 0)
        return;
    if (us > PLAT_SLEEP_MAX_US)
        us = PLAT_SLEEP_MAX_US;

    req.tv_sec  = (time_t)(us / 1000000u);
    req.tv_nsec = (long)((us % 1000000u) * 1000u);
    rem.tv_sec  = 0;
    rem.tv_nsec = 0;

    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR)
            break;                 /* EINVAL/EFAULT: nothing sane left to do */
        req = rem;                 /* resume with whatever time is left       */
        rem.tv_sec  = 0;
        rem.tv_nsec = 0;
        if (req.tv_sec == 0 && req.tv_nsec <= 0)
            break;
    }
}

/* ===========================================================================
 *  PNG writer — checksums
 * ======================================================================== */

static u32 png_crc_table[256];
static bool png_crc_table_ready = false;

static void png_crc_build_table(void)
{
    u32 n;

    for (n = 0; n < 256u; n++) {
        u32 c = n;
        int k;
        for (k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        png_crc_table[n] = c;
    }
    png_crc_table_ready = true;
}

/* Running CRC-32 (PNG/zlib polynomial), pre/post-conditioned by the caller. */
static u32 png_crc_update(u32 crc, const u8 *buf, size_t len)
{
    size_t i;

    if (!png_crc_table_ready)
        png_crc_build_table();
    for (i = 0; i < len; i++)
        crc = png_crc_table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

#define PNG_ADLER_BASE 65521u

static u32 png_adler32(const u8 *buf, size_t len)
{
    u32 a = 1u, b = 0u;
    size_t i = 0;

    while (i < len) {
        /* 5552 is the largest run that cannot overflow the 32-bit sums. */
        size_t n = len - i;
        size_t j;
        if (n > 5552u)
            n = 5552u;
        for (j = 0; j < n; j++) {
            a += buf[i + j];
            b += a;
        }
        a %= PNG_ADLER_BASE;
        b %= PNG_ADLER_BASE;
        i += n;
    }
    return (b << 16) | a;
}

/* ===========================================================================
 *  PNG writer — byte plumbing
 * ======================================================================== */

static bool png_write(FILE *f, const void *data, size_t len)
{
    if (len == 0)
        return true;
    return fwrite(data, 1, len, f) == len;
}

static void png_put_be32(u8 out[4], u32 v)
{
    out[0] = (u8)(v >> 24);
    out[1] = (u8)(v >> 16);
    out[2] = (u8)(v >> 8);
    out[3] = (u8)v;
}

/* One PNG chunk: length, 4-char type, payload, CRC-32 over (type + payload). */
static bool png_write_chunk(FILE *f, const char type[4], const u8 *data, size_t len)
{
    u8 hdr[8];
    u8 crcbuf[4];
    u32 crc;

    if (len > 0x7FFFFFFFu)
        return false;

    png_put_be32(hdr, (u32)len);
    hdr[4] = (u8)type[0];
    hdr[5] = (u8)type[1];
    hdr[6] = (u8)type[2];
    hdr[7] = (u8)type[3];

    crc = png_crc_update(0xFFFFFFFFu, hdr + 4, 4);
    if (len > 0)
        crc = png_crc_update(crc, data, len);
    crc ^= 0xFFFFFFFFu;
    png_put_be32(crcbuf, crc);

    if (!png_write(f, hdr, 8))
        return false;
    if (!png_write(f, data, len))
        return false;
    return png_write(f, crcbuf, 4);
}

/* ===========================================================================
 *  PNG writer — public entry point
 * ======================================================================== */

#define PNG_STORED_MAX 65535u    /* largest LEN of one stored deflate block */

bool plat_write_png(const char *path, const u32 *fb, int w, int h)
{
    static const u8 png_sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

    FILE   *f       = NULL;
    u8     *raw     = NULL;
    u8     *z       = NULL;
    size_t  stride, raw_size, nblocks, zsize, zi, off;
    u32     adler;
    u8      ihdr[13];
    int     y, x;
    bool    okflag = false;

    if (!path || !fb || w <= 0 || h <= 0)
        return false;
    /* PNG allows up to 2^31-1 per dimension, but nothing sane here is bigger
     * than a scaled Game Boy frame; the cap keeps every product below in range. */
    if (w > 65535 || h > 65535)
        return false;

    stride = (size_t)w * 3u + 1u;                 /* filter byte + RGB triples */
    raw_size = stride * (size_t)h;
    if (raw_size / stride != (size_t)h)           /* 32-bit size_t overflow    */
        return false;

    raw = (u8 *)malloc(raw_size);
    if (!raw)
        return false;

    for (y = 0; y < h; y++) {
        u8       *row = raw + (size_t)y * stride;
        const u32 *src = fb + (size_t)y * (size_t)w;
        size_t    o = 0;

        row[o++] = 0x00;                          /* filter type 0 = None      */
        for (x = 0; x < w; x++) {
            u32 p = src[x];                       /* 0xAARRGGBB, alpha dropped */
            row[o++] = (u8)(p >> 16);
            row[o++] = (u8)(p >> 8);
            row[o++] = (u8)p;
        }
    }

    adler = png_adler32(raw, raw_size);

    /* zlib stream: 2-byte header + N stored blocks + 4-byte Adler-32. */
    nblocks = (raw_size + (size_t)PNG_STORED_MAX - 1u) / (size_t)PNG_STORED_MAX;
    if (nblocks == 0)
        nblocks = 1;                              /* unreachable (raw_size>0)  */
    if (nblocks > (SIZE_MAX - raw_size - 6u) / 5u) {
        free(raw);
        return false;
    }
    zsize = 2u + nblocks * 5u + raw_size + 4u;
    if (zsize > 0x7FFFFFFFu) {                    /* one IDAT must fit a u32   */
        free(raw);
        return false;
    }

    z = (u8 *)malloc(zsize);
    if (!z) {
        free(raw);
        return false;
    }

    zi = 0;
    z[zi++] = 0x78;                               /* CMF: deflate, 32K window  */
    z[zi++] = 0x01;                               /* FLG: no dict, check ok    */

    off = 0;
    while (off < raw_size) {
        size_t chunk = raw_size - off;
        u16    len16, nlen16;
        bool   last;

        if (chunk > (size_t)PNG_STORED_MAX)
            chunk = (size_t)PNG_STORED_MAX;
        last = (off + chunk >= raw_size);

        len16  = (u16)chunk;
        nlen16 = (u16)(~len16);

        z[zi++] = (u8)(last ? 0x01 : 0x00);       /* BFINAL | BTYPE=00 (stored)*/
        z[zi++] = (u8)(len16 & 0xFFu);
        z[zi++] = (u8)(len16 >> 8);
        z[zi++] = (u8)(nlen16 & 0xFFu);
        z[zi++] = (u8)(nlen16 >> 8);
        memcpy(z + zi, raw + off, chunk);
        zi  += chunk;
        off += chunk;
    }

    z[zi++] = (u8)(adler >> 24);
    z[zi++] = (u8)(adler >> 16);
    z[zi++] = (u8)(adler >> 8);
    z[zi++] = (u8)adler;

    free(raw);
    raw = NULL;

    /* zi must now equal zsize exactly; if it does not our maths is wrong and
     * writing zsize bytes would leak uninitialised memory into the file.     */
    if (zi != zsize) {
        free(z);
        return false;
    }

    f = fopen(path, "wb");
    if (!f) {
        free(z);
        return false;
    }

    png_put_be32(ihdr + 0, (u32)w);
    png_put_be32(ihdr + 4, (u32)h);
    ihdr[8]  = 8;      /* bit depth                                           */
    ihdr[9]  = 2;      /* colour type 2 = truecolour RGB                      */
    ihdr[10] = 0;      /* compression method: deflate                         */
    ihdr[11] = 0;      /* filter method 0                                     */
    ihdr[12] = 0;      /* no interlace                                        */

    if (!png_write(f, png_sig, sizeof png_sig))
        goto done;
    if (!png_write_chunk(f, "IHDR", ihdr, sizeof ihdr))
        goto done;
    if (!png_write_chunk(f, "IDAT", z, zsize))
        goto done;
    if (!png_write_chunk(f, "IEND", NULL, 0))
        goto done;

    okflag = true;

done:
    free(z);
    if (fclose(f) != 0)
        okflag = false;            /* a failed flush means the file is short  */
    if (!okflag)
        remove(path);              /* never leave a corrupt screenshot behind */
    return okflag;
}

/* CONTRACT-NOTE: include/platform.h declares plat_sdl_backend() and
 * plat_x11_backend() unconditionally and the task states both getters exist in
 * every build, but the project Makefile filters src/platform/plat_sdl.c and
 * src/platform/plat_x11.c out of $(SRCS) when the corresponding library is not
 * detected. A build without SDL2 (or without X11) would therefore fail to link
 * plat_auto_backend(). The header was NOT modified; instead this file
 * re-declares the two getters as weak references, so a missing backend
 * resolves to a NULL function pointer that plat_auto_backend() skips. On a
 * compiler without __attribute__((weak)) the declarations degrade to ordinary
 * ones and the original link requirement applies unchanged.
 */
