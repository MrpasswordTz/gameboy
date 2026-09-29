/* =============================================================================
 *  gb.h — Core contract for the Game Boy / Game Boy Color emulator.
 *
 *  Every translation unit in the emulator core includes exactly this header.
 *  It defines the machine state, the memory map, and the complete set of
 *  inter-module entry points. Nothing in the core allocates except cart.c
 *  (ROM/RAM images) and gb_create(); everything else lives inside `struct GB`.
 * ========================================================================== */
#ifndef GB_H
#define GB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* ---------------------------------------------------------------- constants */
#define GB_SCREEN_W        160
#define GB_SCREEN_H        144
#define GB_CPU_HZ          4194304u          /* T-cycles per second (DMG)     */
#define GB_FRAME_DOTS      70224             /* T-cycles per video frame      */
#define GB_AUDIO_RATE      48000
#define GB_AUDIO_CHANNELS  2
/* Ring buffer holds ~0.25s of stereo float samples. Power of two. */
#define GB_AUDIO_RING      16384

/* Interrupt bits (IE @ 0xFFFF, IF @ 0xFF0F) */
#define INT_VBLANK  0x01
#define INT_STAT    0x02
#define INT_TIMER   0x04
#define INT_SERIAL  0x08
#define INT_JOYPAD  0x10

/* CPU flag bits in register F */
#define FLAG_Z 0x80
#define FLAG_N 0x40
#define FLAG_H 0x20
#define FLAG_C 0x10

/* LCDC bits */
#define LCDC_BG_EN      0x01  /* DMG: BG enable.  CGB: BG/Win master priority */
#define LCDC_OBJ_EN     0x02
#define LCDC_OBJ_SIZE   0x04  /* 0 = 8x8, 1 = 8x16 */
#define LCDC_BG_MAP     0x08  /* 0 = 9800, 1 = 9C00 */
#define LCDC_TILE_DATA  0x10  /* 0 = 8800 signed, 1 = 8000 unsigned */
#define LCDC_WIN_EN     0x20
#define LCDC_WIN_MAP    0x40
#define LCDC_LCD_EN     0x80

/* STAT bits */
#define STAT_MODE_MASK  0x03
#define STAT_LYC_EQ     0x04
#define STAT_INT_HBLANK 0x08
#define STAT_INT_VBLANK 0x10
#define STAT_INT_OAM    0x20
#define STAT_INT_LYC    0x40

/* PPU modes */
enum { PPU_MODE_HBLANK = 0, PPU_MODE_VBLANK = 1, PPU_MODE_OAM = 2, PPU_MODE_DRAW = 3 };

/* Joypad buttons — bit positions used by joypad_set_buttons() */
enum {
    BTN_RIGHT = 0x01, BTN_LEFT   = 0x02, BTN_UP     = 0x04, BTN_DOWN  = 0x08,
    BTN_A     = 0x10, BTN_B      = 0x20, BTN_SELECT = 0x40, BTN_START = 0x80
};

typedef struct GB GB;

/* -------------------------------------------------------------------- CPU   */
typedef struct {
    u8  a, f, b, c, d, e, h, l;
    u16 sp, pc;
    bool ime;          /* interrupt master enable                              */
    bool ime_pending;  /* EI: IME becomes true after the *next* instruction     */
    bool halted;
    bool halt_bug;     /* HALT executed with IME=0 && (IE&IF): PC fails to inc  */
    bool stopped;
    /* set by cpu_step when an illegal opcode is hit (0xD3,0xDB,0xDD,...)       */
    bool illegal_op;
    u8   illegal_opcode;
} CPU;

/* Register-pair accessors (portable, endian-independent). */
static inline u16 rp_get(u8 hi, u8 lo) { return (u16)((u16)hi << 8 | lo); }
#define AF(cpu_) rp_get((cpu_)->a, (cpu_)->f)
#define BC(cpu_) rp_get((cpu_)->b, (cpu_)->c)
#define DE(cpu_) rp_get((cpu_)->d, (cpu_)->e)
#define HL(cpu_) rp_get((cpu_)->h, (cpu_)->l)
static inline void set_af(CPU *c, u16 v){ c->a=(u8)(v>>8); c->f=(u8)(v & 0xF0); }
static inline void set_bc(CPU *c, u16 v){ c->b=(u8)(v>>8); c->c=(u8)v; }
static inline void set_de(CPU *c, u16 v){ c->d=(u8)(v>>8); c->e=(u8)v; }
static inline void set_hl(CPU *c, u16 v){ c->h=(u8)(v>>8); c->l=(u8)v; }

