/* =============================================================================
 *  mmu.c — Memory bus, I/O dispatch, OAM DMA and CGB HDMA/GDMA.
 *
 *  The whole address map lives in mmu_peek()/mmu_poke(); those two never tick
 *  the machine. mmu_read()/mmu_write() are the CPU-facing wrappers: they burn
 *  one M-cycle (gb_tick(gb, 4)) BEFORE performing the access, which is what
 *  makes Blargg's mem_timing line up. Every other consumer (PPU, DMA engines,
 *  debugger, save states) must use peek/poke so nothing re-enters gb_tick().
 *
 *  Memory map
 *  ----------
 *    0000-3FFF  cart ROM bank 0          (boot ROM overlay while enabled)
 *    4000-7FFF  cart ROM bank N
 *    8000-9FFF  VRAM (bank 0/1 on CGB)
 *    A000-BFFF  cart RAM / RTC
 *    C000-CFFF  WRAM bank 0
 *    D000-DFFF  WRAM bank 1..7 (SVBK)
 *    E000-FDFF  echo of C000-DDFF
 *    FE00-FE9F  OAM
 *    FEA0-FEFF  unusable
 *    FF00-FF7F  I/O
 *    FF80-FFFE  HRAM
 *    FFFF       IE
 *
 *  Every index derived from guest-controlled data is masked or bounds-checked
 *  before it touches an array: guest ROMs are untrusted input.
 * ========================================================================== */

#include <string.h>

#include "gb.h"

/* ---------------------------------------------------------------- limits -- */
#define WRAM_BANK_SIZE   0x1000u
#define WRAM_BANK_MASK   (WRAM_BANK_SIZE - 1u)   /* 0x0FFF */
#define WRAM_BANK_COUNT  8u
#define HRAM_SIZE        0x7Fu                   /* FF80..FFFE               */
#define OAM_SIZE         0xA0u
#define OAM_DMA_BYTES    160
#define OAM_DMA_PERIOD   4                       /* T-cycles per byte        */
#define OAM_DMA_DELAY    8                       /* T-cycles before byte 0   */
#define HDMA_BLOCK       16                      /* bytes per HBlank block   */

/* T-cycles consumed by one 16-byte HDMA/GDMA block. Doubles in CGB double
 * speed because the copy engine runs off the (unchanged) system clock while
 * gb_tick() is fed CPU-speed T-cycles. */
#define HDMA_BLOCK_TICKS(gb)  ((gb)->speed == 2 ? 64 : 32)

/* ------------------------------------------------------------- boot ROM --- */
/*
 * Returns true and stores the byte in *out when the boot ROM currently covers
 * `addr`. DMG images are 256 bytes and cover 0000-00FF. CGB images cover
 * 0000-00FF and 0200-08FF (0100-01FF is always the cartridge header). Two CGB
 * dump layouts exist in the wild: the 2304-byte image that contains the header
 * hole verbatim, and the 2048-byte image with the hole stripped out — both are
 * handled, and every offset is bounds-checked against boot_size.
 */
static bool boot_rom_byte(const MMU *m, u16 addr, u8 *out)
{
    size_t off;

    if (!m->boot_active || m->boot == NULL || m->boot_size == 0)
        return false;

    if (addr < 0x0100) {
        off = (size_t)addr;
    } else if (m->boot_size > 0x0100 && addr >= 0x0200 && addr < 0x0900) {
        /* 0x900+ byte dump: flat addressing. 0x800 byte dump: hole removed. */
        off = (m->boot_size >= 0x0900) ? (size_t)addr : (size_t)(addr - 0x0100);
    } else {
        return false;
    }

    if (off >= m->boot_size)
        return false;

    *out = m->boot[off];
    return true;
}

