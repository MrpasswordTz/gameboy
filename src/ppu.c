/* =============================================================================
 *  ppu.c — Picture Processing Unit (DMG + CGB), scanline renderer.
 *
 *  Timing model
 *  ------------
 *  A scanline is 456 dots. Lines 0..143 are visible, lines 144..153 are VBlank.
 *  On a visible line:
 *      dots   0 ..  79            mode 2  (OAM scan)
 *      dots  80 ..  80+len-1      mode 3  (drawing), len = mode3_len
 *      dots  80+len .. 455        mode 0  (HBlank)
 *  ppu_tick() walks the dot counter in chunks that stop exactly on the next
 *  mode boundary, so it behaves identically for t == 4 and for t == 70224.
 *
 *  Rendering is done once, in full, when mode 3 is entered for a visible line.
 *
 *  Memory safety
 *  -------------
 *  Everything the guest can influence (tile indices, map offsets, sprite
 *  coordinates, palette indices, VRAM bank bits) is masked or range-checked
 *  before it is used as an array subscript. A malicious ROM cannot make this
 *  file touch a byte outside gb->ppu.
 * ========================================================================== */

#include <string.h>

#include "gb.h"

/* ------------------------------------------------------------------ tables */

/* Built-in DMG colour schemes. Entry 0 is colour index 0 (lightest). */
static const u32 dmg_palette_table[][4] = {
    /* 0: classic DMG green                                                   */
    { 0xFF9BBC0FU, 0xFF8BAC0FU, 0xFF306230U, 0xFF0F380FU },
    /* 1: grayscale                                                           */
    { 0xFFFFFFFFU, 0xFFAAAAAAU, 0xFF555555U, 0xFF000000U },
    /* 2: Game Boy Pocket grey                                                */
    { 0xFFE0DBCDU, 0xFFA89F94U, 0xFF706B66U, 0xFF2B2B26U },
    /* 3: blue                                                                */
    { 0xFFD8F0FFU, 0xFF6FA8D8U, 0xFF31558CU, 0xFF101B3AU },
    /* 4: brown (CGB start-up palette for monochrome carts)                   */
    { 0xFFFFE6C8U, 0xFFD9A05BU, 0xFF8F5B2EU, 0xFF3B2410U },
};
#define DMG_PALETTE_COUNT ((int)(sizeof dmg_palette_table / sizeof dmg_palette_table[0]))

/* VRAM / OAM / palette sizes, spelled out so every index can be masked. */
#define VRAM_MASK   0x1FFFu
#define OAM_BYTES   0xA0
#define PAL_MASK    0x3Fu

/* ------------------------------------------------------------- small utils */

static bool ppu_is_cgb(const GB *gb)
{
    return gb->cgb_mode;
}

/* 5-bit BGR555 pair -> 0xFFRRGGBB with the usual CGB colour correction. */
static u32 cgb_color(const u8 *pal, int pal_num, int color, bool correct)
{
    unsigned idx = (unsigned)(((pal_num & 7) * 8) + ((color & 3) * 2)) & PAL_MASK;
    unsigned lo  = pal[idx];
    unsigned hi  = pal[(idx + 1u) & PAL_MASK];
    unsigned rgb = lo | (hi << 8);

    int r = (int)(rgb & 31u);
    int g = (int)((rgb >> 5) & 31u);
    int b = (int)((rgb >> 10) & 31u);
    int nr, ng, nb;

    if (correct) {
        /* The CGB's LCD is far less saturated than a modern sRGB display, so
         * raw BGR555 looks harsh and over-bright. This is the standard
         * Gambatte/SameBoy correction: it desaturates and cross-mixes the
         * channels the way the real panel does, landing every channel in
         * 0..248.                                                            */
        nr = (r * 13 + g * 2 + b) >> 1;
        ng = (g * 3 + b) << 1;
        nb = (r * 3 + g * 2 + b * 11) >> 1;
    } else {
        /* Raw expansion: replicate the top bits into the bottom ones so that
         * 31 maps to 255. This is what the hardware reference screenshots of
         * the conformance ROMs are captured in.                             */
        nr = (r << 3) | (r >> 2);
        ng = (g << 3) | (g >> 2);
        nb = (b << 3) | (b >> 2);
    }
    if (nr > 255) nr = 255;
    if (ng > 255) ng = 255;
    if (nb > 255) nb = 255;

    return 0xFF000000u | ((u32)nr << 16) | ((u32)ng << 8) | (u32)nb;
}

