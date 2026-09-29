/* =============================================================================
 *  main.c — command line front end, main loop, frame pacing, hotkeys.
 *
 *  Responsibilities:
 *    * parse the command line by hand (no getopt dependency)
 *    * build the machine, load the boot ROM / cartridge, print a summary
 *    * run either the headless loop (tests) or the windowed loop (play)
 *    * pace the windowed loop to the real Game Boy frame rate
 *      (GB_CPU_HZ / GB_FRAME_DOTS = 59.7275 Hz -> 16742 us per frame)
 *    * translate host key events into the joypad bitmask and the hotkeys
 *    * tear everything down through exactly one cleanup path, on every exit,
 *      so the battery-backed cartridge RAM is always flushed to disk
 *
 *  Memory: this file owns no heap allocations at all. The only resources it
 *  acquires are the machine (gb_create/gb_destroy), the video backend, the
 *  audio sink and an optional trace FILE*, each released exactly once in the
 *  single `cleanup:` block at the bottom of main().
 * ========================================================================== */

#include "gb.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>

/* ------------------------------------------------------------------ misc -- */

#define GB_VERSION_STR   "1.0"

/* Exactly one video frame in microseconds: 70224 / 4194304 s = 16742.7 us.
 * The integer 16742 is what the spec pins the limiter to.                   */
#define FRAME_US         16742u

/* If we ever fall further behind than this, give up on catching up and
 * resynchronise the deadline to "now" (prevents a runaway death spiral).    */
#define RESYNC_US        100000

/* Stereo frames pulled from the APU per apu_pull() call. 2048 frames =
 * 16 KiB of stack, comfortably inside the main thread's stack.              */
#define PULL_FRAMES      2048

/* Bounds for things the user can type or press. */
#define MAX_SCALE        8
#define MAX_SLOT         9
#define MAX_PALETTES     64
#define MAX_SHOTS        1000

/* ------------------------------------------------------------- signals ---- */

static volatile sig_atomic_t g_interrupted = 0;

static void on_signal(int sig)
{
    (void)sig;
    if (g_interrupted) {
        /* Second Ctrl-C: the user really means it. _Exit is async-signal
         * safe; the battery save is sacrificed at their explicit request.   */
        _Exit(130);
    }
    g_interrupted = 1;
}

/* ------------------------------------------------------------- options ---- */

typedef struct {
    const char *rom_path;
    const char *boot_path;
    const char *trace_path;
    int      scale;
    GbModel  model;
    int      palette;
    long     frames;              /* -1 = run forever                        */
    bool     headless;
    bool     serial;
    bool     break_on_serial_done;
    bool     no_audio;
    bool     turbo;
    bool     debug;
    bool     fbhash;              /* print a framebuffer SHA-1 when done     */
    const char *shot_path;        /* headless: write a PNG when done         */
    bool     memresult;           /* poll Blargg's cart-RAM result protocol  */
    bool     raw_color;           /* disable the CGB->sRGB correction        */
    const char *press;            /* headless: scripted input, "120:start,..." */
} Options;

/* --------------------------------------------------------- loop state ----- */

typedef struct {
    GB                    *gb;
    const PlatformBackend *be;
    const Options         *opt;

    u8    buttons;                /* BTN_* bitmask currently held            */
    int   slot;                   /* save-state slot 0..9                    */
    int   palette;                /* current DMG palette index               */
    int   shot;                   /* next screenshot number                  */
    bool  turbo_held;             /* PK_TURBO physically down                */
    bool  limiter_off;            /* --turbo: frame limiter permanently off  */
    bool  saved_mute;             /* apu.muted before turbo muted it         */
    bool  quit;
    bool  title_dirty;
    bool  audio_up;
    int   serial_printed;         /* bytes of gb->serial.log already echoed  */
    char  rom_title[40];
} App;

/* --------------------------------------------------- forward decls -------- */

static void  usage(FILE *out, const char *prog);
static void  print_version(void);
static bool  parse_long_str(const char *s, long *out);
static bool  eq_ci(const char *a, const char *b);
static bool  parse_args(int argc, char **argv, Options *o, int *exit_code);
static void  derive_title(const GB *gb, const char *path, char *out, size_t n);
static void  print_cart_summary(const GB *gb, const char *shown_title);
static int   serial_flush(GB *gb, int printed);
static bool  serial_log_has(const GB *gb, const char *needle);
static void  pump_audio(GB *gb, bool to_device);
static u8    key_to_button(PlatKey k);
static void  app_set_title(App *app, double fps, double pct);
static void  app_key_down(App *app, PlatKey k);
static void  app_key_up(App *app, PlatKey k);
static void  state_path(const App *app, char *out, size_t n);
static int   run_headless(App *app);
static int   run_windowed(App *app);
static void  fb_sha1_hex(const u32 *fb, int n, char *out, size_t out_sz);