/* ----------------------------------------------------------------- WRAM --- */
/* `addr` must already be normalised into C000-DFFF. Always in range. */
static u8 *wram_cell(GB *gb, u16 addr)
{
    unsigned bank;

    if (addr < 0xD000) {
        bank = 0u;
    } else {
        bank = gb->cgb_mode ? (unsigned)(gb->mmu.wram_bank & 0x07u) : 1u;
        if (bank == 0u)
            bank = 1u;            /* SVBK 0 aliases bank 1 */
        if (bank >= WRAM_BANK_COUNT)
            bank = WRAM_BANK_COUNT - 1u;   /* belt and braces */
    }
    return &gb->mmu.wram[bank][addr & WRAM_BANK_MASK];
}

/* ----------------------------------------------------------------- HRAM --- */
/* `addr` must already be inside FF80-FFFE; the clamp is a safety net only. */
static u8 *hram_cell(GB *gb, u16 addr)
{
    unsigned idx = (unsigned)(addr - 0xFF80u);

    if (idx >= HRAM_SIZE)
        idx = HRAM_SIZE - 1u;
    return &gb->mmu.hram[idx];
}

/* ------------------------------------------------------------- OAM DMA ---- */
/* The source bus mirrors E000-FFFF down into C000-DFFF. */
static u16 oam_dma_src_fixup(u16 src)
{
    return (src >= 0xE000) ? (u16)(src - 0x2000) : src;
}

/* -------------------------------------------------------------- CGB DMA --- */
/* Copy one 16-byte block src -> VRAM, advancing both pointers. Destination is
 * always forced back into 8000-9FFF so a runaway length can never escape. */
static void hdma_copy_block(GB *gb)
{
    MMU *m = &gb->mmu;
    int i;

    for (i = 0; i < HDMA_BLOCK; i++) {
        u8 b = mmu_peek(gb, m->hdma_src);
        ppu_write_vram(gb, m->hdma_dst, b);
        m->hdma_src = (u16)(m->hdma_src + 1u);
        m->hdma_dst = (u16)(0x8000u | ((u16)(m->hdma_dst + 1u) & 0x1FFFu));
    }
}

/* ============================================================== reset ===== */
void mmu_reset(GB *gb)
{
    MMU *m;

    if (gb == NULL)
        return;
    m = &gb->mmu;

    memset(m->wram, 0, sizeof m->wram);
    memset(m->hram, 0, sizeof m->hram);

    m->wram_bank = 1;
    m->ie        = 0x00;
    m->ifr       = 0xE1;

    /* gb.c owns m->boot / m->boot_size — never touched here. */
    m->boot_active = (m->boot != NULL && m->boot_size != 0);

    m->dma_active      = false;
    m->dma_src         = 0;
    m->dma_pos         = 0;
    m->dma_timer       = 0;
    m->dma_start_delay = 0;

    m->hdma_src         = 0;
    m->hdma_dst         = 0x8000;
    m->hdma_len         = 0;
    m->hdma_active      = false;
    m->hdma_hblank_done = false;

    m->ff72 = 0x00;
    m->ff73 = 0x00;
    m->ff74 = 0x00;
    m->ff75 = 0x00;
}

