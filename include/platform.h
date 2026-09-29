/* =============================================================================
 *  platform.h — Host abstraction (video / input / audio).
 *
 *  Two video backends are provided and selected at runtime:
 *      - SDL2  (compiled in only when GB_HAVE_SDL2 is defined)
 *      - X11   (compiled in only when GB_HAVE_X11 is defined)
 *  Audio uses ALSA loaded with dlopen() so no -dev package is required, and
 *  silently degrades to a null sink when unavailable.
 * ========================================================================== */
#ifndef GB_PLATFORM_H
#define GB_PLATFORM_H

#include "gb.h"

typedef enum {
    PEV_NONE = 0,
    PEV_QUIT,
    PEV_KEY_DOWN,
    PEV_KEY_UP,
    PEV_RESIZE
} PlatEventType;

/* Logical keys the frontend understands. */
typedef enum {
    PK_NONE = 0,
    PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT,
    PK_A, PK_B, PK_START, PK_SELECT,
    PK_TURBO,          /* hold: uncapped speed                                 */
    PK_PAUSE,
    PK_RESET,
    PK_SAVE_STATE, PK_LOAD_STATE,
    PK_SLOT_NEXT, PK_SLOT_PREV,
    PK_PALETTE,        /* cycle DMG palette                                    */
    PK_MUTE,
    PK_VOL_UP, PK_VOL_DOWN,
    PK_SCREENSHOT,
    PK_DEBUGGER,
    PK_FULLSCREEN,
    PK_QUIT
} PlatKey;

typedef struct {
    PlatEventType type;
    PlatKey       key;
    int           w, h;
} PlatEvent;

typedef struct PlatformBackend PlatformBackend;

struct PlatformBackend {
    const char *name;
    /* Returns false on failure; *err may be filled with a reason. */
    bool (*init)(const char *title, int scale, char *err, size_t err_sz);
    void (*shutdown)(void);
    void (*present)(const u32 *fb);            /* 160x144 0xAARRGGBB           */
    bool (*poll)(PlatEvent *ev);               /* false when queue is empty    */
    void (*set_title)(const char *title);
    void (*toggle_fullscreen)(void);
};

/* Backend getters — return NULL when the backend was not compiled in. */
const PlatformBackend *plat_sdl_backend(void);
const PlatformBackend *plat_x11_backend(void);
/* Picks the best available backend (SDL2 first, then X11). */
const PlatformBackend *plat_auto_backend(void);

/* ----- Audio sink (ALSA via dlopen, or null) ------------------------------ */
bool audio_init(int rate, int channels);
void audio_shutdown(void);
/* Push interleaved stereo float frames. Non-blocking; drops on overrun. */
void audio_push(const float *frames, int n);
/* Approximate number of frames still queued in the device. */
int  audio_queued(void);
bool audio_ok(void);

/* ----- misc host helpers -------------------------------------------------- */
void plat_sleep_us(u64 us);
u64  plat_now_us(void);
bool plat_write_png(const char *path, const u32 *fb, int w, int h);

#endif /* GB_PLATFORM_H */
