/* =============================================================================
 *  plat_sdl.c — SDL2 video / input backend.
 *
 *  Provides the "SDL2" PlatformBackend: an accelerated, vsynced, letterboxed
 *  160x144 streaming texture plus keyboard and game-controller input.
 *
 *  The entire translation unit collapses to a single stub returning NULL when
 *  the build was configured without SDL2, so the frontend can fall back to the
 *  X11 backend (or headless) without any #ifdef of its own.
 *
 *  All state is file-static; shutdown() is idempotent and is safe to call after
 *  a partially failed init(), which is exactly what init() does on every error
 *  path so that no SDL object is ever leaked.
 * ========================================================================== */

#ifdef GB_HAVE_SDL2

/* Our own main() lives in the frontend; stop SDL_main.h from hijacking it. */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED 1
#endif

#include "platform.h"

#include <SDL2/SDL.h>

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------- state ---- */

static SDL_Window         *s_win;
static SDL_Renderer       *s_ren;
static SDL_Texture        *s_tex;
static SDL_GameController *s_pad;
static SDL_JoystickID      s_pad_id;

static bool s_sdl_inited;     /* SDL_Init(SDL_INIT_VIDEO) succeeded            */
static bool s_pad_subsystem;  /* game-controller subsystem is up               */
static bool s_fullscreen;

/* ------------------------------------------------------------- helpers ---- */

static void sdl_set_err(char *err, size_t err_sz, const char *what)
{
    const char *why;

    if (!err || err_sz == 0)
        return;
    why = SDL_GetError();
    if (!why || !*why)
        why = "unknown SDL error";
    snprintf(err, err_sz, "SDL2: %s: %s", what, why);
}

/* Open the first usable game controller. Purely optional: failure is silent. */
static void sdl_open_controller(void)
{
    int n, i;

    if (!s_pad_subsystem || s_pad)
        return;

    n = SDL_NumJoysticks();
    if (n <= 0)
        return;

    for (i = 0; i < n; i++) {
        if (!SDL_IsGameController(i))
            continue;
        s_pad = SDL_GameControllerOpen(i);
        if (s_pad) {
            SDL_Joystick *js = SDL_GameControllerGetJoystick(s_pad);
            s_pad_id = js ? SDL_JoystickInstanceID(js) : -1;
            return;
        }
    }
}

static void sdl_close_controller(void)
{
    if (s_pad) {
        SDL_GameControllerClose(s_pad);
        s_pad = NULL;
    }
    s_pad_id = -1;
}

/* Keyboard map — kept byte-for-byte identical to the X11 backend. */
static PlatKey sdl_map_key(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_UP:        return PK_UP;
    case SDLK_DOWN:      return PK_DOWN;
    case SDLK_LEFT:      return PK_LEFT;
    case SDLK_RIGHT:     return PK_RIGHT;

    case SDLK_z:
    case SDLK_m:         return PK_B;
    case SDLK_x:
    case SDLK_n:         return PK_A;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return PK_START;
    case SDLK_BACKSPACE:
    case SDLK_RSHIFT:    return PK_SELECT;

    case SDLK_SPACE:     return PK_TURBO;
    case SDLK_p:         return PK_PAUSE;
    case SDLK_r:         return PK_RESET;

    case SDLK_F1:        return PK_SAVE_STATE;
    case SDLK_F2:        return PK_LOAD_STATE;
    case SDLK_F3:        return PK_SLOT_PREV;
    case SDLK_F4:        return PK_SLOT_NEXT;

    case SDLK_c:         return PK_PALETTE;

    case SDLK_0:
    case SDLK_KP_0:      return PK_MUTE;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:  return PK_VOL_DOWN;
    case SDLK_EQUALS:
    case SDLK_KP_PLUS:   return PK_VOL_UP;

    case SDLK_F12:       return PK_SCREENSHOT;
    case SDLK_BACKQUOTE: return PK_DEBUGGER;
    case SDLK_F11:       return PK_FULLSCREEN;

    case SDLK_ESCAPE:
    case SDLK_q:         return PK_QUIT;

    default:             return PK_NONE;
    }
}