/* ============================================================ bus core ==== */
u8 mmu_peek(GB *gb, u16 addr)
{
    MMU *m = &gb->mmu;

    switch (addr >> 12) {
    case 0x0:                                   /* 0000-0FFF: boot + ROM 0   */
        if (addr < 0x0900) {
            u8 b;
            if (boot_rom_byte(m, addr, &b))
                return b;
        }
        return cart_read_rom(gb, addr);

    case 0x1: case 0x2: case 0x3:               /* 1000-3FFF: ROM bank 0     */
    case 0x4: case 0x5: case 0x6: case 0x7:     /* 4000-7FFF: ROM bank N     */
        return cart_read_rom(gb, addr);

    case 0x8: case 0x9:                         /* 8000-9FFF: VRAM           */
        return ppu_read_vram(gb, addr);

    case 0xA: case 0xB:                         /* A000-BFFF: cart RAM       */
        return cart_read_ram(gb, addr);

    case 0xC: case 0xD:                         /* C000-DFFF: WRAM           */
        return *wram_cell(gb, addr);

    case 0xE:                                   /* E000-EFFF: echo           */
        return *wram_cell(gb, (u16)(addr - 0x2000));

    case 0xF:
        if (addr < 0xFE00)                      /* F000-FDFF: echo           */
            return *wram_cell(gb, (u16)(addr - 0x2000));
        if (addr < 0xFEA0)                      /* FE00-FE9F: OAM            */
            return ppu_read_oam(gb, addr);
        if (addr < 0xFF00)                      /* FEA0-FEFF: unusable       */
            return gb->cgb_hw ? 0x00 : 0xFF;
        if (addr < 0xFF80)                      /* FF00-FF7F: I/O            */
            return mmu_read_io(gb, addr);
        if (addr < 0xFFFF)                      /* FF80-FFFE: HRAM           */
            return *hram_cell(gb, addr);
        return m->ie;                           /* FFFF: IE                  */

    default:
        break;
    }
    return 0xFF;
}

void mmu_poke(GB *gb, u16 addr, u8 val)
{
    MMU *m = &gb->mmu;

    switch (addr >> 12) {
    case 0x0: case 0x1: case 0x2: case 0x3:     /* MBC control registers     */
    case 0x4: case 0x5: case 0x6: case 0x7:
        cart_write_rom(gb, addr, val);
        return;

    case 0x8: case 0x9:
        ppu_write_vram(gb, addr, val);
        return;

    case 0xA: case 0xB:
        cart_write_ram(gb, addr, val);
        return;

    case 0xC: case 0xD:
        *wram_cell(gb, addr) = val;
        return;

    case 0xE:
        *wram_cell(gb, (u16)(addr - 0x2000)) = val;
        return;

    case 0xF:
        if (addr < 0xFE00) {
            *wram_cell(gb, (u16)(addr - 0x2000)) = val;
        } else if (addr < 0xFEA0) {
            ppu_write_oam(gb, addr, val);
        } else if (addr < 0xFF00) {
            /* unusable — writes are dropped on the floor */
        } else if (addr < 0xFF80) {
            mmu_write_io(gb, addr, val);
        } else if (addr < 0xFFFF) {
            *hram_cell(gb, addr) = val;
        } else {
            m->ie = val;
        }
        return;

    default:
        return;
    }
}

u8 mmu_read(GB *gb, u16 addr)
{
    gb_tick(gb, 4);                 /* the access costs one M-cycle, first */
    return mmu_peek(gb, addr);
}

void mmu_write(GB *gb, u16 addr, u8 val)
{
    gb_tick(gb, 4);
    mmu_poke(gb, addr, val);
}