/* ------------------------------------------------------------------ CART    */
typedef enum {
    MBC_NONE = 0, MBC_1, MBC_2, MBC_3, MBC_5, MBC_HUC1, MBC_UNKNOWN
} MbcType;

typedef struct {
    u8  s, m, h, dl, dh;   /* seconds, minutes, hours, day-low, day-high      */
    u8  latched[5];
    bool latch_armed;
    s64 base_unix;         /* wall-clock anchor for the running RTC           */
    u64 sub_cycles;        /* T-cycles accumulated toward the next RTC second */
} RTC;

typedef struct {
    u8      *rom;          /* owned, malloc'd — rom_size bytes                */
    size_t   rom_size;
    u8      *ram;          /* owned, malloc'd (may be NULL) — ram_size bytes  */
    size_t   ram_size;
    MbcType  mbc;
    bool     has_battery, has_rtc, has_rumble;
    char     title[17];
    u8       cgb_flag;     /* 0x80 = CGB enhanced, 0xC0 = CGB only            */
    u8       sgb_flag;
    u8       cart_type, rom_size_code, ram_size_code;
    u8       header_checksum, computed_header_checksum;
    u16      global_checksum, computed_global_checksum;

    /* live banking state */
    u32  rom_bank;         /* resolved 0-based ROM bank for 0x4000-0x7FFF     */
    u32  rom_bank_0;       /* resolved bank for 0x0000-0x3FFF (MBC1 mode 1)   */
    u32  ram_bank;
    bool ram_enabled;
    u8   bank_lo, bank_hi; /* raw register latches                            */
    u8   mode;             /* MBC1 banking mode select                        */
    u8   rtc_select;       /* MBC3: 0x08-0x0C selects an RTC register         */
    RTC  rtc;

    bool ram_dirty;        /* battery RAM changed since last flush            */
    char save_path[1024];
} Cart;

/* ------------------------------------------------------------------- PPU    */
typedef struct {
    u8  vram[2][0x2000];     /* bank 0 always; bank 1 CGB only                */
    u8  vram_bank;           /* VBK, 0 or 1                                   */
    u8  oam[0xA0];

    u8  lcdc, stat, scy, scx, ly, lyc, dma, bgp, obp0, obp1, wy, wx;
    u8  mode;                /* PPU_MODE_*                                    */
    int dots;                /* 0..455 dot counter inside the current line    */
    int mode3_len;           /* length of the current mode 3 (dots)           */
    u8  wly;                 /* window internal line counter                  */
    bool win_triggered;      /* WY==LY seen at some point this frame          */
    bool stat_line;          /* previous STAT IRQ line level (edge detect)    */
    bool lcd_was_off;
    bool frame_ready;        /* set once per completed frame; cleared by host */
    u32  frames;

    /* CGB palette RAM + auto-increment specifiers */
    u8  bcps, ocps;
    u8  bg_pal[64];
    u8  obj_pal[64];
    bool cgb_mode;
    bool color_correct;      /* apply CGB->sRGB correction (default on) */

    /* DMG compatibility palettes used when a DMG game runs on CGB hardware   */
    u32 dmg_bg_lut[4], dmg_obj_lut[2][4];

    /* Per-scanline shadow used for sprite/BG priority resolution             */
    u8  line_bg_index[GB_SCREEN_W];  /* BG/Win colour index 0..3              */
    u8  line_bg_prio[GB_SCREEN_W];   /* CGB BG attribute bit 7                */

    u32 fb[GB_SCREEN_W * GB_SCREEN_H];   /* 0xAARRGGBB, ready to blit         */
} PPU;

/* ------------------------------------------------------------------- APU    */
typedef struct {
    bool  enabled;        /* channel DAC+trigger state                         */
    bool  dac_on;
    u8    nrx0, nrx1, nrx2, nrx3, nrx4;
    int   length;         /* length counter                                    */
    bool  length_enable;
    int   timer;          /* frequency timer (T-cycles)                        */
    int   duty_pos;
    /* envelope */
    int   env_timer, env_period, volume, env_start_vol;
    bool  env_dir_up;
    /* sweep (ch1 only) */
    int   sweep_timer, sweep_period, sweep_shift;
    bool  sweep_dir_down, sweep_enabled;
    int   sweep_shadow;
    bool  sweep_calc_made;
} SquareCh;

typedef struct {
    bool enabled, dac_on;
    u8   nr30, nr31, nr32, nr33, nr34;
    int  length;
    bool length_enable;
    int  timer;
    int  pos;             /* sample index 0..31                                */
    u8   sample_buf;      /* last byte fetched from wave RAM                   */
    u8   ram[16];
    int  volume_shift;
} WaveCh;

