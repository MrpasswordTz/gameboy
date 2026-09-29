/* =============================================================================
 *  gb.c — The spine of the emulator.
 *
 *  This file owns the machine as a whole: creating and destroying it, loading a
 *  cartridge and an optional boot ROM, resetting every subsystem in the right
 *  order, and — most importantly — the single tick fan-out point that keeps the
 *  PPU, APU, timer, serial link, OAM DMA and the cartridge RTC in lock-step with
 *  the CPU.
 *
 *  Timing model (identical in every module, do not deviate):
 *    - gb_tick(gb, t) advances all NON-CPU subsystems by `t` T-cycles measured
 *      at the CURRENT CPU speed (CGB double-speed doubles the CPU clock).
 *    - mmu_read()/mmu_write() tick 4 T-cycles BEFORE performing the bus access,
 *      so cpu_step() only ticks for its own INTERNAL cycles.
 *    - Fixed-rate hardware (PPU/APU/RTC) runs off the unchanged 4.19 MHz dot
 *      clock, so it is fed t/speed; CPU-clocked hardware (DIV/TIMA, the serial
 *      shifter and OAM DMA) is fed the full `t`.
 * ========================================================================== */

#include "gb.h"

#include <stdlib.h>
#include <string.h>

/* Upper bound on breakpoint slots — mirrors `u32 breakpoints[32]` in gb.h. */
#define GB_MAX_BREAKPOINTS ((int)(sizeof ((GB *)0)->breakpoints / sizeof(u32)))

/* Give up on a frame after this many T-cycles so a host that calls
 * gb_run_frame() with the LCD switched off (or a wedged CPU) never hangs. */
#define GB_FRAME_CYCLE_BUDGET ((u64)GB_FRAME_DOTS * 4u)

/* -------------------------------------------------------------------------- */
/*  small local helpers                                                        */
/* -------------------------------------------------------------------------- */

/* Record a formatted diagnostic in gb->last_error (always NUL-terminated). */
static void gb_set_error(GB *gb, const char *what, const char *path)
{
    if (!gb)
        return;
    if (!path)
        path = "(null)";
    snprintf(gb->last_error, sizeof gb->last_error, "%s: %s", what, path);
}

/* Read an entire file into a freshly malloc'd buffer.
 * Returns NULL on any failure; *out_size receives the byte count on success. */
static u8 *gb_read_file(const char *path, size_t *out_size)
{
    FILE *f;
    long  end;
    size_t size, got;
    u8   *buf;

    *out_size = 0;
    if (!path || !*path)
        return NULL;

    f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0L, SEEK_END) != 0) { fclose(f); return NULL; }
    end = ftell(f);
    if (end <= 0)                    { fclose(f); return NULL; }
    rewind(f);

    size = (size_t)end;
    buf  = calloc(1, size);
    if (!buf)                        { fclose(f); return NULL; }

    got = fread(buf, 1, size, f);
    fclose(f);

    if (got != size) { free(buf); return NULL; }

    *out_size = size;
    return buf;
}

/* Drop any boot ROM we are holding. */
static void gb_free_boot_rom(GB *gb)
{
    free(gb->mmu.boot);
    gb->mmu.boot        = NULL;
    gb->mmu.boot_size   = 0;
    gb->mmu.boot_active = false;
}

/* Put the cartridge's MBC banking registers back into their power-on state.
 * Deliberately leaves cart.rom / cart.ram alone: a reset must never throw away
 * the loaded image or an unsaved battery-backed save. */
static void gb_reset_cart_banking(GB *gb)
{
    Cart *c = &gb->cart;

    c->rom_bank    = 1;
    c->rom_bank_0  = 0;
    c->ram_bank    = 0;
    c->ram_enabled = false;
    c->bank_lo     = 1;
    c->bank_hi     = 0;
    c->mode        = 0;
    c->rtc_select  = 0;
}

/* -------------------------------------------------------------------------- */
/*  lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

GB *gb_create(void)
{
    /* struct GB is ~100 KiB (frame buffer, WRAM, VRAM, audio ring): it must
     * live on the heap, never on a caller's stack. calloc() also gives us the
     * zero-initialised power-on state every subsystem expects. */
    GB *gb = calloc(1, sizeof *gb);
    if (!gb)
        return NULL;

    gb->model   = GB_MODEL_AUTO;
    gb->speed   = 1;
    gb->running = true;
    /* No cartridge yet, so nothing to reset: gb_load_rom() does that. */
    return gb;
}

void gb_destroy(GB *gb)
{
    if (!gb)
        return;

    /* Flush battery RAM before the cartridge image goes away. */
    if (gb->cart.rom)
        cart_save_ram(gb);
    cart_unload(gb);

    gb_free_boot_rom(gb);

    if (gb->trace_file && gb->trace_file != stdout && gb->trace_file != stderr)
        fclose(gb->trace_file);
    gb->trace_file = NULL;

    free(gb);
}

bool gb_load_rom(GB *gb, const char *path)
{
    if (!gb)
        return false;

    if (!cart_load(gb, path))
        return false;                    /* cart_load() filled last_error */

    /* Decide which machine we are pretending to be.
     *   AUTO : follow the cartridge's CGB flag.
     *   CGB  : CGB hardware even for a DMG-only game (compatibility mode).
     *   DMG  : plain Game Boy, CGB features off no matter what the cart says. */
    gb->cgb_hw   = (gb->model == GB_MODEL_CGB) ||
                   (gb->model == GB_MODEL_AUTO && (gb->cart.cgb_flag & 0x80) != 0);
    gb->cgb_mode = gb->cgb_hw && (gb->cart.cgb_flag & 0x80) != 0;

    gb_reset(gb);
    return true;
}