/* ============================================================ usage ======= */

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
"gameboy " GB_VERSION_STR " - Game Boy / Game Boy Color emulator\n"
"\n"
"usage: %s [options] <rom.gb>\n"
"\n"
"  -s, --scale N            window scale 1..%d                  (default 4)\n"
"  -m, --model dmg|cgb|auto force the hardware model            (default auto)\n"
"  -b, --boot PATH          use a real boot ROM\n"
"  -p, --palette N          initial DMG palette index           (default 0)\n"
"  -d, --debug              drop into the debugger before the first instruction\n"
"  -t, --trace FILE         write a CPU trace to FILE (\"-\" for stdout)\n"
"      --headless           no window; run as fast as possible\n"
"      --frames N           with --headless, stop after N frames\n"
"      --serial             echo serial-port bytes to stdout as they appear\n"
"      --break-on-serial-done\n"
"                           headless: exit as soon as the serial log says\n"
"                           \"Passed\" (exit 0) or \"Failed\" (exit 1)\n"
"      --fbhash             headless: print a SHA-1 of the final framebuffer\n"
"      --screenshot PATH    headless: write the final frame to PATH as a PNG\n"
"      --raw-color          disable the CGB colour correction (raw BGR555)\n"
"      --press LIST         headless: scripted input, e.g. \"120:start,180:a,240:down\"\n"
"                           (frame:button, held for 10 frames; useful for tests)\n"
"      --memresult          headless: read the result from cart RAM instead of\n"
"                           the serial port (Blargg's \"-2\" test ROMs)\n"
"      --no-audio           disable audio\n"
"      --turbo              start with the frame limiter off\n"
"  -v, --version            print a version banner and exit\n"
"  -h, --help               print this help and exit\n"
"\n"
"controls: arrows = d-pad, X/N = A, Z/M = B, Enter = Start, Backspace = Select,\n"
"          Space = turbo, P = pause, R = reset, F1/F2 = save/load state,\n"
"          F3/F4 = slot, C = palette, 0 = mute, -/= = volume, F11 = fullscreen,\n"
"          F12 = screenshot, ` = debugger, Esc/Q = quit\n",
        prog, MAX_SCALE);
}

static void print_version(void)
{
    printf("gameboy " GB_VERSION_STR " - Game Boy (DMG) / Game Boy Color (CGB) emulator\n");
    printf("C11, no required dependencies. Built " __DATE__ " " __TIME__ ".\n");
    printf("CPU %u Hz, %d dots/frame, %.4f fps, audio %d Hz.\n",
           GB_CPU_HZ, GB_FRAME_DOTS,
           (double)GB_CPU_HZ / (double)GB_FRAME_DOTS, GB_AUDIO_RATE);
}

/* ==================================================== argument parsing === */

static bool parse_long_str(const char *s, long *out)
{
    char *end = NULL;
    long  v;

    if (!s || !*s) return false;
    errno = 0;
    v = strtol(s, &end, 0);
    if (errno != 0 || end == NULL || *end != '\0') return false;
    *out = v;
    return true;
}