typedef struct {
    bool enabled, dac_on;
    u8   nr41, nr42, nr43, nr44;
    int  length;
    bool length_enable;
    int  timer;
    u16  lfsr;
    int  env_timer, env_period, volume, env_start_vol;
    bool env_dir_up;
    bool width7;
} NoiseCh;

typedef struct {
    bool     enabled;                 /* NR52 bit 7                           */
    SquareCh ch1, ch2;
    WaveCh   ch3;
    NoiseCh  ch4;
    u8       nr50, nr51;
    int      frame_seq;               /* 0..7                                 */
    int      frame_seq_timer;         /* counts down 8192 T-cycles            */
    u64      sample_accum;            /* fixed-point resampler accumulator    */
    /* lock-free-ish single-producer/single-consumer ring of stereo frames    */
    float    ring[GB_AUDIO_RING * 2];
    volatile u32 wr, rd;              /* frame indices, wrap at GB_AUDIO_RING */
    bool     muted;
    float    master_volume;           /* 0.0 .. 1.0, host controlled          */
    float    hp_l, hp_r;              /* DC-blocking filter state             */
} APU;

/* ----------------------------------------------------------------- TIMER    */
typedef struct {
    u16  div;            /* internal 16-bit counter; DIV reg = div >> 8        */
    u8   tima, tma, tac;
    bool last_and;       /* previous (div_bit & tac_enable) for falling edge   */
    u8   overflow_delay; /* T-cycles until TIMA<-TMA + IRQ (0 = idle)          */
    bool overflowing;
} Timer;

/* ---------------------------------------------------------------- JOYPAD    */
typedef struct {
    u8   buttons;        /* BTN_* bitmask, 1 = pressed                         */
    u8   p1;             /* raw selection bits written by the game             */
} Joypad;

/* ---------------------------------------------------------------- SERIAL    */
typedef struct {
    u8   sb, sc;
    int  bit;            /* bits shifted out so far                            */
    int  timer;          /* T-cycles until next bit                            */
    bool transferring;
    /* optional: every byte written is echoed here (Blargg ROMs report here)   */
    char log[4096];
    int  log_len;
} Serial;

/* ------------------------------------------------------------------- MMU    */
typedef struct {
    u8   wram[8][0x1000];   /* 8 banks of 4 KiB (DMG uses banks 0 and 1)       */
    u8   wram_bank;         /* SVBK, 1..7                                      */
    u8   hram[0x7F];
    u8   ie, ifr;           /* 0xFFFF and 0xFF0F                               */

    /* Boot ROM */
    u8  *boot;              /* owned, may be NULL                              */
    size_t boot_size;
    bool boot_active;

    /* OAM DMA (0xFF46) */
    bool dma_active;
    u16  dma_src;
    int  dma_pos;           /* bytes copied 0..159                             */
    int  dma_timer;         /* T-cycles until the next byte                    */
    int  dma_start_delay;

    /* CGB HDMA/GDMA (0xFF51-0xFF55) */
    u16  hdma_src, hdma_dst;
    u16  hdma_len;          /* remaining bytes                                 */
    bool hdma_active;       /* HBlank DMA in progress                          */
    bool hdma_hblank_done;  /* one 0x10 block already copied this HBlank       */

    u8   ff72, ff73, ff74, ff75;   /* undocumented CGB scratch registers       */
} MMU;

/* --------------------------------------------------------------- MACHINE    */
typedef enum { GB_MODEL_AUTO = 0, GB_MODEL_DMG, GB_MODEL_CGB } GbModel;

struct GB {
    CPU    cpu;
    MMU    mmu;
    PPU    ppu;
    APU    apu;
    Timer  timer;
    Joypad joypad;
    Serial serial;
    Cart   cart;

    GbModel model;        /* requested model                                   */
    bool    cgb_hw;       /* hardware is a CGB                                 */
    bool    cgb_mode;     /* CGB features active (cart is CGB-aware)           */
    int     speed;        /* 1 = normal, 2 = double speed (CGB)                */
    bool    speed_switch; /* KEY1 bit 0 armed                                  */

    u64  cycles;          /* total T-cycles since power on (normal speed base) */
    bool throttle_ticks;  /* internal re-entrancy guard for gb_tick            */

    /* diagnostics / control */
    bool  running;
    bool  paused;
    u32   breakpoints[32];
    int   n_breakpoints;
    bool  trace;
    FILE *trace_file;
    bool  hit_breakpoint;
    char  last_error[256];
};

/* =============================== API ====================================== */