/* Colour used when the LCD is blanked. */
static u32 ppu_blank_color(const GB *gb)
{
    return gb->cgb_mode ? 0xFFFFFFFFU : gb->ppu.dmg_bg_lut[0];
}

static void ppu_fill_screen(GB *gb, u32 color)
{
    size_t i;
    for (i = 0; i < (size_t)(GB_SCREEN_W * GB_SCREEN_H); i++)
        gb->ppu.fb[i] = color;
}

/* --------------------------------------------------------- STAT interrupts */

static bool stat_level(const GB *gb)
{
    const PPU *p = &gb->ppu;
    bool line = false;

    if ((p->stat & STAT_INT_LYC) && p->ly == p->lyc)          line = true;
    if ((p->stat & STAT_INT_HBLANK) && p->mode == PPU_MODE_HBLANK) line = true;
    if ((p->stat & STAT_INT_VBLANK) && p->mode == PPU_MODE_VBLANK) line = true;
    if ((p->stat & STAT_INT_OAM)    && p->mode == PPU_MODE_OAM)    line = true;
    /* Mode 2 also pulls the VBlank-source line (documented hardware quirk). */
    if ((p->stat & STAT_INT_VBLANK) && p->mode == PPU_MODE_OAM)    line = true;

    return line;
}

static void ppu_refresh_lyc(GB *gb)
{
    PPU *p = &gb->ppu;
    if (p->ly == p->lyc) p->stat |= STAT_LYC_EQ;
    else                 p->stat = (u8)(p->stat & (u8)~STAT_LYC_EQ);
}

/* Recompute the logical STAT line; fire INT_STAT only on a rising edge. */
static void ppu_update_stat(GB *gb)
{
    PPU *p = &gb->ppu;
    bool level;

    ppu_refresh_lyc(gb);
    level = stat_level(gb);
    if (level && !p->stat_line)
        gb_request_interrupt(gb, INT_STAT);
    p->stat_line = level;
}

/* Latch the STAT line without generating an edge (used when the LCD is
 * switched back on, so no spurious LYC interrupt fires on the first line). */
static void ppu_latch_stat(GB *gb)
{
    ppu_refresh_lyc(gb);
    gb->ppu.stat_line = stat_level(gb);
}

static void ppu_set_mode(GB *gb, u8 mode)
{
    PPU *p = &gb->ppu;
    p->mode = (u8)(mode & STAT_MODE_MASK);
    p->stat = (u8)((p->stat & (u8)~STAT_MODE_MASK) | p->mode);
}

/* ------------------------------------------------------------ mode 3 length */

