/* =============================================================================
 *  plat_x11.c — Pure Xlib video/input backend (no SDL, no toolkit).
 *
 *  Draws the 160x144 ARGB framebuffer into a resizable window using
 *  nearest-neighbour *integer* scaling with a centred black letterbox, so the
 *  aspect ratio is never distorted.
 *
 *  The blit goes through MIT-SHM (XShmPutImage) whenever the X server lives on
 *  the same machine and the extension is available; otherwise it transparently
 *  falls back to a plain malloc'd XImage + XPutImage, which still works over a
 *  network display (just slower).
 *
 *  Everything allocated here — display, window, GC, XImage, shared memory
 *  segment, scale lookup tables — is released in x11_shutdown(), which is safe
 *  to call at any time, including after a failed init().
 * ========================================================================== */
#ifdef GB_HAVE_X11

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

/* Upper bound on a window dimension we are willing to allocate a surface for.
 * Protects against a hostile/buggy window manager asking for a 2 GiB image. */
#define X11_MAX_DIM 8192

/* --------------------------------------------------------------------------
 *  File-static state. A second init() after shutdown() must work, so every one
 *  of these is reset to its zero value by x11_shutdown().
 * ----------------------------------------------------------------------- */
static Display        *g_dpy;
static int             g_screen;
static Window          g_win;
static Window          g_root;
static GC              g_gc;
static Visual         *g_visual;
static int             g_depth;

static XImage         *g_img;            /* window-sized scaled surface       */
static XShmSegmentInfo g_shm;
static bool            g_use_shm;        /* SHM available and in use          */
static bool            g_shm_attached;   /* XShmAttach succeeded              */

static int             g_w, g_h;         /* surface size == window size       */
static int            *g_colmap;         /* g_w entries: dst x -> src x       */
static int            *g_rowmap;         /* g_h entries: dst y -> src y       */
static int             g_x0, g_x1;       /* active (non-letterbox) dst columns*/
static int             g_y0, g_y1;       /* active (non-letterbox) dst rows   */

/* Pixel conversion. g_direct is the common case: 32 bpp, host byte order,
 * masks 0x00FF0000 / 0x0000FF00 / 0x000000FF — i.e. a straight u32 copy. */
static bool            g_direct;
static bool            g_swap;           /* image byte order != host order    */
static int             g_r_shift, g_g_shift, g_b_shift;

static Atom            g_atom_wm_protocols;
static Atom            g_atom_wm_delete;
static Atom            g_atom_net_wm_state;
static Atom            g_atom_net_wm_state_fs;
static Atom            g_atom_net_wm_name;
static Atom            g_atom_utf8;

static bool            g_inited;
static char            g_surf_err[192];  /* reason the last surface failed    */

/* Set by the temporary error handler installed around XShmAttach. */
static volatile int    g_shm_error;

/* --------------------------------------------------------------------------
 *  Small helpers
 * ----------------------------------------------------------------------- */

static void x11_set_err(char *err, size_t err_sz, const char *msg)
{
    if (err && err_sz > 0) {
        snprintf(err, err_sz, "%s", msg);
    }
}

static bool x11_host_is_msb(void)
{
    union { u32 v; u8 b[4]; } probe;
    probe.v = 1u;
    return probe.b[0] == 0;
}

/* Number of trailing zero bits in a non-zero mask. */
static int x11_mask_shift(unsigned long mask)
{
    int s = 0;
    if (mask == 0) {
        return 0;
    }
    while ((mask & 1ul) == 0ul) {
        mask >>= 1;
        s++;
    }
    return s;
}

