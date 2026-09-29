/* =============================================================================
 *  savestate.c — versioned save states.
 *
 *  File layout (little-endian header, then raw machine blob, then cart RAM):
 *
 *      offset  size  field
 *      ------  ----  -------------------------------------------------------
 *       0       4    magic  "GBST"
 *       4       4    version                (ST_VERSION)
 *       8       4    header_checksum        (cart.header_checksum)
 *      12       4    rom_size               (cart.rom_size)
 *      16       4    cart_ram_size          (bytes of cart RAM that follow)
 *      20       4    blob_size              (sizeof(struct GB))
 *      24     blob   the machine blob       (struct GB, pointers NULLed)
 *      24+b   ram    cartridge RAM image
 *
 *  `struct GB` owns several host-side pointers (cart.rom, cart.ram, mmu.boot,
 *  trace_file) and a 128 KiB audio ring. Those are *never* round-tripped: the
 *  snapshot written to disk has them zeroed, and on load the live values are
 *  written straight back over whatever came out of the file. Loading a state
 *  therefore never clobbers a live pointer and never leaks an allocation.
 *
 *  A state is bound to the ROM it came from by header_checksum + rom_size, and
 *  to the build that produced it by blob_size; any mismatch is refused.
 * ========================================================================== */
#include "gb.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define ST_MAGIC0   'G'
#define ST_MAGIC1   'B'
#define ST_MAGIC2   'S'
#define ST_MAGIC3   'T'
#define ST_VERSION  1u
#define ST_HDR_SIZE 24u

/* Cartridge RAM never legitimately exceeds 128 KiB; allow plenty of slack for
 * homebrew mappers but refuse anything absurd so a corrupt header cannot make
 * us attempt a gigabyte-sized allocation. */
#define ST_MAX_RAM  (8u * 1024u * 1024u)

/* Longest path we will build a "<path>.tmp" companion for. */
#define ST_MAX_PATH 4000u

/* ------------------------------------------------------------------ helpers */