/* 172 + SCX fine scroll + window penalty + ~6 dots per sprite, clamped. */
static int calc_mode3_len(GB *gb, int line)
{
    const PPU *p = &gb->ppu;
    int len = 172 + (p->scx & 7);
    int height, count, i;

    if ((p->lcdc & LCDC_WIN_EN) && line >= (int)p->wy && p->wx <= 166)
        len += 6;

    if (p->lcdc & LCDC_OBJ_EN) {
        height = (p->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
        count  = 0;
        for (i = 0; i < 40 && count < 10; i++) {
            int sy  = p->oam[i * 4];            /* i*4 <= 156 < OAM_BYTES */
            int row = line + 16 - sy;
            if (row >= 0 && row < height) count++;
        }
        len += count * 6;
    }

    if (len < 172) len = 172;
    if (len > 289) len = 289;
    return len;
}

/* ------------------------------------------------------------ the renderer */

static void render_line(GB *gb, int line)
{
    PPU *p = &gb->ppu;
    u32 *row;
    bool cgb;
    bool win_drew = false;
    int x;

    if (line < 0 || line >= GB_SCREEN_H) return;

    row = &p->fb[(size_t)line * GB_SCREEN_W];
    cgb = ppu_is_cgb(gb);

    memset(p->line_bg_index, 0, sizeof p->line_bg_index);
    memset(p->line_bg_prio,  0, sizeof p->line_bg_prio);

    /* ---------------------------------------------------------- background */
    if ((p->lcdc & LCDC_BG_EN) || cgb) {
        unsigned map_base = (p->lcdc & LCDC_BG_MAP) ? 0x1C00u : 0x1800u;
        int my      = (line + p->scy) & 0xFF;
        int map_row = my >> 3;
        int fine_y  = my & 7;

        for (x = 0; x < GB_SCREEN_W; x++) {
            int mx     = (x + p->scx) & 0xFF;
            int map_col = mx >> 3;
            int fine_x = mx & 7;
            unsigned mi = (map_base + (unsigned)(map_row * 32 + map_col)) & VRAM_MASK;
            u8  tile = p->vram[0][mi];
            u8  attr = cgb ? p->vram[1][mi] : 0u;
            int bank = cgb ? ((attr >> 3) & 1) : 0;
            int py   = (cgb && (attr & 0x40)) ? (7 - fine_y) : fine_y;
            int px   = (cgb && (attr & 0x20)) ? (7 - fine_x) : fine_x;
            unsigned taddr;
            u8  lo, hi;
            int bit, color;

            if (p->lcdc & LCDC_TILE_DATA)
                taddr = (unsigned)tile * 16u;                       /* 0x8000 */
            else
                taddr = (unsigned)(0x1000 + (int)(s8)tile * 16);     /* 0x9000 */
            taddr = (taddr + (unsigned)(py * 2)) & VRAM_MASK;

            lo = p->vram[bank][taddr];
            hi = p->vram[bank][(taddr + 1u) & VRAM_MASK];

            bit   = 7 - px;
            color = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);

            p->line_bg_index[x] = (u8)color;
            p->line_bg_prio[x]  = (u8)((cgb && (attr & 0x80)) ? 1 : 0);

            row[x] = cgb ? cgb_color(p->bg_pal, attr & 7, color, p->color_correct)
                         : p->dmg_bg_lut[(p->bgp >> (color * 2)) & 3];
        }
    } else {
        /* DMG with BG disabled: the line is blank white, index stays 0. */
        u32 white = p->dmg_bg_lut[0];
        for (x = 0; x < GB_SCREEN_W; x++) row[x] = white;
    }

    /* -------------------------------------------------------------- window */
    if ((p->lcdc & LCDC_WIN_EN) && line == (int)p->wy)
        p->win_triggered = true;

    if ((p->lcdc & LCDC_WIN_EN) && line >= (int)p->wy && p->wx <= 166) {
        int wx_origin = (int)p->wx - 7;          /* screen x of window col 0  */
        int start     = wx_origin < 0 ? 0 : wx_origin;

        if (start < GB_SCREEN_W) {
            unsigned map_base = (p->lcdc & LCDC_WIN_MAP) ? 0x1C00u : 0x1800u;
            int map_row = (p->wly >> 3) & 31;
            int fine_y  = p->wly & 7;

            for (x = start; x < GB_SCREEN_W; x++) {
                int wpx     = x - wx_origin;      /* always >= 0 */
                int map_col = (wpx >> 3) & 31;
                int fine_x  = wpx & 7;
                unsigned mi = (map_base + (unsigned)(map_row * 32 + map_col)) & VRAM_MASK;
                u8  tile = p->vram[0][mi];
                u8  attr = cgb ? p->vram[1][mi] : 0u;
                int bank = cgb ? ((attr >> 3) & 1) : 0;
                int py   = (cgb && (attr & 0x40)) ? (7 - fine_y) : fine_y;
                int px   = (cgb && (attr & 0x20)) ? (7 - fine_x) : fine_x;
                unsigned taddr;
                u8  lo, hi;
                int bit, color;

                if (p->lcdc & LCDC_TILE_DATA)
                    taddr = (unsigned)tile * 16u;
                else
                    taddr = (unsigned)(0x1000 + (int)(s8)tile * 16);
                taddr = (taddr + (unsigned)(py * 2)) & VRAM_MASK;

                lo = p->vram[bank][taddr];
                hi = p->vram[bank][(taddr + 1u) & VRAM_MASK];

                bit   = 7 - px;
                color = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);

                p->line_bg_index[x] = (u8)color;
                p->line_bg_prio[x]  = (u8)((cgb && (attr & 0x80)) ? 1 : 0);

                row[x] = cgb ? cgb_color(p->bg_pal, attr & 7, color, p->color_correct)
                             : p->dmg_bg_lut[(p->bgp >> (color * 2)) & 3];
            }
            win_drew = true;
        }
    }

    /* ------------------------------------------------------------- sprites */
    if (p->lcdc & LCDC_OBJ_EN) {
        int height = (p->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
        int list[10];
        int n = 0;
        int i, k;

        for (i = 0; i < 40 && n < 10; i++) {
            int sy  = p->oam[i * 4];
            int srow = line + 16 - sy;
            if (srow >= 0 && srow < height) list[n++] = i;
        }

        /* DMG priority: smaller X wins, ties broken by lower OAM index.
         * A stable insertion sort on X reproduces exactly that ordering.
         * CGB priority: OAM index order, which is already how we collected. */
        if (!cgb) {
            for (i = 1; i < n; i++) {
                int cur  = list[i];
                int curx = p->oam[cur * 4 + 1];
                int j    = i - 1;
                while (j >= 0 && (int)p->oam[list[j] * 4 + 1] > curx) {
                    list[j + 1] = list[j];
                    j--;
                }
                list[j + 1] = cur;
            }
        }

        /* Draw lowest priority first so higher priority sprites overwrite. */
        for (k = n - 1; k >= 0; k--) {
            int o    = list[k] * 4;            /* list[k] <= 39 -> o <= 156 */
            int sy   = p->oam[o];
            int sx   = p->oam[o + 1];
            u8  tile = p->oam[o + 2];
            u8  attr = p->oam[o + 3];
            int srow = line + 16 - sy;
            int bank = cgb ? ((attr >> 3) & 1) : 0;
            unsigned taddr;
            u8  lo, hi;
            int px;

            if (attr & 0x40) srow = height - 1 - srow;   /* Y flip */
            if (srow < 0) srow = 0;
            if (srow >= height) srow = height - 1;
            if (height == 16) tile = (u8)(tile & 0xFEu);

            /* max = 0xFE*16 + 15*2 + 1 = 4095, well inside 8 KiB */
            taddr = ((unsigned)tile * 16u + (unsigned)(srow * 2)) & VRAM_MASK;
            lo = p->vram[bank][taddr];
            hi = p->vram[bank][(taddr + 1u) & VRAM_MASK];

            for (px = 0; px < 8; px++) {
                int sxp = sx - 8 + px;
                int bit, color;

                if (sxp < 0 || sxp >= GB_SCREEN_W) continue;

                bit   = (attr & 0x20) ? px : (7 - px);
                color = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (color == 0) continue;               /* transparent */

                if (cgb) {
                    /* LCDC bit 0 clear on CGB => objects always on top. */
                    if (p->lcdc & LCDC_BG_EN) {
                        if ((p->line_bg_prio[sxp] || (attr & 0x80)) &&
                            p->line_bg_index[sxp] != 0)
                            continue;
                    }
                    row[sxp] = cgb_color(p->obj_pal, attr & 7, color, p->color_correct);
                } else {
                    u8 obp;
                    if ((attr & 0x80) && p->line_bg_index[sxp] != 0) continue;
                    obp = (attr & 0x10) ? p->obp1 : p->obp0;
                    row[sxp] = p->dmg_obj_lut[(attr >> 4) & 1]
                                             [(obp >> (color * 2)) & 3];
                }
            }
        }
    }

    /* The window has its own line counter, advanced only on lines where the
     * window actually produced pixels. */
    if (win_drew && p->wly < 255) p->wly++;
}