static u32 x11_bswap32(u32 v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

/* Convert 0xAARRGGBB into whatever the server's visual expects. Only used on
 * the slow path (unusual masks or a byte-swapped display). */
static u32 x11_convert(u32 argb)
{
    u32 out = ((argb >> 16) & 0xFFu) << g_r_shift;
    out |= ((argb >> 8) & 0xFFu) << g_g_shift;
    out |= (argb & 0xFFu) << g_b_shift;
    if (g_swap) {
        out = x11_bswap32(out);
    }
    return out;
}

/* Swallows the BadAccess/BadShmSeg that XShmAttach raises on a remote or
 * SHM-less display so we can fall back instead of dying. */
static int x11_shm_error_handler(Display *dpy, XErrorEvent *ev)
{
    (void)dpy;
    (void)ev;
    g_shm_error = 1;
    return 0;
}

/* --------------------------------------------------------------------------
 *  Surface (XImage) management
 * ----------------------------------------------------------------------- */

static void x11_free_surface(void)
{
    if (g_img) {
        if (g_use_shm) {
            if (g_shm_attached && g_dpy) {
                XShmDetach(g_dpy, &g_shm);
                g_shm_attached = false;
            }
            /* The pixels live in the shared segment, not in malloc'd memory:
             * hide them from XDestroyImage so it cannot free() a shmat()
             * pointer, then detach the segment ourselves. */
            g_img->data = NULL;
            XDestroyImage(g_img);
            if (g_shm.shmaddr && g_shm.shmaddr != (char *)-1) {
                shmdt(g_shm.shmaddr);
            }
            if (g_shm.shmid >= 0) {
                /* Already IPC_RMID'd on success; harmless (and correct on the
                 * failure paths) to make sure the segment is gone. */
                shmctl(g_shm.shmid, IPC_RMID, NULL);
            }
        } else {
            XDestroyImage(g_img);   /* frees g_img->data for us */
        }
        g_img = NULL;
    }
    memset(&g_shm, 0, sizeof g_shm);
    g_shm.shmid = -1;
    g_shm_attached = false;

    free(g_colmap);
    free(g_rowmap);
    g_colmap = NULL;
    g_rowmap = NULL;
    g_w = 0;
    g_h = 0;
    g_x0 = g_x1 = g_y0 = g_y1 = 0;
}

/* Plain (non-shared) XImage backed by malloc. */
static bool x11_make_image_plain(int w, int h)
{
    XImage *img;
    size_t  bytes;

    img = XCreateImage(g_dpy, g_visual, (unsigned)g_depth, ZPixmap, 0,
                       NULL, (unsigned)w, (unsigned)h, 32, 0);
    if (!img) {
        snprintf(g_surf_err, sizeof g_surf_err, "XCreateImage failed");
        return false;
    }
    if (img->bits_per_pixel != 32 || img->bytes_per_line < w * 4) {
        snprintf(g_surf_err, sizeof g_surf_err,
                 "unsupported X pixel format (%d bits per pixel at depth %d; "
                 "only 32 bpp is supported)", img->bits_per_pixel, g_depth);
        XDestroyImage(img);
        return false;
    }
    bytes = (size_t)img->bytes_per_line * (size_t)img->height;
    img->data = (char *)calloc(1, bytes);
    if (!img->data) {
        snprintf(g_surf_err, sizeof g_surf_err,
                 "out of memory allocating a %dx%d image", w, h);
        XDestroyImage(img);
        return false;
    }
    g_img     = img;
    g_use_shm = false;
    return true;
}

/* MIT-SHM XImage. Returns false (quietly) so the caller can fall back. */
static bool x11_make_image_shm(int w, int h)
{
    XImage *img;
    size_t  bytes;
    int   (*old_handler)(Display *, XErrorEvent *);
    Status ok;

    memset(&g_shm, 0, sizeof g_shm);
    g_shm.shmid = -1;

    img = XShmCreateImage(g_dpy, g_visual, (unsigned)g_depth, ZPixmap,
                          NULL, &g_shm, (unsigned)w, (unsigned)h);
    if (!img) {
        return false;
    }
    if (img->bits_per_pixel != 32 || img->bytes_per_line < w * 4) {
        XDestroyImage(img);
        return false;
    }

    bytes = (size_t)img->bytes_per_line * (size_t)img->height;
    g_shm.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    if (g_shm.shmid < 0) {
        XDestroyImage(img);
        return false;
    }
    g_shm.shmaddr = (char *)shmat(g_shm.shmid, NULL, 0);
    if (g_shm.shmaddr == (char *)-1) {
        g_shm.shmaddr = NULL;
        shmctl(g_shm.shmid, IPC_RMID, NULL);
        g_shm.shmid = -1;
        XDestroyImage(img);
        return false;
    }
    img->data      = g_shm.shmaddr;
    g_shm.readOnly = False;
    memset(g_shm.shmaddr, 0, bytes);

    g_shm_error = 0;
    XSync(g_dpy, False);
    old_handler = XSetErrorHandler(x11_shm_error_handler);
    ok = XShmAttach(g_dpy, &g_shm);
    XSync(g_dpy, False);                 /* force the error, if any, to arrive */
    XSetErrorHandler(old_handler);

    if (!ok || g_shm_error) {
        shmdt(g_shm.shmaddr);
        shmctl(g_shm.shmid, IPC_RMID, NULL);
        g_shm.shmaddr = NULL;
        g_shm.shmid   = -1;
        img->data     = NULL;
        XDestroyImage(img);
        return false;
    }

    /* Mark the segment for destruction right now: the attachment keeps it
     * alive, and it disappears automatically even if we are SIGKILLed. */
    shmctl(g_shm.shmid, IPC_RMID, NULL);
    g_shm.shmid    = -1;
    g_shm_attached = true;
    g_use_shm      = true;
    g_img          = img;
    return true;
}

/* Recompute the destination->source lookup tables for the current size. */
static void x11_rebuild_maps(void)
{
    int scale, sx, sy, x, y;
    int dst_x, dst_y, dst_w, dst_h;

    if (g_w <= 0 || g_h <= 0 || !g_colmap || !g_rowmap) {
        return;
    }

    scale = g_w / GB_SCREEN_W;
    if (g_h / GB_SCREEN_H < scale) {
        scale = g_h / GB_SCREEN_H;
    }
    if (scale < 1) {
        scale = 1;                       /* window smaller than 160x144: clip */
    }

    dst_w = GB_SCREEN_W * scale;
    dst_h = GB_SCREEN_H * scale;
    dst_x = (g_w - dst_w) / 2;
    dst_y = (g_h - dst_h) / 2;

    g_x0 = dst_x < 0 ? 0 : dst_x;
    g_x1 = dst_x + dst_w;
    if (g_x1 > g_w) g_x1 = g_w;
    if (g_x1 < g_x0) g_x1 = g_x0;

    g_y0 = dst_y < 0 ? 0 : dst_y;
    g_y1 = dst_y + dst_h;
    if (g_y1 > g_h) g_y1 = g_h;
    if (g_y1 < g_y0) g_y1 = g_y0;

    for (x = g_x0; x < g_x1; x++) {
        sx = (x - dst_x) / scale;
        if (sx < 0)              sx = 0;
        if (sx >= GB_SCREEN_W)   sx = GB_SCREEN_W - 1;
        g_colmap[x] = sx;
    }
    for (y = g_y0; y < g_y1; y++) {
        sy = (y - dst_y) / scale;
        if (sy < 0)              sy = 0;
        if (sy >= GB_SCREEN_H)   sy = GB_SCREEN_H - 1;
        g_rowmap[y] = sy;
    }
}

/* Drop the old surface and build a new one at w x h. */
static bool x11_resize(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > X11_MAX_DIM) w = X11_MAX_DIM;
    if (h > X11_MAX_DIM) h = X11_MAX_DIM;

    x11_free_surface();
    g_surf_err[0] = '\0';

    if (!x11_make_image_shm(w, h)) {
        g_use_shm = false;
        if (!x11_make_image_plain(w, h)) {
            return false;
        }
    }

    g_colmap = (int *)calloc((size_t)g_img->width,  sizeof *g_colmap);
    g_rowmap = (int *)calloc((size_t)g_img->height, sizeof *g_rowmap);
    if (!g_colmap || !g_rowmap) {
        snprintf(g_surf_err, sizeof g_surf_err,
                 "out of memory allocating scale tables");
        x11_free_surface();
        return false;
    }

    g_w = g_img->width;
    g_h = g_img->height;
    x11_rebuild_maps();
    return true;
}

