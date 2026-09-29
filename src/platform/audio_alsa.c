/* =============================================================================
 *  audio_alsa.c — ALSA playback sink loaded at run time with dlopen().
 *
 *  The ALSA development headers are deliberately NOT used: only libasound.so.2
 *  needs to be present on the machine, and even that is optional. Every symbol
 *  we need is declared here against ALSA's stable ABI and resolved with
 *  dlsym(). If anything at all goes wrong — no library, missing symbol, no
 *  sound card, device busy — we print one short warning and turn every entry
 *  point into a harmless no-op. Audio must never be the reason the emulator
 *  refuses to run.
 *
 *  audio_push() is called from the emulation loop and therefore never blocks:
 *  the PCM is opened non-blocking, an underrun is recovered from exactly once,
 *  and when the device ring is full the remaining frames are simply dropped.
 * ========================================================================== */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "gb.h"
#include "platform.h"

/* ===========================================================================
 *  Minimal ALSA ABI declarations (no <alsa/asoundlib.h> anywhere)
 * ======================================================================== */

typedef struct _snd_pcm            snd_pcm_t;
typedef struct _snd_pcm_hw_params  snd_pcm_hw_params_t;
typedef long                       snd_pcm_sframes_t;
typedef unsigned long              snd_pcm_uframes_t;

/* Part of ALSA's stable ABI — these numeric values have never changed. */
enum {
    SND_PCM_STREAM_PLAYBACK       = 0,
    SND_PCM_ACCESS_RW_INTERLEAVED = 3,
    SND_PCM_FORMAT_FLOAT_LE       = 14
};