bool gb_load_boot_rom(GB *gb, const char *path)
{
    u8    *data;
    size_t size = 0;

    if (!gb)
        return false;

    /* A failed load must leave us with no boot ROM at all, so release the old
     * one up front and only install the new image once it has validated. */
    gb_free_boot_rom(gb);

    data = gb_read_file(path, &size);
    if (!data) {
        gb_set_error(gb, "cannot read boot ROM", path);
        return false;
    }

    /* 256 bytes = DMG/MGB boot ROM, 2048 bytes = CGB boot ROM. Nothing else. */
    if (size != 256 && size != 2048) {
        free(data);
        snprintf(gb->last_error, sizeof gb->last_error,
                 "boot ROM must be 256 or 2048 bytes (got %zu): %s",
                 size, path ? path : "(null)");
        return false;
    }

    gb->mmu.boot      = data;
    gb->mmu.boot_size = size;
    /* boot_active is armed by mmu_reset(), i.e. at the next gb_reset(). */
    return true;
}

void gb_reset(GB *gb)
{
    if (!gb)
        return;

    gb->speed        = 1;
    gb->speed_switch = false;
    gb->cycles       = 0;

    gb->ppu.cgb_mode = gb->cgb_mode;

    /* Order matters: cpu_reset() comes last because it inspects
     * mmu.boot_active to decide whether to start at 0x0000 (boot ROM) or at
     * 0x0100 with the post-boot register set. */
    mmu_reset(gb);
    ppu_reset(gb);
    /* ppu_reset() may clear the PPU wholesale — re-assert the mode it runs in. */
    gb->ppu.cgb_mode = gb->cgb_mode;
    apu_reset(gb);
    timer_reset(gb);
    joypad_reset(gb);
    serial_reset(gb);
    cpu_reset(gb);

    gb_reset_cart_banking(gb);

    gb->hit_breakpoint = false;
}

/* -------------------------------------------------------------------------- */
/*  the tick fan-out                                                           */
/* -------------------------------------------------------------------------- */

void gb_tick(GB *gb, int t)
{
    int nt;

    gb->cycles += (u64)t;

    /* CPU-clocked: these run twice as fast in CGB double-speed mode. */
    timer_tick(gb, t);
    serial_tick(gb, t);
    mmu_tick_dma(gb, t);

    /* Fixed-rate: the dot clock, the audio clock and the RTC do NOT change with
     * the CPU speed, so scale back down. `speed` is only ever 1 or 2 and `t` is
     * always a multiple of it, making the shift exact (and div-by-zero proof). */
    nt = (gb->speed > 1) ? (t >> 1) : t;

    ppu_tick(gb, nt);
    apu_tick(gb, nt);
    cart_tick_rtc(gb, nt);
}

void gb_request_interrupt(GB *gb, u8 mask)
{
    gb->mmu.ifr |= (u8)(mask & 0x1F);
}

/* -------------------------------------------------------------------------- */
/*  execution                                                                  */
/* -------------------------------------------------------------------------- */

int gb_step(GB *gb)
{
    int c;

    if (!gb || gb->paused)
        return 0;

    if (gb->trace) {
        char line[256];
        FILE *out = gb->trace_file ? gb->trace_file : stdout;

        line[0] = '\0';
        gb_trace_line(gb, line, sizeof line);
        if (line[0]) {
            size_t len = strlen(line);
            fputs(line, out);
            if (line[len - 1] != '\n')       /* len >= 1 here */
                fputc('\n', out);
        }
    }

    /* One instruction (or one idle M-cycle while halted). cpu_step() ticks the
     * machine itself, both through mmu_read/mmu_write and for internal cycles,
     * and returns the total T-cycle count. */
    c = cpu_step(gb);

    /* Breakpoints fire on the address we are ABOUT to execute next. */
    if (gb->n_breakpoints > 0) {
        int n = gb->n_breakpoints;
        int i;
        if (n > GB_MAX_BREAKPOINTS)
            n = GB_MAX_BREAKPOINTS;
        for (i = 0; i < n; i++) {
            if (gb->breakpoints[i] == (u32)gb->cpu.pc) {
                gb->hit_breakpoint = true;
                break;
            }
        }
    }

    return c;
}

void gb_run_frame(GB *gb)
{
    u64 start;

    if (!gb || gb->paused)
        return;

    gb->ppu.frame_ready = false;
    start = gb->cycles;

    while (!gb->ppu.frame_ready && !gb->hit_breakpoint && !gb->paused) {
        u64 before = gb->cycles;
        int c      = gb_step(gb);

        /* Guard against a CPU that cannot make progress (STOP with no wake-up
         * source, or a host that paused us mid-step): bail instead of spinning. */
        if (c <= 0 && gb->cycles == before)
            break;

        /* With the LCD off the PPU never completes a frame. Bound the work so
         * the host's main loop keeps servicing input and audio. */
        if (gb->cycles - start > GB_FRAME_CYCLE_BUDGET)
            break;
    }
}

/* CONTRACT-NOTE:
 *   struct GB carries `bool throttle_ticks` described in gb.h as an "internal
 *   re-entrancy guard for gb_tick". gb.c deliberately does not read or write it:
 *   the fan-out has no re-entrant path (OAM DMA, HDMA, the PPU and save states
 *   all use the non-ticking mmu_peek/mmu_poke, and no *_tick callee calls
 *   gb_tick), so a guard here could only ever drop cycles and desynchronise the
 *   subsystems. The field is left zeroed by gb_create(); honouring it would also
 *   risk permanently freezing the machine if another module ever set it without
 *   clearing it. gb_tick is also the one function in this file without a NULL
 *   check — it runs millions of times a second and is only ever reached from
 *   code that already holds a valid GB *.
 */