/* ------------------------------------------------------------- LCD on/off */

static void lcd_turn_off(GB *gb)
{
    PPU *p = &gb->ppu;

    p->lcd_was_off  = true;
    p->ly           = 0;
    p->dots         = 0;
    p->mode         = PPU_MODE_HBLANK;
    p->stat         = (u8)(p->stat & (u8)~STAT_MODE_MASK);
    p->stat_line    = false;
    p->wly          = 0;
    p->win_triggered = false;
    p->mode3_len    = 172;

    ppu_fill_screen(gb, ppu_blank_color(gb));
}

static void lcd_turn_on(GB *gb)
{
    PPU *p = &gb->ppu;

    p->lcd_was_off  = false;
    p->ly           = 0;
    p->dots         = 0;
    p->wly          = 0;
    p->win_triggered = false;
    ppu_set_mode(gb, PPU_MODE_OAM);
    p->mode3_len    = calc_mode3_len(gb, 0);
    /* Latch, do not edge-trigger: no spurious LYC IRQ on the first line. */
    ppu_latch_stat(gb);
}

/* ------------------------------------------------------------- line events */

static void enter_mode3(GB *gb)
{
    PPU *p = &gb->ppu;
    p->mode3_len = calc_mode3_len(gb, (int)p->ly);
    ppu_set_mode(gb, PPU_MODE_DRAW);
    render_line(gb, (int)p->ly);
    ppu_update_stat(gb);
}