/* ============================================================ I/O read ==== */
u8 mmu_read_io(GB *gb, u16 addr)
{
    MMU *m = &gb->mmu;

    /* FF10-FF3F: APU registers + wave RAM. */
    if (addr >= 0xFF10 && addr <= 0xFF3F)
        return apu_read(gb, addr);

    switch (addr) {
    case 0xFF00:                                          /* P1 / JOYP      */
        return joypad_read(gb);

    case 0xFF01: case 0xFF02:                             /* SB / SC        */
        return serial_read(gb, addr);

    case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:   /* DIV TIMA TMA TAC */
        return timer_read(gb, addr);

    case 0xFF0F:                                          /* IF             */
        return (u8)(m->ifr | 0xE0);

    case 0xFF40: case 0xFF41: case 0xFF42: case 0xFF43:   /* LCDC..LY       */
    case 0xFF44: case 0xFF45:
    case 0xFF47: case 0xFF48: case 0xFF49:                /* BGP OBP0 OBP1  */
    case 0xFF4A: case 0xFF4B:                             /* WY WX          */
        return ppu_read(gb, addr);

    case 0xFF46:                                          /* DMA            */
        return gb->ppu.dma;

    case 0xFF4D:                                          /* KEY1           */
        if (!gb->cgb_mode)
            return 0xFF;
        return (u8)((gb->speed == 2 ? 0x80u : 0x00u) |
                    (gb->speed_switch ? 0x01u : 0x00u) | 0x7Eu);

    case 0xFF4F:                                          /* VBK            */
        if (!gb->cgb_mode)
            return 0xFF;
        return (u8)((gb->ppu.vram_bank & 0x01u) | 0xFEu);

    case 0xFF50:                                          /* boot disable   */
        return 0xFF;

    case 0xFF51: case 0xFF52: case 0xFF53: case 0xFF54:   /* HDMA1-4 (WO)   */
        return 0xFF;

    case 0xFF55:                                          /* HDMA5          */
        if (!gb->cgb_mode)
            return 0xFF;
        if (m->hdma_active)
            return (u8)((((unsigned)m->hdma_len / HDMA_BLOCK) - 1u) & 0x7Fu);
        if (m->hdma_len != 0)   /* stopped by a bit7=0 write, bytes remain  */
            return (u8)(0x80u | (((((unsigned)m->hdma_len / HDMA_BLOCK) - 1u)) & 0x7Fu));
        return 0xFF;            /* completed / idle                         */

    case 0xFF56:                                          /* RP (IR port)   */
        return 0xFF;

    case 0xFF68: case 0xFF69: case 0xFF6A: case 0xFF6B:   /* BCPS/BCPD/OCPS/OCPD */
        return gb->cgb_mode ? ppu_read(gb, addr) : 0xFF;

    case 0xFF6C:                                          /* OPRI           */
        return gb->cgb_mode ? 0xFE : 0xFF;

    case 0xFF70:                                          /* SVBK           */
        if (!gb->cgb_mode)
            return 0xFF;
        return (u8)((m->wram_bank & 0x07u) | 0xF8u);

    case 0xFF72: return m->ff72;
    case 0xFF73: return m->ff73;
    case 0xFF74: return m->ff74;
    case 0xFF75: return (u8)(m->ff75 | 0x8Fu);

    default:
        return 0xFF;
    }
}