/* --------------------------------------------------------------------------
 *  Keyboard mapping
 * ----------------------------------------------------------------------- */

static PlatKey x11_map_key(KeySym ks)
{
    switch (ks) {
    case XK_Up:                                  return PK_UP;
    case XK_Down:                                return PK_DOWN;
    case XK_Left:                                return PK_LEFT;
    case XK_Right:                               return PK_RIGHT;

    case XK_z: case XK_Z: case XK_m: case XK_M:  return PK_B;
    case XK_x: case XK_X: case XK_n: case XK_N:  return PK_A;

    case XK_Return: case XK_KP_Enter:            return PK_START;
    case XK_BackSpace: case XK_Shift_R:          return PK_SELECT;

    case XK_space:                               return PK_TURBO;
    case XK_p: case XK_P:                        return PK_PAUSE;
    case XK_r: case XK_R:                        return PK_RESET;

    case XK_F1:                                  return PK_SAVE_STATE;
    case XK_F2:                                  return PK_LOAD_STATE;
    case XK_F3:                                  return PK_SLOT_PREV;
    case XK_F4:                                  return PK_SLOT_NEXT;

    case XK_c: case XK_C:                        return PK_PALETTE;
    case XK_0: case XK_KP_0:                     return PK_MUTE;

    case XK_minus: case XK_underscore:
    case XK_KP_Subtract:                         return PK_VOL_DOWN;
    case XK_equal: case XK_plus:
    case XK_KP_Add:                              return PK_VOL_UP;

    case XK_F12:                                 return PK_SCREENSHOT;
    case XK_grave: case XK_asciitilde:           return PK_DEBUGGER;
    case XK_F11:                                 return PK_FULLSCREEN;

    case XK_Escape: case XK_q: case XK_Q:        return PK_QUIT;
    default:                                     return PK_NONE;
    }
}