#if defined(__GNUC__)
static void st_err(GB *gb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
static void st_err(GB *gb, const char *fmt, ...);
#endif

static void st_err(GB *gb, const char *fmt, ...)
{
    /* Deliberately tiny: last_error is a fixed 256-byte buffer in struct GB. */
    va_list ap;
    if (!gb) return;
    va_start(ap, fmt);
    vsnprintf(gb->last_error, sizeof gb->last_error, fmt, ap);
    va_end(ap);
}

static void st_put_u32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

static u32 st_get_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int st_clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* --------------------------------------------------------------- sanitising */
/*
 * A state file is untrusted input: it may be truncated, hand-edited, produced
 * by an older build, or simply corrupt. Every field that later ends up as an
 * array subscript is forced back into range here, so a bad file can make the
 * emulation wrong but can never make it read or write out of bounds.
 */
static void st_sanitize(GB *gb)
{
    size_t nbanks;
    int i;

    /* ---- machine ---------------------------------------------------- */
    if (gb->speed != 1 && gb->speed != 2) gb->speed = 1;   /* never 0: divisor */
    if (gb->model != GB_MODEL_AUTO && gb->model != GB_MODEL_DMG &&
        gb->model != GB_MODEL_CGB)
        gb->model = GB_MODEL_AUTO;

    /* ---- MMU -------------------------------------------------------- */
    gb->mmu.wram_bank &= 0x07;
    if (gb->mmu.wram_bank == 0) gb->mmu.wram_bank = 1;     /* SVBK 0 maps to 1 */
    gb->mmu.dma_pos         = st_clamp_i(gb->mmu.dma_pos, 0, 160);
    gb->mmu.dma_timer       = st_clamp_i(gb->mmu.dma_timer, 0, 4096);
    gb->mmu.dma_start_delay = st_clamp_i(gb->mmu.dma_start_delay, 0, 4096);
    if (gb->mmu.hdma_len > 0x800) gb->mmu.hdma_len = 0;

    /* ---- PPU -------------------------------------------------------- */
    gb->ppu.vram_bank &= 0x01;
    gb->ppu.mode      &= 0x03;
    gb->ppu.dots       = st_clamp_i(gb->ppu.dots, 0, 455);
    gb->ppu.mode3_len  = st_clamp_i(gb->ppu.mode3_len, 0, 456);
    if (gb->ppu.ly > 153) gb->ppu.ly = 0;
    if (gb->ppu.wly > 143) gb->ppu.wly = 0;
    gb->ppu.bcps &= 0xBF;
    gb->ppu.ocps &= 0xBF;

    /* ---- APU -------------------------------------------------------- */
    gb->apu.frame_seq       = st_clamp_i(gb->apu.frame_seq, 0, 7);
    gb->apu.frame_seq_timer = st_clamp_i(gb->apu.frame_seq_timer, 0, 8192);

    gb->apu.ch1.duty_pos   = gb->apu.ch1.duty_pos & 7;
    gb->apu.ch2.duty_pos   = gb->apu.ch2.duty_pos & 7;
    gb->apu.ch1.timer      = st_clamp_i(gb->apu.ch1.timer, 0, 0x20000);
    gb->apu.ch2.timer      = st_clamp_i(gb->apu.ch2.timer, 0, 0x20000);
    gb->apu.ch1.length     = st_clamp_i(gb->apu.ch1.length, 0, 64);
    gb->apu.ch2.length     = st_clamp_i(gb->apu.ch2.length, 0, 64);
    gb->apu.ch1.volume     = st_clamp_i(gb->apu.ch1.volume, 0, 15);
    gb->apu.ch2.volume     = st_clamp_i(gb->apu.ch2.volume, 0, 15);
    gb->apu.ch1.env_start_vol = st_clamp_i(gb->apu.ch1.env_start_vol, 0, 15);
    gb->apu.ch2.env_start_vol = st_clamp_i(gb->apu.ch2.env_start_vol, 0, 15);
    gb->apu.ch1.env_timer  = st_clamp_i(gb->apu.ch1.env_timer, 0, 8);
    gb->apu.ch2.env_timer  = st_clamp_i(gb->apu.ch2.env_timer, 0, 8);
    gb->apu.ch1.env_period = st_clamp_i(gb->apu.ch1.env_period, 0, 8);
    gb->apu.ch2.env_period = st_clamp_i(gb->apu.ch2.env_period, 0, 8);
    gb->apu.ch1.sweep_timer  = st_clamp_i(gb->apu.ch1.sweep_timer, 0, 8);
    gb->apu.ch1.sweep_period = st_clamp_i(gb->apu.ch1.sweep_period, 0, 8);
    gb->apu.ch1.sweep_shift  = st_clamp_i(gb->apu.ch1.sweep_shift, 0, 7);
    gb->apu.ch1.sweep_shadow = st_clamp_i(gb->apu.ch1.sweep_shadow, 0, 0x7FF);
    gb->apu.ch2.sweep_timer  = 0;   /* ch2 has no sweep */
    gb->apu.ch2.sweep_period = 0;
    gb->apu.ch2.sweep_shift  = 0;
    gb->apu.ch2.sweep_shadow = 0;

    gb->apu.ch3.pos          = gb->apu.ch3.pos & 31;
    gb->apu.ch3.timer        = st_clamp_i(gb->apu.ch3.timer, 0, 0x20000);
    gb->apu.ch3.length       = st_clamp_i(gb->apu.ch3.length, 0, 256);
    gb->apu.ch3.volume_shift = st_clamp_i(gb->apu.ch3.volume_shift, 0, 4);

    gb->apu.ch4.timer         = st_clamp_i(gb->apu.ch4.timer, 0, 0x100000);
    gb->apu.ch4.length        = st_clamp_i(gb->apu.ch4.length, 0, 64);
    gb->apu.ch4.volume        = st_clamp_i(gb->apu.ch4.volume, 0, 15);
    gb->apu.ch4.env_start_vol = st_clamp_i(gb->apu.ch4.env_start_vol, 0, 15);
    gb->apu.ch4.env_timer     = st_clamp_i(gb->apu.ch4.env_timer, 0, 8);
    gb->apu.ch4.env_period    = st_clamp_i(gb->apu.ch4.env_period, 0, 8);

    if (!(gb->apu.master_volume >= 0.0f) || gb->apu.master_volume > 1.0f)
        gb->apu.master_volume = 1.0f;      /* also catches NaN */

    /* ---- timer / serial --------------------------------------------- */
    gb->serial.bit     = st_clamp_i(gb->serial.bit, 0, 8);
    gb->serial.timer   = st_clamp_i(gb->serial.timer, 0, 0x20000);
    gb->serial.log_len = st_clamp_i(gb->serial.log_len, 0,
                                    (int)sizeof gb->serial.log - 1);
    gb->serial.log[sizeof gb->serial.log - 1] = '\0';

    /* ---- cartridge banking ------------------------------------------ */
    nbanks = gb->cart.rom_size / 0x4000u;
    if (nbanks == 0) {
        gb->cart.rom_bank   = 0;
        gb->cart.rom_bank_0 = 0;
    } else {
        gb->cart.rom_bank   = (u32)(gb->cart.rom_bank   % nbanks);
        gb->cart.rom_bank_0 = (u32)(gb->cart.rom_bank_0 % nbanks);
    }

    nbanks = gb->cart.ram_size / 0x2000u;
    if (nbanks == 0) gb->cart.ram_bank = 0;
    else             gb->cart.ram_bank = (u32)(gb->cart.ram_bank % nbanks);

    if ((unsigned)gb->cart.mbc > (unsigned)MBC_UNKNOWN)
        gb->cart.mbc = MBC_UNKNOWN;
    if (gb->cart.rtc_select > 0x0C) gb->cart.rtc_select = 0;
    gb->cart.title[sizeof gb->cart.title - 1] = '\0';
    gb->cart.save_path[sizeof gb->cart.save_path - 1] = '\0';

    /* ---- debug bookkeeping ------------------------------------------ */
    i = (int)(sizeof gb->breakpoints / sizeof gb->breakpoints[0]);
    gb->n_breakpoints = st_clamp_i(gb->n_breakpoints, 0, i);
    gb->last_error[sizeof gb->last_error - 1] = '\0';
}

/* ------------------------------------------------------------------- commit */
/*
 * Copy `snap` over the live machine, then put every host-owned field back.
 * Nothing here can fail, so the caller has already completed all I/O: either
 * the whole state lands or none of it does.
 */
static void st_commit(GB *gb, const GB *snap, const u8 *ram, size_t ram_sz)
{
    /* --- host-owned state that must survive the blob copy ------------- */
    u8     *live_rom       = gb->cart.rom;
    size_t  live_rom_size  = gb->cart.rom_size;
    u8     *live_ram       = gb->cart.ram;
    size_t  live_ram_size  = gb->cart.ram_size;
    u8     *live_boot      = gb->mmu.boot;
    size_t  live_boot_size = gb->mmu.boot_size;
    FILE   *live_tracef    = gb->trace_file;
    bool    live_trace     = gb->trace;
    int     live_nbp       = gb->n_breakpoints;
    bool    live_running   = gb->running;
    bool    live_paused    = gb->paused;
    bool    live_hit_bp    = gb->hit_breakpoint;
    bool    live_throttle  = gb->throttle_ticks;
    float   live_volume    = gb->apu.master_volume;
    bool    live_muted     = gb->apu.muted;
    u32     live_bp[sizeof gb->breakpoints / sizeof gb->breakpoints[0]];
    char    live_save_path[sizeof gb->cart.save_path];

    memcpy(live_bp, gb->breakpoints, sizeof live_bp);
    memcpy(live_save_path, gb->cart.save_path, sizeof live_save_path);

    memcpy(gb, snap, sizeof(GB));

    /* --- restore, unconditionally, before anything can use them ------- */
    gb->cart.rom       = live_rom;
    gb->cart.rom_size  = live_rom_size;
    gb->cart.ram       = live_ram;
    gb->cart.ram_size  = live_ram_size;
    gb->mmu.boot       = live_boot;
    gb->mmu.boot_size  = live_boot_size;
    gb->trace_file     = live_tracef;
    gb->trace          = live_trace;
    gb->n_breakpoints  = live_nbp;
    memcpy(gb->breakpoints, live_bp, sizeof live_bp);
    memcpy(gb->cart.save_path, live_save_path, sizeof live_save_path);

    /* Host control flags belong to the frontend, not to the snapshot.
     * throttle_ticks in particular is a re-entrancy guard: inheriting a stale
     * `true` from a file would wedge gb_tick() permanently. */
    gb->running        = live_running;
    gb->paused         = live_paused;
    gb->hit_breakpoint = live_hit_bp;
    gb->throttle_ticks = live_throttle;

    /* Audio: drop whatever was queued and resync the ring. The saved ring is
     * always zeroed, but clear it anyway so a doctored file cannot inject
     * garbage samples into the mixer. */
    memset(gb->apu.ring, 0, sizeof gb->apu.ring);
    gb->apu.wr = 0;
    gb->apu.rd = 0;
    gb->apu.hp_l = 0.0f;
    gb->apu.hp_r = 0.0f;
    gb->apu.master_volume = live_volume;
    gb->apu.muted         = live_muted;

    st_sanitize(gb);

    /* --- cartridge RAM ------------------------------------------------ */
    if (ram && ram_sz && gb->cart.ram && gb->cart.ram_size) {
        size_t n = (ram_sz < gb->cart.ram_size) ? ram_sz : gb->cart.ram_size;
        memcpy(gb->cart.ram, ram, n);
    }
    gb->cart.ram_dirty = true;

    gb->last_error[0] = '\0';
}

/* ================================================================== SAVE === */

bool savestate_save(GB *gb, const char *path)
{
    u8      hdr[ST_HDR_SIZE];
    FILE   *f    = NULL;
    GB     *snap = NULL;
    char   *tmp  = NULL;
    size_t  plen, ram_size;

    if (!gb) return false;

    if (!path || path[0] == '\0') {
        st_err(gb, "save state: no path given");
        return false;
    }
    if (!gb->cart.rom || gb->cart.rom_size == 0) {
        st_err(gb, "save state: no ROM is loaded");
        return false;
    }

    plen = strlen(path);
    if (plen > ST_MAX_PATH) {
        st_err(gb, "save state: path too long (%zu bytes)", plen);
        return false;
    }

    /* "<path>.tmp" — written first, renamed into place, so an interrupted
     * save can never destroy a good existing state. */
    tmp = malloc(plen + 5);
    if (!tmp) {
        st_err(gb, "save state: out of memory");
        return false;
    }
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);

    snap = malloc(sizeof(GB));
    if (!snap) {
        st_err(gb, "save state: out of memory (%zu bytes)", sizeof(GB));
        free(tmp);
        return false;
    }
    memcpy(snap, gb, sizeof(GB));

    /* Pointers and host-only fields never go to disk. */
    snap->cart.rom   = NULL;
    snap->cart.ram   = NULL;
    snap->mmu.boot   = NULL;
    snap->trace_file = NULL;
    snap->trace      = false;
    memset(snap->cart.save_path, 0, sizeof snap->cart.save_path);
    memset(snap->breakpoints, 0, sizeof snap->breakpoints);
    snap->n_breakpoints  = 0;
    snap->hit_breakpoint = false;
    memset(snap->last_error, 0, sizeof snap->last_error);

    /* Queued audio is worthless once reloaded — drop the whole ring. */
    memset(snap->apu.ring, 0, sizeof snap->apu.ring);
    snap->apu.wr = 0;
    snap->apu.rd = 0;

    ram_size = (gb->cart.ram && gb->cart.ram_size) ? gb->cart.ram_size : 0;
    if (ram_size > ST_MAX_RAM) ram_size = ST_MAX_RAM;

    hdr[0] = (u8)ST_MAGIC0;
    hdr[1] = (u8)ST_MAGIC1;
    hdr[2] = (u8)ST_MAGIC2;
    hdr[3] = (u8)ST_MAGIC3;
    st_put_u32(hdr +  4, ST_VERSION);
    st_put_u32(hdr +  8, (u32)gb->cart.header_checksum);
    st_put_u32(hdr + 12, (u32)gb->cart.rom_size);
    st_put_u32(hdr + 16, (u32)ram_size);
    st_put_u32(hdr + 20, (u32)sizeof(GB));

    f = fopen(tmp, "wb");
    if (!f) {
        st_err(gb, "save state: cannot create %s: %s", tmp, strerror(errno));
        goto fail;
    }

    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        st_err(gb, "save state: short write on header: %s", strerror(errno));
        goto fail;
    }
    if (fwrite(snap, 1, sizeof(GB), f) != sizeof(GB)) {
        st_err(gb, "save state: short write on machine blob: %s",
               strerror(errno));
        goto fail;
    }
    if (ram_size && fwrite(gb->cart.ram, 1, ram_size, f) != ram_size) {
        st_err(gb, "save state: short write on cart RAM: %s", strerror(errno));
        goto fail;
    }
    if (fflush(f) != 0) {
        st_err(gb, "save state: flush failed: %s", strerror(errno));
        goto fail;
    }
    if (fclose(f) != 0) {
        f = NULL;                       /* closed (or lost) either way */
        st_err(gb, "save state: close failed: %s", strerror(errno));
        goto fail;
    }
    f = NULL;

    if (rename(tmp, path) != 0) {
        st_err(gb, "save state: cannot rename %s -> %s: %s", tmp, path,
               strerror(errno));
        goto fail;
    }

    free(snap);
    free(tmp);
    return true;