/* =========================================================== I/O write ==== */
void mmu_write_io(GB *gb, u16 addr, u8 val)
{
    MMU *m = &gb->mmu;

    if (addr >= 0xFF10 && addr <= 0xFF3F) {
        apu_write(gb, addr, val);
        return;
    }

    switch (addr) {
    case 0xFF00:
        joypad_write(gb, val);
        return;

    case 0xFF01: case 0xFF02:
        serial_write(gb, addr, val);
        return;

    case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
        timer_write(gb, addr, val);
        return;

    case 0xFF0F:
        m->ifr = (u8)(val & 0x1Fu);
        return;

    case 0xFF40: case 0xFF41: case 0xFF42: case 0xFF43:
    case 0xFF44: case 0xFF45:
    case 0xFF47: case 0xFF48: case 0xFF49:
    case 0xFF4A: case 0xFF4B:
        ppu_write(gb, addr, val);
        return;

    case 0xFF46:                                          /* OAM DMA start  */
        gb->ppu.dma         = val;
        m->dma_src          = (u16)((unsigned)val << 8);
        m->dma_pos          = 0;
        m->dma_timer        = 0;
        m->dma_start_delay  = OAM_DMA_DELAY;
        m->dma_active       = true;
        return;

    case 0xFF4D:                                          /* KEY1           */
        if (gb->cgb_mode)
            gb->speed_switch = (val & 0x01u) != 0;
        return;

    case 0xFF4F:                                          /* VBK            */
        if (gb->cgb_mode) {
            ppu_write(gb, addr, val);
            /* The PPU owns vram_bank; forcing the (identical) documented
             * value afterwards keeps banking correct regardless of how far
             * ppu_write()'s own address decode reaches. Idempotent. */
            gb->ppu.vram_bank = (u8)(val & 0x01u);
        }
        return;

    case 0xFF50:                                          /* boot disable   */
        m->boot_active = false;
        return;

    case 0xFF51:                                          /* HDMA1 src hi   */
        if (gb->cgb_mode)
            m->hdma_src = (u16)((((unsigned)val << 8) | (m->hdma_src & 0x00FFu)) & 0xFFF0u);
        return;

    case 0xFF52:                                          /* HDMA2 src lo   */
        if (gb->cgb_mode)
            m->hdma_src = (u16)(((m->hdma_src & 0xFF00u) | (val & 0xF0u)) & 0xFFF0u);
        return;

    case 0xFF53: {                                        /* HDMA3 dst hi   */
        if (gb->cgb_mode) {
            u16 d = (u16)(((unsigned)val << 8) | (m->hdma_dst & 0x00FFu));
            m->hdma_dst = (u16)(0x8000u | (d & 0x1FF0u));
        }
        return;
    }

    case 0xFF54: {                                        /* HDMA4 dst lo   */
        if (gb->cgb_mode) {
            u16 d = (u16)((m->hdma_dst & 0xFF00u) | val);
            m->hdma_dst = (u16)(0x8000u | (d & 0x1FF0u));
        }
        return;
    }

    case 0xFF55: {                                        /* HDMA5 control  */
        u16 len;
        if (!gb->cgb_mode)
            return;
        len = (u16)((((unsigned)(val & 0x7Fu) + 1u) * HDMA_BLOCK));

        if (val & 0x80u) {
            /* HBlank DMA: one block per mode-0 entry. */
            m->hdma_len         = len;
            m->hdma_active      = true;
            m->hdma_hblank_done = false;
            return;
        }

        if (m->hdma_active) {
            /* bit7=0 while an HBlank DMA runs cancels it; the remaining
             * length stays readable through FF55 with bit 7 set. */
            m->hdma_active      = false;
            m->hdma_hblank_done = false;
            return;
        }

        /* General purpose DMA: copy the whole thing right now, charging the
         * machine for every block as we go. */
        m->hdma_active      = false;
        m->hdma_hblank_done = false;
        m->hdma_len         = len;
        while (m->hdma_len >= HDMA_BLOCK) {
            hdma_copy_block(gb);
            m->hdma_len = (u16)(m->hdma_len - HDMA_BLOCK);
            gb_tick(gb, HDMA_BLOCK_TICKS(gb));
        }
        m->hdma_len = 0;
        return;
    }

    case 0xFF56:                                          /* RP — not wired */
        return;

    case 0xFF68: case 0xFF69: case 0xFF6A: case 0xFF6B:
        if (gb->cgb_mode)
            ppu_write(gb, addr, val);
        return;

    case 0xFF6C:                                          /* OPRI           */
        return;

    case 0xFF70:                                          /* SVBK           */
        if (gb->cgb_mode)
            m->wram_bank = (u8)((val & 0x07u) ? (val & 0x07u) : 1u);
        return;

    case 0xFF72: m->ff72 = val;                     return;
    case 0xFF73: m->ff73 = val;                     return;
    case 0xFF74: m->ff74 = val;                     return;
    case 0xFF75: m->ff75 = (u8)(val & 0x70u);       return;

    default:
        return;
    }
}

/* ============================================================== OAM DMA === */
/*
 * Called from gb_tick() for every M-cycle of the machine, so the fast path is
 * a single predictable branch. Once armed, a byte is transferred every four
 * T-cycles after an initial start delay, for 160 bytes total.
 */