/* --------------------------------------------------------------------------
 *  PlatformBackend implementation
 * ----------------------------------------------------------------------- */

static void x11_shutdown(void);

static bool x11_init(const char *title, int scale, char *err, size_t err_sz)
{
    XVisualInfo      vinfo;
    XSetWindowAttributes swa;
    XSizeHints      *hints;
    XClassHint      *cls;
    int              win_w, win_h;
    unsigned long    black;

    if (g_inited) {
        x11_shutdown();
    }
    memset(&g_shm, 0, sizeof g_shm);
    g_shm.shmid = -1;

    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    win_w = GB_SCREEN_W * scale;
    win_h = GB_SCREEN_H * scale;

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        x11_set_err(err, err_sz, "cannot open X display (is DISPLAY set?)");
        return false;
    }

    g_screen = DefaultScreen(g_dpy);
    g_root   = RootWindow(g_dpy, g_screen);
    g_visual = DefaultVisual(g_dpy, g_screen);
    g_depth  = DefaultDepth(g_dpy, g_screen);
    black    = BlackPixel(g_dpy, g_screen);

    if ((g_depth != 24 && g_depth != 32) ||
        !XMatchVisualInfo(g_dpy, g_screen, g_depth, TrueColor, &vinfo)) {
        char msg[192];
        snprintf(msg, sizeof msg,
                 "unsupported X visual: need a TrueColor visual at depth 24 or "
                 "32, got depth %d class %d", g_depth,
                 g_visual ? g_visual->class : -1);
        x11_set_err(err, err_sz, msg);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return false;
    }
    g_visual = vinfo.visual;
    g_depth  = vinfo.depth;

    /* Work out how to pack a pixel for this visual. */
    g_r_shift = x11_mask_shift(g_visual->red_mask);
    g_g_shift = x11_mask_shift(g_visual->green_mask);
    g_b_shift = x11_mask_shift(g_visual->blue_mask);

    memset(&swa, 0, sizeof swa);
    swa.background_pixel = black;
    swa.border_pixel     = black;
    swa.backing_store    = NotUseful;
    swa.event_mask       = KeyPressMask | KeyReleaseMask | StructureNotifyMask |
                           ExposureMask | FocusChangeMask;

    g_win = XCreateWindow(g_dpy, g_root, 0, 0,
                          (unsigned)win_w, (unsigned)win_h, 0,
                          g_depth, InputOutput, g_visual,
                          CWBackPixel | CWBorderPixel | CWEventMask |
                          CWBackingStore, &swa);
    if (!g_win) {
        x11_set_err(err, err_sz, "XCreateWindow failed");
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return false;
    }

    g_gc = XCreateGC(g_dpy, g_win, 0, NULL);
    if (!g_gc) {
        x11_set_err(err, err_sz, "XCreateGC failed");
        XDestroyWindow(g_dpy, g_win);
        g_win = 0;
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return false;
    }
    XSetForeground(g_dpy, g_gc, black);
    XSetBackground(g_dpy, g_gc, black);

    /* Atoms: close button + EWMH fullscreen + UTF-8 title. */
    g_atom_wm_protocols     = XInternAtom(g_dpy, "WM_PROTOCOLS", False);
    g_atom_wm_delete        = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    g_atom_net_wm_state     = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    g_atom_net_wm_state_fs  = XInternAtom(g_dpy, "_NET_WM_STATE_FULLSCREEN",
                                          False);
    g_atom_net_wm_name      = XInternAtom(g_dpy, "_NET_WM_NAME", False);
    g_atom_utf8             = XInternAtom(g_dpy, "UTF8_STRING", False);
    if (g_atom_wm_delete != None) {
        Atom protos[1];
        protos[0] = g_atom_wm_delete;
        XSetWMProtocols(g_dpy, g_win, protos, 1);
    }

    /* Resizable, but never smaller than one native screen. */
    hints = XAllocSizeHints();
    if (hints) {
        hints->flags      = PMinSize | PBaseSize;
        hints->min_width  = GB_SCREEN_W;
        hints->min_height = GB_SCREEN_H;
        hints->base_width  = win_w;
        hints->base_height = win_h;
        XSetWMNormalHints(g_dpy, g_win, hints);
        XFree(hints);
    }
    cls = XAllocClassHint();
    if (cls) {
        static char res_name[]  = "gameboy";
        static char res_class[] = "Gameboy";
        cls->res_name  = res_name;
        cls->res_class = res_class;
        XSetClassHint(g_dpy, g_win, cls);
        XFree(cls);
    }

    if (title) {
        XStoreName(g_dpy, g_win, title);
        XSetIconName(g_dpy, g_win, title);
        if (g_atom_net_wm_name != None && g_atom_utf8 != None) {
            XChangeProperty(g_dpy, g_win, g_atom_net_wm_name, g_atom_utf8, 8,
                            PropModeReplace,
                            (const unsigned char *)title,
                            (int)strlen(title));
        }
    }

    XMapRaised(g_dpy, g_win);
    XFlush(g_dpy);

    /* Shared memory only makes sense for a local server. */
    g_use_shm = false;
    if (XShmQueryExtension(g_dpy)) {
        g_use_shm = true;               /* x11_resize() verifies and may undo */
    }

    if (!x11_resize(win_w, win_h)) {
        char msg[256];
        snprintf(msg, sizeof msg, "cannot create the X image: %s",
                 g_surf_err[0] ? g_surf_err : "unknown error");
        x11_set_err(err, err_sz, msg);
        XFreeGC(g_dpy, g_gc);
        g_gc = NULL;
        XDestroyWindow(g_dpy, g_win);
        g_win = 0;
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return false;
    }

    g_swap = (g_img->byte_order == MSBFirst) != x11_host_is_msb();
    g_direct = (!g_swap &&
                g_visual->red_mask   == 0x00FF0000ul &&
                g_visual->green_mask == 0x0000FF00ul &&
                g_visual->blue_mask  == 0x000000FFul);

    g_inited = true;
    return true;
}