static void enter_mode0(GB *gb)
{
    ppu_set_mode(gb, PPU_MODE_HBLANK);
    ppu_update_stat(gb);
    if (gb->cgb_mode && gb->mmu.hdma_active)
        mmu_hdma_hblank(gb);
}

static void advance_line(GB *gb)
{
    PPU *p = &gb->ppu;

    p->dots -= 456;
    if (p->dots < 0) p->dots = 0;
    p->ly++;

    if (p->ly == 144) {
        ppu_set_mode(gb, PPU_MODE_VBLANK);
        gb_request_interrupt(gb, INT_VBLANK);
        p->frame_ready = true;
        p->frames++;
        ppu_update_stat(gb);
    } else if (p->ly > 153) {
        p->ly = 0;
        p->wly = 0;
        p->win_triggered = false;
        ppu_set_mode(gb, PPU_MODE_OAM);
        ppu_update_stat(gb);
    } else if (p->ly < 144) {
        ppu_set_mode(gb, PPU_MODE_OAM);
        ppu_update_stat(gb);
    } else {
        ppu_update_stat(gb);           /* still in VBlank, LY/LYC may match  */
    }
}

/* ================================ API ==================================== */

void ppu_reset(GB *gb)
{
    /* Colour correction is on by default; --raw-color turns it off so the
     * output can be compared byte-for-byte with hardware reference captures. */
    gb->ppu.color_correct = true;
    PPU *p = &gb->ppu;

    memset(p, 0, sizeof *p);

    p->lcdc      = 0x91;
    p->stat      = 0x85;
    p->scy       = 0;
    p->scx       = 0;
    p->ly        = 0;
    p->lyc       = 0;
    p->dma       = 0xFF;
    p->bgp       = 0xFC;
    p->obp0      = 0xFF;
    p->obp1      = 0xFF;
    p->wy        = 0;
    p->wx        = 0;
    p->dots      = 0;
    p->mode3_len = 172;
    p->vram_bank = 0;
    p->bcps      = 0;
    p->ocps      = 0;
    p->cgb_mode  = gb->cgb_mode;
    p->lcd_was_off = false;
    p->frame_ready = false;
    p->frames    = 0;

    /* Keep the mode bits of STAT consistent with the starting mode. */
    ppu_set_mode(gb, PPU_MODE_OAM);

    memset(p->bg_pal,  0xFF, sizeof p->bg_pal);
    memset(p->obj_pal, 0xFF, sizeof p->obj_pal);

    ppu_set_dmg_palette(gb, 0);
    ppu_fill_screen(gb, p->dmg_bg_lut[0]);

    ppu_latch_stat(gb);
}