static bool eq_ci(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Fetch the value for an option, either from "--opt=value" or the next argv. */
static const char *take_value(int argc, char **argv, int *idx,
                              const char *inline_val, const char *name)
{
    if (inline_val) return inline_val;
    if (*idx + 1 < argc) {
        (*idx)++;
        return argv[*idx];
    }
    fprintf(stderr, "gameboy: option '%s' requires an argument\n", name);
    return NULL;
}

/* Returns true to carry on, false to exit immediately with *exit_code. */
static bool parse_args(int argc, char **argv, Options *o, int *exit_code)
{
    const char *prog = (argc > 0 && argv[0] && argv[0][0]) ? argv[0] : "gameboy";
    bool files_only = false;
    int  ai;

    memset(o, 0, sizeof *o);
    o->scale   = 4;
    o->model   = GB_MODEL_AUTO;
    o->palette = 0;
    o->frames  = -1;

    *exit_code = 0;

    for (ai = 1; ai < argc; ai++) {
        char       *arg = argv[ai];
        const char *val = NULL;
        const char *name;
        char        namebuf[64];
        long        n;

        if (arg == NULL) continue;

        if (files_only || arg[0] != '-' || arg[1] == '\0') {
            if (o->rom_path) {
                fprintf(stderr, "gameboy: more than one ROM given ('%s' and '%s')\n",
                        o->rom_path, arg);
                usage(stderr, prog);
                *exit_code = 2;
                return false;
            }
            o->rom_path = arg;
            continue;
        }

        if (strcmp(arg, "--") == 0) { files_only = true; continue; }

        /* Split "--long=value" into name + value. */
        name = arg;
        if (arg[0] == '-' && arg[1] == '-') {
            const char *eq = strchr(arg, '=');
            if (eq) {
                size_t len = (size_t)(eq - arg);
                if (len >= sizeof namebuf) len = sizeof namebuf - 1;
                memcpy(namebuf, arg, len);
                namebuf[len] = '\0';
                name = namebuf;
                val  = eq + 1;
            }
        }

        if (!strcmp(name, "-h") || !strcmp(name, "--help")) {
            usage(stdout, prog);
            *exit_code = 0;
            return false;
        }
        if (!strcmp(name, "-v") || !strcmp(name, "--version")) {
            print_version();
            *exit_code = 0;
            return false;
        }
        if (!strcmp(name, "-s") || !strcmp(name, "--scale")) {
            const char *v = take_value(argc, argv, &ai, val, name);
            if (!v) { *exit_code = 2; return false; }
            if (!parse_long_str(v, &n) || n < 1 || n > MAX_SCALE) {
                fprintf(stderr, "gameboy: --scale must be an integer 1..%d (got '%s')\n",
                        MAX_SCALE, v);
                *exit_code = 2;
                return false;
            }
            o->scale = (int)n;
            continue;
        }
        if (!strcmp(name, "-m") || !strcmp(name, "--model")) {
            const char *v = take_value(argc, argv, &ai, val, name);
            if (!v) { *exit_code = 2; return false; }
            if (eq_ci(v, "dmg") || eq_ci(v, "gb"))
                o->model = GB_MODEL_DMG;
            else if (eq_ci(v, "cgb") || eq_ci(v, "gbc") || eq_ci(v, "color"))
                o->model = GB_MODEL_CGB;
            else if (eq_ci(v, "auto"))
                o->model = GB_MODEL_AUTO;
            else {
                fprintf(stderr, "gameboy: --model must be dmg, cgb or auto (got '%s')\n", v);
                *exit_code = 2;
                return false;
            }
            continue;
        }
        if (!strcmp(name, "-b") || !strcmp(name, "--boot")) {
            const char *v = take_value(argc, argv, &ai, val, name);
            if (!v) { *exit_code = 2; return false; }
            o->boot_path = v;
            continue;
        }
        if (!strcmp(name, "-p") || !strcmp(name, "--palette")) {
            const char *v = take_value(argc, argv, &ai, val, name);
            if (!v) { *exit_code = 2; return false; }
            if (!parse_long_str(v, &n) || n < 0) {
                fprintf(stderr, "gameboy: --palette must be a non-negative integer (got '%s')\n", v);
                *exit_code = 2;
                return false;
            }
            o->palette = (int)(n % MAX_PALETTES);
            continue;
        }
        if (!strcmp(name, "-t") || !strcmp(name, "--trace")) {
            const char *v = take_value(argc, argv, &ai, val, name);
            if (!v) { *exit_code = 2; return false; }
            o->trace_path = v;
            continue;
        }
        if (!strcmp(name, "--frames")) {
            const char *v = take_value(argc, argv, &ai, val, name);
            if (!v) { *exit_code = 2; return false; }
            if (!parse_long_str(v, &n) || n < 0) {
                fprintf(stderr, "gameboy: --frames must be a non-negative integer (got '%s')\n", v);
                *exit_code = 2;
                return false;
            }
            o->frames = n;
            continue;
        }
        if (!strcmp(name, "-d") || !strcmp(name, "--debug")) { o->debug = true;    continue; }
        if (!strcmp(name, "--headless"))                     { o->headless = true; continue; }
        if (!strcmp(name, "--serial"))                       { o->serial = true;   continue; }
        if (!strcmp(name, "--break-on-serial-done"))         { o->break_on_serial_done = true; continue; }
        if (!strcmp(name, "--fbhash"))                       { o->fbhash = true;   continue; }
        if (!strcmp(name, "--memresult"))                    { o->memresult = true; continue; }
        if (!strcmp(name, "--raw-color"))                    { o->raw_color = true; continue; }
        if (!strcmp(name, "--press")) {
            const char *v = take_value(argc, argv, &ai, val, name);
            if (!v) { *exit_code = 2; return false; }
            o->press = v;
            continue;
        }
        if (!strcmp(name, "--screenshot")) {
            const char *v = take_value(argc, argv, &ai, val, name);
            if (!v) { *exit_code = 2; return false; }
            o->shot_path = v;
            continue;
        }
        if (!strcmp(name, "--no-audio"))                     { o->no_audio = true; continue; }
        if (!strcmp(name, "--turbo"))                        { o->turbo = true;    continue; }

        fprintf(stderr, "gameboy: unknown option '%s'\n", arg);
        usage(stderr, prog);
        *exit_code = 2;
        return false;
    }

    if (!o->rom_path) {
        fprintf(stderr, "gameboy: no ROM file given\n");
        usage(stderr, prog);
        *exit_code = 2;
        return false;
    }
    return true;
}

/* ================================================= cartridge reporting === */

static void derive_title(const GB *gb, const char *path, char *out, size_t n)
{
    char   tmp[sizeof gb->cart.title + 1];
    size_t len = 0, i;

    if (!out || n == 0) return;
    out[0] = '\0';

    for (i = 0; i + 1 < sizeof gb->cart.title; i++) {
        unsigned char ch = (unsigned char)gb->cart.title[i];
        if (ch == 0) break;
        if (ch < 0x20 || ch > 0x7E) break;      /* stop at the CGB flag byte  */
        tmp[len++] = (char)ch;
    }
    while (len > 0 && (tmp[len - 1] == ' ' || tmp[len - 1] == '_')) len--;
    tmp[len] = '\0';

    if (len == 0) {
        const char *base = path ? path : "rom";
        const char *p;
        for (p = base; *p; p++)
            if (*p == '/') base = p + 1;
        snprintf(out, n, "%s", base);
    } else {
        snprintf(out, n, "%s", tmp);
    }
}

static void print_cart_summary(const GB *gb, const char *shown_title)
{
    const char *cgb;
    bool ck_ok = (gb->cart.header_checksum == gb->cart.computed_header_checksum);

    switch (gb->cart.cgb_flag) {
    case 0x80: cgb = "CGB enhanced (DMG compatible)"; break;
    case 0xC0: cgb = "CGB only";                      break;
    default:   cgb = "DMG only";                      break;
    }

    printf("-----------------------------------------------------------\n");
    printf(" title        : %s\n", shown_title);
    printf(" mapper       : %s (cart type 0x%02X)%s%s%s\n",
           gb_mbc_name(gb->cart.mbc), gb->cart.cart_type,
           gb->cart.has_battery ? " +battery" : "",
           gb->cart.has_rtc     ? " +RTC"     : "",
           gb->cart.has_rumble  ? " +rumble"  : "");
    printf(" ROM          : %lu KiB (code 0x%02X)\n",
           (unsigned long)(gb->cart.rom_size / 1024u), gb->cart.rom_size_code);
    printf(" RAM          : %lu KiB (code 0x%02X)\n",
           (unsigned long)(gb->cart.ram_size / 1024u), gb->cart.ram_size_code);
    printf(" CGB flag     : 0x%02X  %s\n", gb->cart.cgb_flag, cgb);
    printf(" header cksum : 0x%02X  %s\n",
           gb->cart.header_checksum, ck_ok ? "OK" : "MISMATCH");
    printf(" running as   : %s%s\n",
           gb->cgb_hw ? "Game Boy Color" : "Game Boy (DMG)",
           gb->cgb_mode ? ", CGB features on" : "");
    printf("-----------------------------------------------------------\n");
    fflush(stdout);
}

/* ==================================================== serial port echo === */

/* Print everything in the serial log we have not printed yet. Returns the new
 * "already printed" watermark. Every index is clamped to the buffer.        */
static int serial_flush(GB *gb, int printed)
{
    int cap = (int)sizeof gb->serial.log;
    int len = gb->serial.log_len;

    if (len < 0)   len = 0;
    if (len > cap) len = cap;
    if (printed < 0)   printed = 0;
    if (printed > len) printed = len;      /* log was reset: do not re-print  */

    if (len > printed) {
        fwrite(gb->serial.log + printed, 1, (size_t)(len - printed), stdout);
        fflush(stdout);
        printed = len;
    }
    return printed;
}

/* Bounded substring search over the (not necessarily NUL-terminated) log. */
static bool serial_log_has(const GB *gb, const char *needle)
{
    int cap = (int)sizeof gb->serial.log;
    int len = gb->serial.log_len;
    int nl  = (int)strlen(needle);
    int i;

    if (len < 0)   len = 0;
    if (len > cap) len = cap;
    if (nl == 0 || nl > len) return false;

    for (i = 0; i + nl <= len; i++)
        if (memcmp(gb->serial.log + i, needle, (size_t)nl) == 0)
            return true;
    return false;
}

/* ========================================================== audio pump === */

static void pump_audio(GB *gb, bool to_device)
{
    float buf[PULL_FRAMES * 2];
    int   avail = apu_available(gb);
    int   guard = 64;                  /* hard bound: never spin forever     */

    if (avail < 0) return;
    while (avail > 0 && guard-- > 0) {
        int want = (avail < PULL_FRAMES) ? avail : PULL_FRAMES;
        int got  = apu_pull(gb, buf, want);
        if (got <= 0) break;
        if (got > want) got = want;    /* defensive: never trust > requested */
        if (to_device) audio_push(buf, got);
        avail -= got;
    }
}

/* ============================================================== hotkeys == */

static u8 key_to_button(PlatKey k)
{
    switch (k) {
    case PK_UP:     return BTN_UP;
    case PK_DOWN:   return BTN_DOWN;
    case PK_LEFT:   return BTN_LEFT;
    case PK_RIGHT:  return BTN_RIGHT;
    case PK_A:      return BTN_A;
    case PK_B:      return BTN_B;
    case PK_START:  return BTN_START;
    case PK_SELECT: return BTN_SELECT;
    default:        return 0;
    }
}

static void state_path(const App *app, char *out, size_t n)
{
    int slot = app->slot;
    if (slot < 0) slot = 0;
    if (slot > MAX_SLOT) slot = MAX_SLOT;
    snprintf(out, n, "%s.s%d", app->opt->rom_path, slot);
}

static void app_set_title(App *app, double fps, double pct)
{
    char buf[256];

    if (!app->be || !app->be->set_title) return;
    snprintf(buf, sizeof buf,
             "gameboy - %s | slot %d | %.1f fps (%.0f%%)%s%s%s",
             app->rom_title, app->slot, fps, pct,
             app->gb->paused ? " [PAUSED]" : "",
             (app->limiter_off || app->turbo_held) ? " [TURBO]" : "",
             app->gb->apu.muted ? " [MUTE]" : "");
    app->be->set_title(buf);
}

static void app_key_down(App *app, PlatKey k)
{
    GB  *gb = app->gb;
    u8   btn = key_to_button(k);
    char path[1120];

    if (btn) {
        app->buttons |= btn;
        joypad_set_buttons(gb, app->buttons);
        return;
    }

    switch (k) {
    case PK_QUIT:
        app->quit = true;
        break;

    case PK_PAUSE:
        gb->paused = !gb->paused;
        app->title_dirty = true;
        break;

    case PK_RESET:
        gb_reset(gb);
        app->buttons = 0;
        joypad_set_buttons(gb, 0);
        printf("reset\n");
        fflush(stdout);
        break;

    case PK_TURBO:
        if (!app->turbo_held) {
            app->turbo_held = true;
            app->saved_mute = gb->apu.muted;
            gb->apu.muted   = true;
            app->title_dirty = true;
        }
        break;

    case PK_SAVE_STATE:
        state_path(app, path, sizeof path);
        if (savestate_save(gb, path)) printf("state saved   -> %s\n", path);
        else                          printf("state save FAILED (%s)\n", path);
        fflush(stdout);
        break;

    case PK_LOAD_STATE:
        state_path(app, path, sizeof path);
        if (savestate_load(gb, path)) {
            printf("state loaded  <- %s\n", path);
            app->buttons = 0;
            joypad_set_buttons(gb, 0);
        } else {
            printf("state load FAILED (%s)\n", path);
        }
        fflush(stdout);
        break;

    case PK_SLOT_NEXT:
        app->slot = (app->slot + 1) % (MAX_SLOT + 1);
        app->title_dirty = true;
        break;

    case PK_SLOT_PREV:
        app->slot = (app->slot + MAX_SLOT) % (MAX_SLOT + 1);
        app->title_dirty = true;
        break;

    case PK_PALETTE:
        app->palette = (app->palette + 1) % MAX_PALETTES;
        ppu_set_dmg_palette(gb, app->palette);
        break;

    case PK_MUTE:
        gb->apu.muted = !gb->apu.muted;
        app->saved_mute = gb->apu.muted;   /* keep turbo's snapshot in sync  */
        app->title_dirty = true;
        break;

    case PK_VOL_UP:
        gb->apu.master_volume += 0.1f;
        if (gb->apu.master_volume > 1.0f) gb->apu.master_volume = 1.0f;
        app->title_dirty = true;
        break;

    case PK_VOL_DOWN:
        gb->apu.master_volume -= 0.1f;
        if (gb->apu.master_volume < 0.0f) gb->apu.master_volume = 0.0f;
        app->title_dirty = true;
        break;

    case PK_SCREENSHOT: {
        char shot[64];
        int  tries = 0;
        for (; app->shot < MAX_SHOTS && tries < MAX_SHOTS; app->shot++, tries++) {
            FILE *probe;
            snprintf(shot, sizeof shot, "screenshot-%03d.png", app->shot);
            probe = fopen(shot, "rb");
            if (!probe) break;
            fclose(probe);
        }
        if (app->shot >= MAX_SHOTS) app->shot = 0;
        snprintf(shot, sizeof shot, "screenshot-%03d.png", app->shot);
        if (plat_write_png(shot, gb->ppu.fb, GB_SCREEN_W, GB_SCREEN_H)) {
            printf("screenshot -> %s\n", shot);
            if (app->shot < MAX_SHOTS - 1) app->shot++;
        } else {
            printf("screenshot FAILED (%s)\n", shot);
        }
        fflush(stdout);
        break;
    }

    case PK_DEBUGGER:
        debugger_repl(gb);
        app->title_dirty = true;
        break;

    case PK_FULLSCREEN:
        if (app->be && app->be->toggle_fullscreen) app->be->toggle_fullscreen();
        break;

    default:
        break;
    }
}

static void app_key_up(App *app, PlatKey k)
{
    u8 btn = key_to_button(k);

    if (btn) {
        app->buttons = (u8)(app->buttons & (u8)~btn);
        joypad_set_buttons(app->gb, app->buttons);
        return;
    }
    if (k == PK_TURBO && app->turbo_held) {
        app->turbo_held      = false;
        app->gb->apu.muted   = app->saved_mute;
        app->title_dirty     = true;
    }
}


/* --------------------------------------------------------------------------
 * Blargg's "-2" test ROMs (and several others) do not use the serial port.
 * They publish their verdict in cartridge RAM instead:
 *      $A000       status: 0x80 = still running, 0 = passed, else a failure code
 *      $A001-$A003 signature 0xDE 0xB0 0x61
 *      $A004...    a NUL-terminated ASCII report
 * Returns 1 when a final verdict is available, 0 while still running and -1
 * when the signature is absent (this ROM does not use the protocol).
 * -------------------------------------------------------------------------- */
static int memresult_poll(GB *gb, int *status)
{
    if (mmu_peek(gb, 0xA001) != 0xDE ||
        mmu_peek(gb, 0xA002) != 0xB0 ||
        mmu_peek(gb, 0xA003) != 0x61)
        return -1;
    *status = mmu_peek(gb, 0xA000);
    return (*status == 0x80) ? 0 : 1;
}

static void memresult_print(GB *gb)
{
    /* Bounded walk of the report text; cart RAM is 8 KiB at most here. */
    for (u16 a = 0xA004; a < 0xBFFF; a++) {
        u8 ch = mmu_peek(gb, a);
        if (ch == 0) break;
        fputc((ch >= 0x20 && ch < 0x7F) || ch == '\n' ? ch : '.', stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
}


/* --------------------------------------------------------------------------
 * Scripted input for headless runs: "--press 120:start,180:a".
 * Each entry presses a button at the given frame and holds it for HOLD_FRAMES.
 * This makes it possible to drive a real game to a known screen from a script,
 * which is what the screenshot regression tests use.
 * -------------------------------------------------------------------------- */
#define PRESS_MAX   64
#define HOLD_FRAMES 10

typedef struct { long frame; u8 btn; } PressEvent;

static PressEvent g_press[PRESS_MAX];
static int        g_press_n;

static u8 button_by_name(const char *s, size_t n)
{
    static const struct { const char *name; u8 bit; } tab[] = {
        { "right", BTN_RIGHT }, { "left",  BTN_LEFT   }, { "up",    BTN_UP    },
        { "down",  BTN_DOWN  }, { "a",     BTN_A      }, { "b",     BTN_B     },
        { "select",BTN_SELECT}, { "start", BTN_START  },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strlen(tab[i].name) == n && !strncasecmp(tab[i].name, s, n))
            return tab[i].bit;
    return 0;
}

/* Returns false (with a message) when the list cannot be parsed. */
static bool press_parse(const char *spec)
{
    g_press_n = 0;
    while (*spec && g_press_n < PRESS_MAX) {
        char *end;
        long frame = strtol(spec, &end, 10);
        if (end == spec || *end != ':' || frame < 0) {
            fprintf(stderr, "gameboy: --press expects frame:button, got '%s'\n", spec);
            return false;
        }
        const char *nm = end + 1;
        const char *comma = strchr(nm, ',');
        size_t len = comma ? (size_t)(comma - nm) : strlen(nm);
        u8 bit = button_by_name(nm, len);
        if (!bit) {
            fprintf(stderr, "gameboy: --press: unknown button '%.*s'\n", (int)len, nm);
            return false;
        }
        g_press[g_press_n].frame = frame;
        g_press[g_press_n].btn   = bit;
        g_press_n++;
        if (!comma) break;
        spec = comma + 1;
    }
    return true;
}

/* Recompute the held-button mask for the given frame. */
static u8 press_mask(long frame)
{
    u8 m = 0;
    for (int i = 0; i < g_press_n; i++)
        if (frame >= g_press[i].frame && frame < g_press[i].frame + HOLD_FRAMES)
            m |= g_press[i].btn;
    return m;
}

/* ======================================================= headless loop === */

static int run_headless(App *app)
{
    GB            *gb  = app->gb;
    const Options *opt = app->opt;
    int   code  = 0;
    bool  done  = false;
    u64   iters = 0;
    /* Backstop so a pathological ROM that never completes a PPU frame still
     * terminates when --frames was given.                                   */
    u64   iter_cap = (opt->frames >= 0) ? (u64)opt->frames + 4096u : 0;

    if (opt->debug) debugger_repl(gb);

    while (!g_interrupted && !gb->hit_breakpoint) {
        if (g_press_n)
            joypad_set_buttons(gb, press_mask((long)gb->ppu.frames));

        gb_run_frame(gb);
        iters++;

        if (opt->serial)
            app->serial_printed = serial_flush(gb, app->serial_printed);

        /* Keep the APU ring drained so it never sits full. */
        pump_audio(gb, false);

        if (opt->memresult) {
            int status = 0;
            int r = memresult_poll(gb, &status);
            if (r == 1) {
                memresult_print(gb);
                code = (status == 0) ? 0 : 1;
                printf("%s\n", status == 0 ? "Passed" : "Failed");
                fflush(stdout);
                done = true;
                break;
            }
        }

        if (opt->break_on_serial_done) {
            if (serial_log_has(gb, "Failed")) { code = 1; done = true; }
            else if (serial_log_has(gb, "Passed")) { code = 0; done = true; }
            if (done) {
                /* Print the whole log (serial_flush starts at 0 when the
                 * --serial live echo was not enabled).                      */
                app->serial_printed = serial_flush(gb, app->serial_printed);
                fputc('\n', stdout);
                fflush(stdout);
                break;
            }
        }

        if (opt->frames >= 0 &&
            ((u64)gb->ppu.frames >= (u64)opt->frames || iters >= iter_cap))
            break;
    }

    if (gb->hit_breakpoint) {
        /* A breakpoint in headless mode is only meaningful with -d. */
        if (opt->debug) debugger_repl(gb);
        gb->hit_breakpoint = false;
    }

    if (opt->serial)
        app->serial_printed = serial_flush(gb, app->serial_printed);

    if (opt->memresult && !done) {
        fprintf(stderr, "gameboy: stopped without a cart-RAM verdict\n");
        code = 1;
    }

    if (opt->break_on_serial_done && !opt->memresult && !done) {
        fprintf(stderr, "gameboy: stopped without a serial verdict\n");
        code = 1;
    }

    if (opt->shot_path) {
        if (plat_write_png(opt->shot_path, gb->ppu.fb, GB_SCREEN_W, GB_SCREEN_H))
            fprintf(stderr, "wrote %s\n", opt->shot_path);
        else
            fprintf(stderr, "gameboy: could not write %s\n", opt->shot_path);
    }

    if (opt->fbhash) {
        char hex[41];
        fb_sha1_hex(gb->ppu.fb, GB_SCREEN_W * GB_SCREEN_H, hex, sizeof hex);
        printf("%s\n", hex);
        fflush(stdout);
    }
    return code;
}

/* ======================================================= windowed loop === */

static int run_windowed(App *app)
{
    GB            *gb  = app->gb;
    const Options *opt = app->opt;

    u64 next_frame = plat_now_us();
    u64 stat_t0    = next_frame;
    u64 stat_cyc0  = gb->cycles;
    u32 stat_frm0  = gb->ppu.frames;
    double fps = 0.0, pct = 0.0;
    long  frames_run = 0;

    if (opt->debug) debugger_repl(gb);
    app_set_title(app, 0.0, 0.0);

    while (!app->quit && !g_interrupted) {
        PlatEvent ev;
        bool fast;
        u64  now;

        /* ---- 1. input ------------------------------------------------- */
        while (app->be->poll(&ev)) {
            switch (ev.type) {
            case PEV_QUIT:     app->quit = true;            break;
            case PEV_KEY_DOWN: app_key_down(app, ev.key);   break;
            case PEV_KEY_UP:   app_key_up(app, ev.key);     break;
            case PEV_RESIZE:   /* backend handles scaling */ break;
            case PEV_NONE:
            default:                                        break;
            }
            if (app->quit) break;
        }
        if (app->quit || g_interrupted) break;

        /* ---- 2. emulate ----------------------------------------------- */
        if (!gb->paused) {
            gb_run_frame(gb);
            frames_run++;
            if (gb->hit_breakpoint) {
                debugger_repl(gb);
                gb->hit_breakpoint = false;
                next_frame = plat_now_us();     /* the REPL blocked: resync  */
                app->title_dirty = true;
            }
        }

        /* ---- 3. video -------------------------------------------------- */
        app->be->present(gb->ppu.fb);

        /* ---- 4. audio -------------------------------------------------- */
        fast = app->limiter_off || app->turbo_held;
        pump_audio(gb, app->audio_up && !fast);

        if (opt->serial)
            app->serial_printed = serial_flush(gb, app->serial_printed);

        /* ---- 5. pacing ------------------------------------------------- */
        now = plat_now_us();
        if (fast) {
            next_frame = now;                    /* limiter off             */
        } else {
            next_frame += FRAME_US;
            if ((s64)(now - next_frame) > RESYNC_US) {
                next_frame = now;                /* too far behind: resync  */
            } else if ((s64)(next_frame - now) > 0) {
                u64 delta = next_frame - now;
                if (delta > 1000000u) delta = 1000000u;   /* clock sanity   */
                plat_sleep_us(delta);
            }
        }

        /* ---- 6. title / stats ------------------------------------------ */
        now = plat_now_us();
        if ((s64)(now - stat_t0) >= 1000000 || app->title_dirty) {
            if ((s64)(now - stat_t0) >= 1000000) {
                double secs = (double)(now - stat_t0) / 1000000.0;
                if (secs > 0.0) {
                    fps = (double)(u32)(gb->ppu.frames - stat_frm0) / secs;
                    pct = ((double)(gb->cycles - stat_cyc0) / secs)
                          / (double)GB_CPU_HZ * 100.0;
                }
                stat_t0   = now;
                stat_cyc0 = gb->cycles;
                stat_frm0 = gb->ppu.frames;
            }
            app_set_title(app, fps, pct);
            app->title_dirty = false;
        }

        if (opt->frames >= 0 && frames_run >= opt->frames) break;
    }

    if (g_interrupted)
        printf("\ninterrupted - saving and exiting\n");

    return 0;
}

/* ======================================================= framebuffer hash = */

typedef struct {
    u32    h[5];
    u8     blk[64];
    size_t used;
    u64    bits;
} Sha1;

static void sha1_compress(u32 h[5], const u8 blk[64])
{
    u32 w[80];
    u32 a, b, c, d, e;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (u32)blk[i * 4] << 24 | (u32)blk[i * 4 + 1] << 16 |
               (u32)blk[i * 4 + 2] << 8 | (u32)blk[i * 4 + 3];
    for (i = 16; i < 80; i++) {
        u32 t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (t << 1) | (t >> 31);
    }

    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        u32 f, k, tmp;
        if (i < 20)      { f = (b & c) | (~b & d);         k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
    memset(s->blk, 0, sizeof s->blk);
    s->used = 0;
    s->bits = 0;
}

static void sha1_push(Sha1 *s, u8 byte)
{
    s->blk[s->used++] = byte;           /* used is always < 64 on entry      */
    s->bits += 8;
    if (s->used == 64) {
        sha1_compress(s->h, s->blk);
        s->used = 0;
    }
}

/* Hash the framebuffer as big-endian ARGB bytes so the digest is identical on
 * every host regardless of endianness. out must hold at least 41 bytes.     */
static void fb_sha1_hex(const u32 *fb, int n, char *out, size_t out_sz)
{
    static const char hexd[] = "0123456789abcdef";
    Sha1 s;
    u64  bits;
    int  i, j;

    if (!out || out_sz < 41) return;
    out[0] = '\0';
    if (!fb || n <= 0) return;

    sha1_init(&s);
    for (i = 0; i < n; i++) {
        u32 p = fb[i];
        sha1_push(&s, (u8)(p >> 24));
        sha1_push(&s, (u8)(p >> 16));
        sha1_push(&s, (u8)(p >> 8));
        sha1_push(&s, (u8)p);
    }

    bits = s.bits;
    sha1_push(&s, 0x80);
    while (s.used != 56) sha1_push(&s, 0x00);
    for (i = 7; i >= 0; i--) s.blk[s.used++] = (u8)(bits >> (i * 8));
    sha1_compress(s.h, s.blk);
    s.used = 0;

    for (i = 0; i < 5; i++) {
        for (j = 0; j < 4; j++) {
            u8 by = (u8)(s.h[i] >> (24 - j * 8));
            out[i * 8 + j * 2]     = hexd[by >> 4];
            out[i * 8 + j * 2 + 1] = hexd[by & 0x0F];
        }
    }
    out[40] = '\0';
}

/* ================================================================ main === */

int main(int argc, char **argv)
{
    Options opt;
    App     app;
    GB     *gb        = NULL;
    const PlatformBackend *be = NULL;
    bool    video_up  = false;
    bool    audio_up  = false;
    bool    trace_own = false;       /* we opened the trace file ourselves   */
    int     exit_code = 0;
    int     parse_rc  = 0;
    char    err[256];
    char    wtitle[160];

    if (!parse_args(argc, argv, &opt, &parse_rc))
        return parse_rc;

    /* Ctrl-C (and SIGTERM) must leave through the cleanup path so battery
     * RAM is flushed. The handler only sets a flag.                        */
    if (signal(SIGINT,  on_signal) == SIG_ERR)
        fprintf(stderr, "gameboy: warning: cannot install SIGINT handler\n");
    if (signal(SIGTERM, on_signal) == SIG_ERR)
        fprintf(stderr, "gameboy: warning: cannot install SIGTERM handler\n");

    memset(&app, 0, sizeof app);

    gb = gb_create();
    if (!gb) {
        fprintf(stderr, "gameboy: out of memory creating the machine\n");
        return 1;
    }

    /* The model must be set before the ROM is loaded: gb_load_rom() resolves
     * AUTO against the cartridge's CGB flag.                               */
    gb->model = opt.model;

    if (opt.boot_path) {
        if (!gb_load_boot_rom(gb, opt.boot_path)) {
            fprintf(stderr, "gameboy: %s\n",
                    gb->last_error[0] ? gb->last_error : "cannot load boot ROM");
            exit_code = 1;
            goto cleanup;
        }
        printf("boot ROM     : %s (%lu bytes)\n",
               opt.boot_path, (unsigned long)gb->mmu.boot_size);
    }

    if (!gb_load_rom(gb, opt.rom_path)) {
        fprintf(stderr, "gameboy: %s\n",
                gb->last_error[0] ? gb->last_error : "cannot load ROM");
        exit_code = 1;
        goto cleanup;
    }

    derive_title(gb, opt.rom_path, app.rom_title, sizeof app.rom_title);
    print_cart_summary(gb, app.rom_title);

    ppu_set_dmg_palette(gb, opt.palette);
    gb->ppu.color_correct = !opt.raw_color;

    if (opt.press && !press_parse(opt.press)) {
        exit_code = 2;
        goto cleanup;
    }

    if (opt.trace_path) {
        if (strcmp(opt.trace_path, "-") == 0) {
            gb->trace_file = stdout;
        } else {
            gb->trace_file = fopen(opt.trace_path, "w");
            if (!gb->trace_file) {
                fprintf(stderr, "gameboy: cannot open trace file '%s': %s\n",
                        opt.trace_path, strerror(errno));
                exit_code = 1;
                goto cleanup;
            }
            trace_own = true;
        }
        gb->trace = true;
    }

    gb->running = true;
    gb->paused  = false;

    app.gb          = gb;
    app.opt         = &opt;
    app.slot        = 0;
    app.palette     = opt.palette;
    app.shot        = 0;
    app.limiter_off = opt.turbo;
    app.saved_mute  = gb->apu.muted;

    if (opt.headless) {
        exit_code = run_headless(&app);
        goto cleanup;
    }

    /* ---------------------------------------------------------- video --- */
    be = plat_auto_backend();
    if (!be) {
        fprintf(stderr,
            "gameboy: no video backend is available in this build.\n"
            "         Install one and rebuild:\n"
            "             sudo apt install libsdl2-dev     (preferred)\n"
            "         or  sudo apt install libx11-dev\n"
            "         then run 'make clean && make'.\n"
            "         --headless still works without any video backend.\n");
        exit_code = 1;
        goto cleanup;
    }

    snprintf(wtitle, sizeof wtitle, "gameboy - %s", app.rom_title);
    err[0] = '\0';
    if (!be->init(wtitle, opt.scale, err, sizeof err)) {
        fprintf(stderr, "gameboy: %s backend failed to start: %s\n",
                be->name ? be->name : "video", err[0] ? err : "unknown error");
        exit_code = 1;
        goto cleanup;
    }
    video_up = true;
    app.be   = be;

    /* ---------------------------------------------------------- audio --- */
    if (!opt.no_audio) {
        if (audio_init(GB_AUDIO_RATE, GB_AUDIO_CHANNELS) && audio_ok()) {
            audio_up = true;
        } else {
            fprintf(stderr, "gameboy: warning: no audio device "
                            "(install libasound2 or use --no-audio); "
                            "continuing silently\n");
            audio_shutdown();          /* release anything half-initialised  */
        }
    }
    app.audio_up = audio_up;

    printf("video: %s  audio: %s  scale: %dx\n",
           be->name ? be->name : "?", audio_up ? "on" : "off", opt.scale);
    fflush(stdout);

    exit_code = run_windowed(&app);

cleanup:
    /* One cleanup path, reached by every exit. Each resource is released
     * exactly once and only if it was actually acquired.                   */
    if (gb) {
        bool need_save = gb->cart.has_battery &&
                         gb->cart.ram != NULL && gb->cart.ram_size > 0;
        if (!cart_save_ram(gb) && need_save) {
            fprintf(stderr, "gameboy: warning: could not write battery save%s%s\n",
                    gb->cart.save_path[0] ? " " : "",
                    gb->cart.save_path[0] ? gb->cart.save_path : "");
            if (exit_code == 0) exit_code = 1;
        }
        if (gb->trace_file) {
            fflush(gb->trace_file);
            if (trace_own) fclose(gb->trace_file);
            gb->trace_file = NULL;     /* never let gb_destroy double-close  */
            gb->trace = false;
        }
    }

    if (audio_up) audio_shutdown();
    if (video_up && be && be->shutdown) be->shutdown();
    if (gb) gb_destroy(gb);

    return exit_code;
}

/* -----------------------------------------------------------------------------
 * CONTRACT-NOTE:
 *   1. tools/run_tests.sh invokes the binary with a "--fbhash" switch that the
 *      written assignment for this file does not list. It is required for the
 *      acid2 PPU conformance checks (they draw a picture instead of printing a
 *      verdict), so it is implemented here: in headless mode it prints a SHA-1
 *      of the final 160x144 framebuffer as the LAST line of stdout, which is
 *      exactly what run_tests.sh's `tail -1` consumes. No other behaviour is
 *      affected when the flag is absent.
 *   2. gb.h gives no way to query how many built-in DMG palettes ppu.c offers,
 *      so the palette index cycled by PK_PALETTE (and accepted by --palette) is
 *      wrapped at 64 here; ppu_set_dmg_palette() is expected to reduce it
 *      modulo its own table size.
 *   3. gb.h does not say whether gb_destroy() closes gb->trace_file. To make
 *      either implementation safe, main() flushes it, closes it only if main()
 *      opened it (never when it is stdout), and NULLs the field before
 *      gb_destroy() runs, so a double close is impossible.
 *   4. cart_save_ram() has no documented "nothing to save" return value, so its
 *      false result is only reported as a warning when the cartridge actually
 *      has battery-backed RAM.
 * -------------------------------------------------------------------------- */