static void x11_shutdown(void)
{
    x11_free_surface();

    if (g_dpy) {
        if (g_gc) {
            XFreeGC(g_dpy, g_gc);
            g_gc = NULL;
        }
        if (g_win) {
            XDestroyWindow(g_dpy, g_win);
            g_win = 0;
        }
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
    }

    g_gc    = NULL;
    g_win   = 0;
    g_root  = 0;
    g_visual = NULL;
    g_depth  = 0;
    g_screen = 0;
    g_use_shm      = false;
    g_shm_attached = false;
    g_shm_error    = 0;
    g_direct = false;
    g_swap   = false;
    g_r_shift = g_g_shift = g_b_shift = 0;
    g_atom_wm_protocols = g_atom_wm_delete = None;
    g_atom_net_wm_state = g_atom_net_wm_state_fs = None;
    g_atom_net_wm_name  = g_atom_utf8 = None;
    g_surf_err[0] = '\0';
    g_inited = false;
    memset(&g_shm, 0, sizeof g_shm);
    g_shm.shmid = -1;
}

static void x11_present(const u32 *fb)
{
    char       *base;
    const u32  *srow;
    u32        *drow;
    u32        *prev_drow = NULL;
    int         stride, x, y, sy;
    int         prev_sy = -1;
    size_t      row_bytes;

    if (!g_dpy || !g_img || !g_img->data || !fb || !g_colmap || !g_rowmap ||
        g_w <= 0 || g_h <= 0) {
        return;
    }

    base      = g_img->data;
    stride    = g_img->bytes_per_line;
    row_bytes = (size_t)g_w * sizeof(u32);

    for (y = 0; y < g_h; y++) {
        drow = (u32 *)(void *)(base + (size_t)y * (size_t)stride);

        if (y < g_y0 || y >= g_y1) {            /* top/bottom letterbox */
            memset(drow, 0, row_bytes);
            prev_sy   = -1;
            prev_drow = NULL;
            continue;
        }

        sy = g_rowmap[y];
        if (sy == prev_sy && prev_drow) {       /* same source line: copy it */
            memcpy(drow, prev_drow, row_bytes);
            prev_drow = drow;
            continue;
        }

        if (g_x0 > 0) {
            memset(drow, 0, (size_t)g_x0 * sizeof(u32));
        }
        if (g_x1 < g_w) {
            memset(drow + g_x1, 0, (size_t)(g_w - g_x1) * sizeof(u32));
        }

        srow = fb + (size_t)sy * (size_t)GB_SCREEN_W;
        if (g_direct) {
            for (x = g_x0; x < g_x1; x++) {
                drow[x] = srow[g_colmap[x]];
            }
        } else {
            for (x = g_x0; x < g_x1; x++) {
                drow[x] = x11_convert(srow[g_colmap[x]]);
            }
        }

        prev_sy   = sy;
        prev_drow = drow;
    }

    if (g_use_shm) {
        XShmPutImage(g_dpy, g_win, g_gc, g_img, 0, 0, 0, 0,
                     (unsigned)g_w, (unsigned)g_h, False);
        XSync(g_dpy, False);
    } else {
        XPutImage(g_dpy, g_win, g_gc, g_img, 0, 0, 0, 0,
                  (unsigned)g_w, (unsigned)g_h);
    }
    XFlush(g_dpy);
}