static PlatKey sdl_map_button(Uint8 button)
{
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return PK_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return PK_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return PK_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return PK_RIGHT;
    case SDL_CONTROLLER_BUTTON_A:             return PK_A;
    case SDL_CONTROLLER_BUTTON_B:             return PK_B;
    case SDL_CONTROLLER_BUTTON_START:         return PK_START;
    case SDL_CONTROLLER_BUTTON_BACK:          return PK_SELECT;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PK_TURBO;
    default:                                  return PK_NONE;
    }
}

/* ---------------------------------------------------------- shutdown ------ */

/* Idempotent: valid before init, after a failed init and after a good one. */
static void sdl_shutdown(void)
{
    if (s_tex) {
        SDL_DestroyTexture(s_tex);
        s_tex = NULL;
    }
    if (s_ren) {
        SDL_DestroyRenderer(s_ren);
        s_ren = NULL;
    }
    if (s_win) {
        SDL_DestroyWindow(s_win);
        s_win = NULL;
    }
    sdl_close_controller();

    if (s_sdl_inited) {
        SDL_Quit();
        s_sdl_inited = false;
    }
    s_pad_subsystem = false;
    s_fullscreen    = false;
}

/* -------------------------------------------------------------- init ------ */

static bool sdl_init(const char *title, int scale, char *err, size_t err_sz)
{
    int w, h;

    /* Never leak a previous session's objects if init is called twice. */
    sdl_shutdown();

    if (err && err_sz > 0)
        err[0] = '\0';

    if (scale < 1)
        scale = 1;
    if (scale > 8)          /* 1280x1152 — plenty, and keeps the window sane  */
        scale = 8;
    if (!title || !*title)
        title = "Game Boy";

    /* Hints must be set before the objects they affect are created. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* nearest neighbour   */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

    SDL_SetMainReady();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        sdl_set_err(err, err_sz, "SDL_Init(SDL_INIT_VIDEO)");
        return false;
    }
    s_sdl_inited = true;

    /* Controllers are a bonus; a missing subsystem must not fail the build. */
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0)
        s_pad_subsystem = true;
    else
        SDL_ClearError();

    w = GB_SCREEN_W * scale;
    h = GB_SCREEN_H * scale;

    s_win = SDL_CreateWindow(title,
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w, h, SDL_WINDOW_RESIZABLE);
    if (!s_win) {
        sdl_set_err(err, err_sz, "SDL_CreateWindow");
        sdl_shutdown();
        return false;
    }

    s_ren = SDL_CreateRenderer(s_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren) {
        /* No GPU / no vsync: software rasteriser still gives a usable picture. */
        SDL_ClearError();
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_ren) {
        sdl_set_err(err, err_sz, "SDL_CreateRenderer");
        sdl_shutdown();
        return false;
    }

    /* Fixed logical size: SDL does the integer-ish scaling and letterboxing. */
    SDL_RenderSetLogicalSize(s_ren, GB_SCREEN_W, GB_SCREEN_H);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);

    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING,
                              GB_SCREEN_W, GB_SCREEN_H);
    if (!s_tex) {
        sdl_set_err(err, err_sz, "SDL_CreateTexture");
        sdl_shutdown();
        return false;
    }
    SDL_SetTextureBlendMode(s_tex, SDL_BLENDMODE_NONE);

    sdl_open_controller();

    /* Start from a clean black frame instead of whatever the GPU had. */
    SDL_RenderClear(s_ren);
    SDL_RenderPresent(s_ren);
    return true;
}

/* ----------------------------------------------------------- present ------ */

static void sdl_present(const u32 *fb)
{
    if (!s_ren || !s_tex || !fb)
        return;

    /* Pitch is one full scanline of 0xAARRGGBB pixels. */
    SDL_UpdateTexture(s_tex, NULL, fb, GB_SCREEN_W * 4);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
}

/* -------------------------------------------------------------- poll ------ */