/* --- gb.c ---------------------------------------------------------------- */
GB  *gb_create(void);
void gb_destroy(GB *gb);
bool gb_load_rom(GB *gb, const char *path);          /* false => gb->last_error */
bool gb_load_boot_rom(GB *gb, const char *path);
void gb_reset(GB *gb);
/* Execute one CPU instruction (or one idle M-cycle when halted). Returns the
 * number of T-cycles consumed, measured in normal-speed units.               */
int  gb_step(GB *gb);
/* Run until a full video frame has been produced. */
void gb_run_frame(GB *gb);
/* Advance every non-CPU subsystem by `t` T-cycles. Called from inside memory
 * accesses; must be safe to call with t == 4 many times per instruction.     */
void gb_tick(GB *gb, int t);
void gb_request_interrupt(GB *gb, u8 mask);
const char *gb_mbc_name(MbcType m);

/* --- cpu.c --------------------------------------------------------------- */
void cpu_reset(GB *gb);
int  cpu_step(GB *gb);          /* returns T-cycles; ticks the machine itself  */
int  cpu_handle_interrupts(GB *gb);
void cpu_exec_cb(GB *gb);       /* implemented in cpu_cb.c                     */

/* --- mmu.c --------------------------------------------------------------- */
void mmu_reset(GB *gb);
u8   mmu_read(GB *gb, u16 addr);              /* ticks 4 T-cycles              */
void mmu_write(GB *gb, u16 addr, u8 val);     /* ticks 4 T-cycles              */
u8   mmu_peek(GB *gb, u16 addr);              /* side-effect free (debugger)   */
void mmu_poke(GB *gb, u16 addr, u8 val);      /* no ticking                    */
u8   mmu_read_io(GB *gb, u16 addr);
void mmu_write_io(GB *gb, u16 addr, u8 val);
void mmu_tick_dma(GB *gb, int t);
void mmu_hdma_hblank(GB *gb);                 /* called by ppu on HBlank entry */

/* --- cart.c -------------------------------------------------------------- */
bool cart_load(GB *gb, const char *path);
void cart_unload(GB *gb);
u8   cart_read_rom(GB *gb, u16 addr);
u8   cart_read_ram(GB *gb, u16 addr);
void cart_write_rom(GB *gb, u16 addr, u8 val);   /* MBC control registers      */
void cart_write_ram(GB *gb, u16 addr, u8 val);
void cart_tick_rtc(GB *gb, int t);
bool cart_save_ram(GB *gb);
bool cart_load_ram(GB *gb);

/* --- ppu.c --------------------------------------------------------------- */
void ppu_reset(GB *gb);
void ppu_tick(GB *gb, int t);
u8   ppu_read(GB *gb, u16 addr);
void ppu_write(GB *gb, u16 addr, u8 val);
u8   ppu_read_vram(GB *gb, u16 addr);
void ppu_write_vram(GB *gb, u16 addr, u8 val);
u8   ppu_read_oam(GB *gb, u16 addr);
void ppu_write_oam(GB *gb, u16 addr, u8 val);
void ppu_set_dmg_palette(GB *gb, int which);   /* cycle built-in DMG palettes  */

/* --- apu.c --------------------------------------------------------------- */
void apu_reset(GB *gb);
void apu_tick(GB *gb, int t);
u8   apu_read(GB *gb, u16 addr);
void apu_write(GB *gb, u16 addr, u8 val);
/* Drain up to `frames` stereo frames into `out` (interleaved L,R). Returns the
 * number of frames actually written; pads with silence if starved.           */
int  apu_pull(GB *gb, float *out, int frames);
int  apu_available(GB *gb);

/* --- timer.c ------------------------------------------------------------- */
void timer_reset(GB *gb);
void timer_tick(GB *gb, int t);
u8   timer_read(GB *gb, u16 addr);
void timer_write(GB *gb, u16 addr, u8 val);

/* --- joypad.c / serial.c ------------------------------------------------- */
void joypad_reset(GB *gb);
void joypad_set_buttons(GB *gb, u8 mask);      /* full state, 1 = pressed      */
u8   joypad_read(GB *gb);
void joypad_write(GB *gb, u8 val);
void serial_reset(GB *gb);
void serial_tick(GB *gb, int t);
u8   serial_read(GB *gb, u16 addr);
void serial_write(GB *gb, u16 addr, u8 val);

/* --- savestate.c --------------------------------------------------------- */
bool savestate_save(GB *gb, const char *path);
bool savestate_load(GB *gb, const char *path);

/* --- debugger.c ---------------------------------------------------------- */
int  gb_disasm(GB *gb, u16 addr, char *out, size_t out_sz);  /* ret: length    */
void gb_trace_line(GB *gb, char *out, size_t out_sz);
void debugger_repl(GB *gb);
void debugger_break_here(GB *gb);

#endif /* GB_H */