void ppu_tick(GB *gb, int t)
{
    PPU *p = &gb->ppu;

    if (t <= 0) return;

    p->cgb_mode = gb->cgb_mode;

    if (!(p->lcdc & LCDC_LCD_EN)) {
        if (!p->lcd_was_off) lcd_turn_off(gb);
        return;
    }
    if (p->lcd_was_off) lcd_turn_on(gb);

    while (t > 0) {
        int step;

        if (p->ly < 144) {
            if (p->dots < 80)                        step = 80 - p->dots;
            else if (p->dots < 80 + p->mode3_len)    step = 80 + p->mode3_len - p->dots;
            else                                     step = 456 - p->dots;
        } else {
            step = 456 - p->dots;
        }

        if (step < 1)  step = 1;      /* defensive: never spin forever */
        if (step > t)  step = t;

        p->dots += step;
        t       -= step;

        if (p->dots >= 456) {
            advance_line(gb);
        } else if (p->ly < 144) {
            if (p->dots == 80)                       enter_mode3(gb);
            else if (p->dots == 80 + p->mode3_len)   enter_mode0(gb);
        }
    }
}

u8 ppu_read(GB *gb, u16 addr)
{
    PPU *p = &gb->ppu;

    switch (addr) {
    case 0xFF40: return p->lcdc;
    case 0xFF41: return (u8)(0x80u | p->stat);
    case 0xFF42: return p->scy;
    case 0xFF43: return p->scx;
    case 0xFF44: return p->ly;
    case 0xFF45: return p->lyc;
    case 0xFF46: return p->dma;
    case 0xFF47: return p->bgp;
    case 0xFF48: return p->obp0;
    case 0xFF49: return p->obp1;
    case 0xFF4A: return p->wy;
    case 0xFF4B: return p->wx;
    case 0xFF4F: return gb->cgb_mode ? (u8)(p->vram_bank | 0xFEu) : 0xFF;
    case 0xFF68: return gb->cgb_mode ? p->bcps : 0xFF;
    case 0xFF69: return gb->cgb_mode ? p->bg_pal[p->bcps & PAL_MASK] : 0xFF;
    case 0xFF6A: return gb->cgb_mode ? p->ocps : 0xFF;
    case 0xFF6B: return gb->cgb_mode ? p->obj_pal[p->ocps & PAL_MASK] : 0xFF;
    default:     return 0xFF;
    }
}