static bool sdl_poll(PlatEvent *ev)
{
    SDL_Event e;

    if (!ev)
        return false;

    ev->type = PEV_NONE;
    ev->key  = PK_NONE;
    ev->w    = 0;
    ev->h    = 0;

    if (!s_sdl_inited)
        return false;

    memset(&e, 0, sizeof e);
    if (SDL_PollEvent(&e) == 0)
        return false;      /* queue drained — the caller stops looping        */

    switch (e.type) {
    case SDL_QUIT:
        ev->type = PEV_QUIT;
        break;

    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e.window.event == SDL_WINDOWEVENT_RESIZED) {
            ev->type = PEV_RESIZE;
            ev->w    = (int)e.window.data1;
            ev->h    = (int)e.window.data2;
        } else if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
            ev->type = PEV_QUIT;
        }
        break;

    case SDL_KEYDOWN:
        if (e.key.repeat)
            break;                       /* auto-repeat is noise to the core  */
        ev->key = sdl_map_key(e.key.keysym.sym);
        if (ev->key != PK_NONE)
            ev->type = PEV_KEY_DOWN;
        break;

    case SDL_KEYUP:
        ev->key = sdl_map_key(e.key.keysym.sym);
        if (ev->key != PK_NONE)
            ev->type = PEV_KEY_UP;
        break;

    case SDL_CONTROLLERBUTTONDOWN:
        ev->key = sdl_map_button(e.cbutton.button);
        if (ev->key != PK_NONE)
            ev->type = PEV_KEY_DOWN;
        break;

    case SDL_CONTROLLERBUTTONUP:
        ev->key = sdl_map_button(e.cbutton.button);
        if (ev->key != PK_NONE)
            ev->type = PEV_KEY_UP;
        break;

    case SDL_CONTROLLERDEVICEADDED:
        /* Hot-plug: adopt the pad only when we do not already hold one. */
        if (!s_pad)
            sdl_open_controller();
        break;

    case SDL_CONTROLLERDEVICEREMOVED:
        /* e.cdevice.which is the *instance* id here, not the device index. */
        if (s_pad && e.cdevice.which == s_pad_id)
            sdl_close_controller();
        break;

    default:
        break;             /* unmapped event: PEV_NONE, but keep draining     */
    }

    return true;
}

/* --------------------------------------------------------- set_title ------ */

static void sdl_set_title(const char *title)
{
    if (s_win && title)
        SDL_SetWindowTitle(s_win, title);
}

/* -------------------------------------------------- toggle_fullscreen ----- */

static void sdl_toggle_fullscreen(void)
{
    Uint32 flags;

    if (!s_win)
        return;

    flags = SDL_GetWindowFlags(s_win);
    s_fullscreen = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == 0;

    if (SDL_SetWindowFullscreen(s_win,
                                s_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
        SDL_ClearError();                 /* purely cosmetic; keep running    */
        s_fullscreen = !s_fullscreen;
        return;
    }
    SDL_ShowCursor(s_fullscreen ? SDL_DISABLE : SDL_ENABLE);
}

/* ------------------------------------------------------------ vtable ------ */

static const PlatformBackend s_backend = {
    .name              = "SDL2",
    .init              = sdl_init,
    .shutdown          = sdl_shutdown,
    .present           = sdl_present,
    .poll              = sdl_poll,
    .set_title         = sdl_set_title,
    .toggle_fullscreen = sdl_toggle_fullscreen
};

const PlatformBackend *plat_sdl_backend(void)
{
    return &s_backend;
}

#else  /* !GB_HAVE_SDL2 ----------------------------------------------------- */

#include "platform.h"

const PlatformBackend *plat_sdl_backend(void)
{
    return NULL;
}

#endif /* GB_HAVE_SDL2 */

/* CONTRACT-NOTE:
 *   1. The assignment listed SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"0")
 *      after renderer creation. SDL only samples that hint when the *texture*
 *      is created, and on some drivers when the renderer is created, so it is
 *      set before SDL_Init here. Setting it later is a silent no-op and would
 *      have produced blurry linear scaling.
 *   2. SDL_INIT_GAMECONTROLLER is brought up with SDL_InitSubSystem() after the
 *      required SDL_Init(SDL_INIT_VIDEO); without it SDL_NumJoysticks() always
 *      reports 0 and no SDL_CONTROLLER* events are ever delivered. Failure of
 *      that subsystem is non-fatal, exactly as specified.
 *   3. poll() returns true with type == PEV_NONE for events it does not map
 *      (mouse motion, text input, ...). platform.h documents the return value
 *      as "false when queue is empty", so the frontend's drain loop must keep
 *      iterating on those; returning false would have stalled the queue and
 *      added a frame of input latency per unmapped event.
 */