typedef int  (*fn_pcm_open)(snd_pcm_t **, const char *, int, int);
typedef int  (*fn_pcm_close)(snd_pcm_t *);
typedef int  (*fn_hwp_malloc)(snd_pcm_hw_params_t **);
typedef void (*fn_hwp_free)(snd_pcm_hw_params_t *);
typedef int  (*fn_hwp_any)(snd_pcm_t *, snd_pcm_hw_params_t *);
typedef int  (*fn_hwp_set_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
typedef int  (*fn_hwp_set_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
typedef int  (*fn_hwp_set_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int);
typedef int  (*fn_hwp_set_rate_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *, int *);
typedef int  (*fn_hwp_set_buffer_near)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *);
typedef int  (*fn_hwp_set_period_near)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *, int *);
typedef int  (*fn_hwp_apply)(snd_pcm_t *, snd_pcm_hw_params_t *);
typedef snd_pcm_sframes_t (*fn_pcm_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
typedef int  (*fn_pcm_prepare)(snd_pcm_t *);
typedef int  (*fn_pcm_recover)(snd_pcm_t *, int, int);
typedef snd_pcm_sframes_t (*fn_pcm_avail_update)(snd_pcm_t *);
typedef int  (*fn_pcm_drop)(snd_pcm_t *);
typedef int  (*fn_pcm_nonblock)(snd_pcm_t *, int);
typedef const char *(*fn_strerror)(int);

struct AlsaApi {
    fn_pcm_open            open;
    fn_pcm_close           close;
    fn_hwp_malloc          hwp_malloc;
    fn_hwp_free            hwp_free;
    fn_hwp_any             hwp_any;
    fn_hwp_set_access      set_access;
    fn_hwp_set_format      set_format;
    fn_hwp_set_channels    set_channels;
    fn_hwp_set_rate_near   set_rate_near;
    fn_hwp_set_buffer_near set_buffer_near;
    fn_hwp_set_period_near set_period_near;
    fn_hwp_apply           apply;
    fn_pcm_writei          writei;
    fn_pcm_prepare         prepare;
    fn_pcm_recover         recover;
    fn_pcm_avail_update    avail_update;
    fn_pcm_drop            drop;
    fn_pcm_nonblock        nonblock;
    fn_strerror            strerror_fn;
};

/* ===========================================================================
 *  State
 * ======================================================================== */

#define ALSA_LIB_SONAME   "libasound.so.2"
#define ALSA_DEVICE       "default"
#define ALSA_WANT_BUFFER  4096u     /* frames of device buffer  (~85 ms @48k) */
#define ALSA_WANT_PERIOD  1024u     /* frames per period        (~21 ms @48k) */
#define ALSA_WRITE_TRIES  64        /* hard cap so a sick device cannot spin  */

static struct AlsaApi  s_api;
static void           *s_lib        = NULL;
static snd_pcm_t      *s_pcm        = NULL;
static bool            s_ok         = false;
static bool            s_warned     = false;
static int             s_channels   = 0;
static size_t          s_frame_size = 0;               /* bytes per frame     */
static snd_pcm_uframes_t s_buffer_frames = 0;

static void audio_warn(const char *what, const char *detail)
{
    if (s_warned)
        return;
    s_warned = true;
    if (detail)
        fprintf(stderr, "audio: disabled (%s: %s)\n", what, detail);
    else
        fprintf(stderr, "audio: disabled (%s)\n", what);
}

/* ===========================================================================
 *  dlsym plumbing
 *
 *  ISO C has no conversion between object and function pointers; POSIX
 *  guarantees dlsym()'s result can be carried through one, and the portable
 *  spelling of that is a union rather than a direct cast.
 * ======================================================================== */

typedef void (*generic_fn)(void);

static generic_fn dl_lookup(void *lib, const char *name)
{
    union { void *obj; generic_fn fn; } cast;

    cast.obj = dlsym(lib, name);
    return cast.fn;
}

/* Resolve one symbol; records the first failure in *missing. */
#define BIND(field, type, name)                                   \
    do {                                                          \
        generic_fn g_ = dl_lookup(s_lib, (name));                 \
        s_api.field = (type)g_;                                   \
        if (!s_api.field && !missing)                             \
            missing = (name);                                     \
    } while (0)

static bool alsa_bind_symbols(void)
{
    const char *missing = NULL;

    BIND(open,            fn_pcm_open,            "snd_pcm_open");
    BIND(close,           fn_pcm_close,           "snd_pcm_close");
    BIND(hwp_malloc,      fn_hwp_malloc,          "snd_pcm_hw_params_malloc");
    BIND(hwp_free,        fn_hwp_free,            "snd_pcm_hw_params_free");
    BIND(hwp_any,         fn_hwp_any,             "snd_pcm_hw_params_any");
    BIND(set_access,      fn_hwp_set_access,      "snd_pcm_hw_params_set_access");
    BIND(set_format,      fn_hwp_set_format,      "snd_pcm_hw_params_set_format");
    BIND(set_channels,    fn_hwp_set_channels,    "snd_pcm_hw_params_set_channels");
    BIND(set_rate_near,   fn_hwp_set_rate_near,   "snd_pcm_hw_params_set_rate_near");
    BIND(set_buffer_near, fn_hwp_set_buffer_near, "snd_pcm_hw_params_set_buffer_size_near");
    BIND(set_period_near, fn_hwp_set_period_near, "snd_pcm_hw_params_set_period_size_near");
    BIND(apply,           fn_hwp_apply,           "snd_pcm_hw_params");
    BIND(writei,          fn_pcm_writei,          "snd_pcm_writei");
    BIND(prepare,         fn_pcm_prepare,         "snd_pcm_prepare");
    BIND(recover,         fn_pcm_recover,         "snd_pcm_recover");
    BIND(avail_update,    fn_pcm_avail_update,    "snd_pcm_avail_update");
    BIND(drop,            fn_pcm_drop,            "snd_pcm_drop");
    BIND(nonblock,        fn_pcm_nonblock,        "snd_pcm_nonblock");
    BIND(strerror_fn,     fn_strerror,            "snd_strerror");

    if (missing) {
        audio_warn("missing ALSA symbol", missing);
        return false;
    }
    return true;
}

#undef BIND

/* snd_strerror() when we have it, plain strerror() otherwise. */
static const char *alsa_err(int err)
{
    if (s_api.strerror_fn) {
        const char *m = s_api.strerror_fn(err);
        if (m)
            return m;
    }
    return strerror(err < 0 ? -err : err);
}

/* ===========================================================================
 *  Public API
 * ======================================================================== */

bool audio_init(int rate, int channels)
{
    snd_pcm_hw_params_t *hw = NULL;
    snd_pcm_uframes_t    buf_frames = ALSA_WANT_BUFFER;
    snd_pcm_uframes_t    period_frames = ALSA_WANT_PERIOD;
    unsigned int         want_rate;
    int                  dir = 0;
    int                  err;

    audio_shutdown();          /* idempotent: also covers a re-init */

    if (rate < 4000)  rate = 4000;
    if (rate > 384000) rate = 384000;
    if (channels < 1) channels = 1;
    if (channels > 8) channels = 8;
    want_rate = (unsigned int)rate;

    s_lib = dlopen(ALSA_LIB_SONAME, RTLD_NOW | RTLD_LOCAL);
    if (!s_lib) {
        const char *e = dlerror();
        audio_warn("cannot load " ALSA_LIB_SONAME, e);
        return false;
    }
    if (!alsa_bind_symbols()) {
        audio_shutdown();
        return false;
    }

    err = s_api.open(&s_pcm, ALSA_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0 || !s_pcm) {
        s_pcm = NULL;
        audio_warn("cannot open PCM device", alsa_err(err));
        audio_shutdown();
        return false;
    }

    /* Never let the emulation thread block inside snd_pcm_writei(). */
    (void)s_api.nonblock(s_pcm, 1);

    if (s_api.hwp_malloc(&hw) < 0 || !hw) {
        hw = NULL;
        audio_warn("out of memory for hw params", NULL);
        audio_shutdown();
        return false;
    }

    err = s_api.hwp_any(s_pcm, hw);
    if (err >= 0)
        err = s_api.set_access(s_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err >= 0)
        err = s_api.set_format(s_pcm, hw, SND_PCM_FORMAT_FLOAT_LE);
    if (err >= 0)
        err = s_api.set_channels(s_pcm, hw, (unsigned int)channels);
    if (err >= 0)
        err = s_api.set_rate_near(s_pcm, hw, &want_rate, &dir);
    if (err >= 0) {
        dir = 0;
        err = s_api.set_buffer_near(s_pcm, hw, &buf_frames);
    }
    if (err >= 0) {
        dir = 0;
        err = s_api.set_period_near(s_pcm, hw, &period_frames, &dir);
    }
    if (err >= 0)
        err = s_api.apply(s_pcm, hw);

    s_api.hwp_free(hw);
    hw = NULL;

    if (err < 0) {
        audio_warn("unsupported PCM configuration", alsa_err(err));
        audio_shutdown();
        return false;
    }

    err = s_api.prepare(s_pcm);
    if (err < 0) {
        audio_warn("cannot prepare PCM device", alsa_err(err));
        audio_shutdown();
        return false;
    }

    if (buf_frames == 0)
        buf_frames = ALSA_WANT_BUFFER;

    s_channels      = channels;
    s_frame_size    = (size_t)channels * sizeof(float);
    s_buffer_frames = buf_frames;
    s_ok            = true;
    return true;
}

void audio_shutdown(void)
{
    if (s_pcm) {
        if (s_api.drop)
            (void)s_api.drop(s_pcm);
        if (s_api.close)
            (void)s_api.close(s_pcm);
        s_pcm = NULL;
    }
    if (s_lib) {
        dlclose(s_lib);
        s_lib = NULL;
    }
    memset(&s_api, 0, sizeof s_api);
    s_ok            = false;
    s_channels      = 0;
    s_frame_size    = 0;
    s_buffer_frames = 0;
    /* s_warned is intentionally kept: one warning per process, not per retry. */
}

void audio_push(const float *frames, int n)
{
    const unsigned char *base;
    int  done = 0;
    int  tries = 0;
    bool recovered = false;

    if (!s_ok || !s_pcm || !s_api.writei || !frames || n <= 0)
        return;
    if (s_channels <= 0 || s_frame_size == 0)
        return;

    base = (const unsigned char *)frames;

    while (done < n && tries++ < ALSA_WRITE_TRIES) {
        snd_pcm_sframes_t got = s_api.writei(s_pcm,
                                             base + (size_t)done * s_frame_size,
                                             (snd_pcm_uframes_t)(n - done));
        if (got > 0) {
            if (got > (snd_pcm_sframes_t)(n - done))
                got = (snd_pcm_sframes_t)(n - done);   /* paranoia, never true */
            done += (int)got;
            continue;
        }
        if (got == 0)
            break;                                 /* no progress: give up    */

        if (got == -EAGAIN)
            break;                                 /* ring full: drop the rest */
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        if (got == -EWOULDBLOCK)
            break;
#endif

        if (got == -EINTR)
            continue;                              /* signal: just try again   */

        if (got == -EPIPE || got == -ESTRPIPE) {
            if (recovered)
                break;                             /* retry exactly once       */
            recovered = true;
            if (s_api.recover) {
                if (s_api.recover(s_pcm, (int)got, 1) < 0)
                    break;
            } else if (s_api.prepare) {
                if (s_api.prepare(s_pcm) < 0)
                    break;
            } else {
                break;
            }
            continue;
        }

        /* Anything else (device unplugged, ENODEV, EBADFD...) is fatal for
         * this buffer; stay silent and keep the emulator running.            */
        break;
    }
}

int audio_queued(void)
{
    snd_pcm_sframes_t avail;
    long              queued;

    if (!s_ok || !s_pcm || !s_api.avail_update)
        return 0;

    avail = s_api.avail_update(s_pcm);
    if (avail < 0)
        return 0;                                  /* underrun / error        */

    queued = (long)s_buffer_frames - (long)avail;
    if (queued < 0)
        queued = 0;
    if (queued > (long)INT_MAX)
        queued = (long)INT_MAX;
    return (int)queued;
}

bool audio_ok(void)
{
    return s_ok;
}

/* CONTRACT-NOTE: none — include/platform.h was used exactly as written and the
 * five entry points above match its declarations byte for byte. For the record:
 * SND_PCM_STREAM_PLAYBACK (0), SND_PCM_ACCESS_RW_INTERLEAVED (3) and
 * SND_PCM_FORMAT_FLOAT_LE (14) are hard-coded from libasound's stable ABI
 * because the ALSA development headers are not installed on this machine, and
 * snd_pcm_hw_params_t is treated as an opaque type that only libasound itself
 * ever allocates (snd_pcm_hw_params_malloc / _free), so its layout never
 * matters to this translation unit.
 */