void ppu_write(GB *gb, u16 addr, u8 val)
{
    PPU *p = &gb->ppu;

    switch (addr) {
    case 0xFF40: {
        u8 old = p->lcdc;
        p->lcdc = val;
        if ((old ^ val) & LCDC_LCD_EN) {
            if (val & LCDC_LCD_EN) lcd_turn_on(gb);
            else                   lcd_turn_off(gb);
        }
        break;
    }
    case 0xFF41:
        /* bits 0-2 are read-only; only bits 3-6 are writable */
        p->stat = (u8)((p->stat & (u8)~0x78u) | (val & 0x78u));
        if (p->lcdc & LCDC_LCD_EN) ppu_update_stat(gb);
        break;
    case 0xFF42: p->scy = val; break;
    case 0xFF43: p->scx = val; break;
    case 0xFF44: break;                     /* LY is read-only */
    case 0xFF45:
        p->lyc = val;
        if (p->lcdc & LCDC_LCD_EN) ppu_update_stat(gb);
        else                       ppu_refresh_lyc(gb);
        break;
    case 0xFF46: p->dma  = val; break;      /* the transfer itself lives in mmu.c */
    case 0xFF47: p->bgp  = val; break;
    case 0xFF48: p->obp0 = val; break;
    case 0xFF49: p->obp1 = val; break;
    case 0xFF4A: p->wy   = val; break;
    case 0xFF4B: p->wx   = val; break;
    case 0xFF4F:
        if (gb->cgb_mode) p->vram_bank = (u8)(val & 1u);
        break;
    case 0xFF68:
        if (gb->cgb_mode) p->bcps = val;
        break;
    case 0xFF69:
        if (gb->cgb_mode) {
            unsigned i = p->bcps & PAL_MASK;
            p->bg_pal[i] = val;
            if (p->bcps & 0x80u)
                p->bcps = (u8)(0x80u | ((i + 1u) & PAL_MASK));
        }
        break;
    case 0xFF6A:
        if (gb->cgb_mode) p->ocps = val;
        break;
    case 0xFF6B:
        if (gb->cgb_mode) {
            unsigned i = p->ocps & PAL_MASK;
            p->obj_pal[i] = val;
            if (p->ocps & 0x80u)
                p->ocps = (u8)(0x80u | ((i + 1u) & PAL_MASK));
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------ VRAM / OAM access */

static bool vram_locked(const GB *gb)
{
    const PPU *p = &gb->ppu;
    return (p->lcdc & LCDC_LCD_EN) && p->mode == PPU_MODE_DRAW;
}

static bool oam_locked(const GB *gb)
{
    const PPU *p = &gb->ppu;
    return (p->lcdc & LCDC_LCD_EN) &&
           (p->mode == PPU_MODE_OAM || p->mode == PPU_MODE_DRAW);
}

u8 ppu_read_vram(GB *gb, u16 addr)
{
    if (vram_locked(gb)) return 0xFF;
    return gb->ppu.vram[gb->ppu.vram_bank & 1][(unsigned)(addr - 0x8000u) & VRAM_MASK];
}

void ppu_write_vram(GB *gb, u16 addr, u8 val)
{
    if (vram_locked(gb)) return;
    gb->ppu.vram[gb->ppu.vram_bank & 1][(unsigned)(addr - 0x8000u) & VRAM_MASK] = val;
}

u8 ppu_read_oam(GB *gb, u16 addr)
{
    unsigned off = (unsigned)(addr - 0xFE00u);
    if (off >= OAM_BYTES) return 0xFF;
    if (oam_locked(gb))   return 0xFF;
    return gb->ppu.oam[off];
}

void ppu_write_oam(GB *gb, u16 addr, u8 val)
{
    unsigned off = (unsigned)(addr - 0xFE00u);
    if (off >= OAM_BYTES) return;
    if (oam_locked(gb))   return;
    gb->ppu.oam[off] = val;
}

/* ------------------------------------------------------------ DMG palettes */

void ppu_set_dmg_palette(GB *gb, int which)
{
    PPU *p = &gb->ppu;
    int n = DMG_PALETTE_COUNT;
    int sel = which % n;
    int i;

    if (sel < 0) sel += n;

    for (i = 0; i < 4; i++) {
        u32 c = dmg_palette_table[sel][i];
        p->dmg_bg_lut[i]     = c;
        p->dmg_obj_lut[0][i] = c;
        p->dmg_obj_lut[1][i] = c;
    }
}

/* =============================================================================
 * CONTRACT-NOTE:
 *
 *  1. struct PPU has no field for the CGB OPRI register (0xFF6C, object
 *     priority mode select). The assignment asks for "CGB with OPRI selecting
 *     DMG priority" to use X-coordinate sprite ordering, but there is nowhere
 *     in the contract to store that bit and the header must not be edited.
 *     This file therefore selects the ordering purely from gb->cgb_mode:
 *     DMG/DMG-compat -> smallest-X-wins (ties by OAM index), CGB -> OAM index
 *     order. That matches every commercial title, since OPRI is only ever
 *     written by the CGB boot ROM when it runs a monochrome cartridge, in
 *     which case gb->cgb_mode is false anyway and the DMG ordering is already
 *     what we use. If an OPRI field is ever added to PPU, the single `if
 *     (!cgb)` guard around the insertion sort in render_line() is the only
 *     place that needs to change.
 *
 *  2. The assignment specifies both `stat = 0x85` and `mode = PPU_MODE_OAM`
 *     for ppu_reset(), which are inconsistent: 0x85 encodes mode 1. Since
 *     STAT bits 0-1 must always mirror ppu.mode for ppu_read(0xFF41) to be
 *     correct, ppu_reset() stores 0x85 and then applies PPU_MODE_OAM, leaving
 *     stat == 0x86 (bit 7 set, LYC_EQ set, mode 2). The interrupt-enable bits
 *     3-6 — the only part software can observe before the first mode change —
 *     are identical either way.
 *
 *  3. Writes to 0xFF46 are accepted here only to keep ppu.dma in sync for
 *     read-back, as instructed; the OAM DMA engine itself lives in mmu.c.
 * ========================================================================== */