fail:
    if (f) fclose(f);
    remove(tmp);                        /* best effort; ignore failure */
    free(snap);
    free(tmp);
    return false;
}

/* ================================================================== LOAD === */

bool savestate_load(GB *gb, const char *path)
{
    u8      hdr[ST_HDR_SIZE];
    FILE   *f    = NULL;
    GB     *snap = NULL;
    u8     *ram  = NULL;
    u32     version, hchk, rom_size, ram_size, blob_size;
    size_t  ram_sz;

    if (!gb) return false;

    if (!path || path[0] == '\0') {
        st_err(gb, "load state: no path given");
        return false;
    }
    if (!gb->cart.rom || gb->cart.rom_size == 0) {
        st_err(gb, "load state: no ROM is loaded");
        return false;
    }

    f = fopen(path, "rb");
    if (!f) {
        st_err(gb, "load state: cannot open %s: %s", path, strerror(errno));
        return false;
    }

    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        st_err(gb, "load state: %s is truncated (no header)", path);
        goto fail;
    }
    if (hdr[0] != (u8)ST_MAGIC0 || hdr[1] != (u8)ST_MAGIC1 ||
        hdr[2] != (u8)ST_MAGIC2 || hdr[3] != (u8)ST_MAGIC3) {
        st_err(gb, "load state: %s is not a save state", path);
        goto fail;
    }

    version   = st_get_u32(hdr +  4);
    hchk      = st_get_u32(hdr +  8);
    rom_size  = st_get_u32(hdr + 12);
    ram_size  = st_get_u32(hdr + 16);
    blob_size = st_get_u32(hdr + 20);

    if (version != ST_VERSION) {
        st_err(gb, "load state: version %u not supported (expected %u)",
               version, ST_VERSION);
        goto fail;
    }
    if (blob_size != (u32)sizeof(GB)) {
        st_err(gb, "load state: blob size %u != %zu — state was written by a "
                   "different build", blob_size, sizeof(GB));
        goto fail;
    }
    if (hchk != (u32)gb->cart.header_checksum) {
        st_err(gb, "load state: belongs to a different ROM "
                   "(state is for header checksum %02X, loaded ROM is %02X)",
               (unsigned)(hchk & 0xFFu),
               (unsigned)gb->cart.header_checksum);
        goto fail;
    }
    if ((size_t)rom_size != gb->cart.rom_size) {
        st_err(gb, "load state: belongs to a different ROM "
                   "(state is for a %u byte ROM, loaded ROM is %zu bytes)",
               rom_size, gb->cart.rom_size);
        goto fail;
    }
    if (ram_size > ST_MAX_RAM) {
        st_err(gb, "load state: implausible cart RAM size %u — file is corrupt",
               ram_size);
        goto fail;
    }

    snap = malloc(sizeof(GB));
    if (!snap) {
        st_err(gb, "load state: out of memory (%zu bytes)", sizeof(GB));
        goto fail;
    }
    if (fread(snap, 1, sizeof(GB), f) != sizeof(GB)) {
        st_err(gb, "load state: %s is truncated (machine blob)", path);
        goto fail;
    }

    ram_sz = (size_t)ram_size;
    if (ram_sz) {
        ram = malloc(ram_sz);
        if (!ram) {
            st_err(gb, "load state: out of memory (%zu bytes of cart RAM)",
                   ram_sz);
            goto fail;
        }
        if (fread(ram, 1, ram_sz, f) != ram_sz) {
            st_err(gb, "load state: %s is truncated (cart RAM)", path);
            goto fail;
        }
    }

    fclose(f);
    f = NULL;

    /* All I/O succeeded — commit is now infallible. */
    st_commit(gb, snap, ram, ram_sz);

    free(ram);
    free(snap);
    return true;