static bool x11_poll(PlatEvent *ev)
{
    XEvent  xe;
    KeySym  ks;

    if (!ev) {
        return false;
    }
    ev->type = PEV_NONE;
    ev->key  = PK_NONE;
    ev->w    = g_w;
    ev->h    = g_h;

    if (!g_dpy || XPending(g_dpy) <= 0) {
        return false;
    }
    XNextEvent(g_dpy, &xe);

    switch (xe.type) {
    case ClientMessage:
        if (xe.xclient.message_type == g_atom_wm_protocols &&
            xe.xclient.format == 32 &&
            (Atom)xe.xclient.data.l[0] == g_atom_wm_delete) {
            ev->type = PEV_QUIT;
        }
        break;

    case DestroyNotify:
        ev->type = PEV_QUIT;
        break;

    case ConfigureNotify: {
        int nw = xe.xconfigure.width;
        int nh = xe.xconfigure.height;
        if (nw != g_w || nh != g_h) {
            if (x11_resize(nw, nh)) {
                ev->type = PEV_RESIZE;
                ev->w    = g_w;
                ev->h    = g_h;
            }
        }
        break;
    }

    case KeyPress:
        ks = XLookupKeysym(&xe.xkey, 0);
        ev->type = PEV_KEY_DOWN;
        ev->key  = x11_map_key(ks);
        break;

    case KeyRelease:
        /* X synthesises KeyRelease+KeyPress pairs for auto-repeat. If the very
         * next queued event is a KeyPress of the same key at the same instant,
         * the key was never physically released — swallow both so held buttons
         * do not stutter. */
        if (XEventsQueued(g_dpy, QueuedAfterReading) > 0) {
            XEvent peek;
            XPeekEvent(g_dpy, &peek);
            if (peek.type == KeyPress &&
                peek.xkey.time    == xe.xkey.time &&
                peek.xkey.keycode == xe.xkey.keycode) {
                XNextEvent(g_dpy, &peek);   /* drop the repeat KeyPress too */
                break;                      /* PEV_NONE; keep draining     */
            }
        }
        ks = XLookupKeysym(&xe.xkey, 0);
        ev->type = PEV_KEY_UP;
        ev->key  = x11_map_key(ks);
        break;

    case Expose:
    case FocusIn:
    case FocusOut:
    case MapNotify:
    case UnmapNotify:
    case ReparentNotify:
    default:
        break;                              /* PEV_NONE, but keep draining */
    }

    return true;
}