void mmu_tick_dma(GB *gb, int t)
{
    MMU *m = &gb->mmu;

    /* Cheap bookkeeping for the HBlank DMA one-block-per-HBlank latch: it is
     * cleared as soon as the PPU leaves mode 0. */
    if (m->hdma_hblank_done && gb->ppu.mode != PPU_MODE_HBLANK)
        m->hdma_hblank_done = false;

    if (!m->dma_active || t <= 0)
        return;

    if (m->dma_start_delay > 0) {
        if (m->dma_start_delay >= t) {
            m->dma_start_delay -= t;
            return;
        }
        t -= m->dma_start_delay;
        m->dma_start_delay = 0;
    }

    m->dma_timer += t;
    while (m->dma_timer >= OAM_DMA_PERIOD && m->dma_pos < OAM_DMA_BYTES) {
        u16 src;
        u8  b;

        m->dma_timer -= OAM_DMA_PERIOD;
        src = oam_dma_src_fixup((u16)(m->dma_src + (u16)m->dma_pos));
        b   = mmu_peek(gb, src);

        if (m->dma_pos >= 0 && (unsigned)m->dma_pos < OAM_SIZE)
            gb->ppu.oam[m->dma_pos] = b;

        m->dma_pos++;
    }

    if (m->dma_pos >= OAM_DMA_BYTES) {
        m->dma_pos    = OAM_DMA_BYTES;
        m->dma_timer  = 0;
        m->dma_active = false;
    }
}

/* ============================================================= HDMA step == */
/* Called by the PPU each time it enters mode 0 with the LCD enabled. */
void mmu_hdma_hblank(GB *gb)
{
    MMU *m = &gb->mmu;

    if (!gb->cgb_mode || !m->hdma_active || m->hdma_len == 0)
        return;
    if (!(gb->ppu.lcdc & LCDC_LCD_EN))
        return;
    if (m->hdma_hblank_done)        /* at most one block per HBlank */
        return;

    hdma_copy_block(gb);

    m->hdma_len = (m->hdma_len >= HDMA_BLOCK) ? (u16)(m->hdma_len - HDMA_BLOCK) : 0;
    if (m->hdma_len == 0)
        m->hdma_active = false;

    /* Latch before ticking: gb_tick() re-enters the PPU, and this stops a
     * nested mode-0 notification from copying a second block. */
    m->hdma_hblank_done = true;

    gb_tick(gb, HDMA_BLOCK_TICKS(gb));
}

/* CONTRACT-NOTE: -------------------------------------------------------------
 * 1. FF6C (OPRI) has no backing field anywhere in `struct GB`, so the written
 *    bit cannot be retained across a read or a save state. Reads therefore
 *    return 0xFE (OPRI = 0, i.e. CGB object priority) in CGB mode and 0xFF
 *    otherwise, and writes are accepted but discarded. The PPU has to decide
 *    sprite priority from gb->cgb_mode instead. Adding `u8 opri;` to MMU (or
 *    PPU) would make this register fully functional.
 * 2. FF55 needs to distinguish "HBlank DMA cancelled, N bytes left" (reads
 *    0x80 | remaining) from "transfer finished" (reads 0xFF). With no spare
 *    status field, that state is encoded as hdma_active == false together
 *    with hdma_len != 0; a completed transfer always leaves hdma_len == 0.
 * 3. `hdma_hblank_done` is documented as "one 0x10 block already copied this
 *    HBlank" but nothing in the contract resets it. mmu_tick_dma() clears it
 *    whenever the PPU is not in mode 0, which both matches the documented
 *    meaning and makes mmu_hdma_hblank() safe against being called more than
 *    once per HBlank (including re-entrantly from its own gb_tick()).
 * 4. FF4F is dispatched to ppu_write() (the PPU owns vram_bank) and then the
 *    documented value is written to gb->ppu.vram_bank directly. The second
 *    store is idempotent with a correct ppu_write() and keeps VRAM banking
 *    working if that decode ever misses FF4F.
 * -------------------------------------------------------------------------- */