fail:
    if (f) fclose(f);
    free(ram);
    free(snap);
    return false;
}

/* -----------------------------------------------------------------------------
 * CONTRACT-NOTE: none of the declarations in include/gb.h needed working
 * around, but two properties of `struct GB` shape this file and are worth
 * recording for whoever integrates it:
 *
 *  1. struct GB interleaves emulated machine state with host-owned state
 *     (cart.rom/ram, mmu.boot, cart.save_path, trace/trace_file, breakpoints,
 *     running/paused/hit_breakpoint/throttle_ticks, apu.master_volume/muted).
 *     There is no marker separating the two, so the save path zeroes those
 *     fields in its private copy and the load path re-installs the live values
 *     explicitly in st_commit(). If a host-owned field is ever added to
 *     struct GB, add it to st_commit()'s preserve list too, otherwise loading
 *     a state will overwrite it (and, for a pointer, leak or dangle).
 *
 *  2. The body is a raw image of struct GB, so a state file is only portable
 *     between binaries with identical struct layout. That is enforced rather
 *     than assumed: the header stores sizeof(struct GB) and a load refuses any
 *     file whose blob size differs, which catches ABI/compiler/bitness changes.
 *     Byte order is not separately tagged; the 24-byte header is written
 *     explicitly little-endian, and a big-endian build would additionally need
 *     field-wise (de)serialisation of the blob if cross-host states ever
 *     matter. Bump ST_VERSION whenever struct GB's meaning changes in a way
 *     size alone does not reveal.
 * -------------------------------------------------------------------------- */