static void x11_set_title(const char *title)
{
    if (!g_dpy || !g_win || !title) {
        return;
    }
    XStoreName(g_dpy, g_win, title);
    XSetIconName(g_dpy, g_win, title);
    if (g_atom_net_wm_name != None && g_atom_utf8 != None) {
        XChangeProperty(g_dpy, g_win, g_atom_net_wm_name, g_atom_utf8, 8,
                        PropModeReplace, (const unsigned char *)title,
                        (int)strlen(title));
    }
    XFlush(g_dpy);
}

static void x11_toggle_fullscreen(void)
{
    XEvent xe;

    if (!g_dpy || !g_win ||
        g_atom_net_wm_state == None || g_atom_net_wm_state_fs == None) {
        return;
    }

    memset(&xe, 0, sizeof xe);
    xe.type                 = ClientMessage;
    xe.xclient.serial       = 0;
    xe.xclient.send_event   = True;
    xe.xclient.display      = g_dpy;
    xe.xclient.window       = g_win;
    xe.xclient.message_type = g_atom_net_wm_state;
    xe.xclient.format       = 32;
    xe.xclient.data.l[0]    = 2;    /* _NET_WM_STATE_TOGGLE                  */
    xe.xclient.data.l[1]    = (long)g_atom_net_wm_state_fs;
    xe.xclient.data.l[2]    = 0;
    xe.xclient.data.l[3]    = 1;    /* source indication: normal application */
    xe.xclient.data.l[4]    = 0;

    XSendEvent(g_dpy, g_root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &xe);
    XFlush(g_dpy);
}

static const PlatformBackend g_x11_backend = {
    .name              = "X11",
    .init              = x11_init,
    .shutdown          = x11_shutdown,
    .present           = x11_present,
    .poll              = x11_poll,
    .set_title         = x11_set_title,
    .toggle_fullscreen = x11_toggle_fullscreen
};

const PlatformBackend *plat_x11_backend(void)
{
    return &g_x11_backend;
}

#else  /* !GB_HAVE_X11 */

#include "platform.h"

const PlatformBackend *plat_x11_backend(void)
{
    return NULL;
}

#endif /* GB_HAVE_X11 */

/* CONTRACT-NOTE: platform.h declares no way for a backend to report the
 * *current* window size on demand, so x11_poll() fills ev->w/ev->h with the
 * live surface size on every event (not just PEV_RESIZE); callers that only
 * read them on PEV_RESIZE are unaffected.
 *
 * CONTRACT-NOTE: PlatformBackend has no "grab/release input focus" or
 * "keys cleared" notification, so a FocusOut cannot tell the frontend to
 * release every held button. Alt-Tabbing away while holding a D-pad direction
 * therefore leaves that button latched until the key is pressed and released
 * again. Fixing it properly would need a new event type (e.g. PEV_FOCUS_LOST)
 * in platform.h, which this file is not allowed to modify.
 */
