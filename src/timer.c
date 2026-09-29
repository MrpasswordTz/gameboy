/* =============================================================================
 *  timer.c — DIV / TIMA / TMA / TAC (0xFF04-0xFF07)
 *
 *  Cycle-accurate "falling edge" model, which is what Blargg's timer test ROMs
 *  actually probe:
 *
 *    * `timer.div` is the internal 16-bit system counter.  It is incremented
 *      once per T-cycle and is NEVER reset except by a write to 0xFF04 (and by
 *      a hardware reset).  The DIV register visible at 0xFF04 is `div >> 8`.
 *
 *    * TAC (0xFF07) bit 2 enables the timer; bits 1-0 select which bit of the
 *      internal counter is tapped:
 *
 *          TAC&3 | tapped bit | resulting TIMA rate
 *          ------+------------+---------------------
 *            0   |     9      |   4096 Hz  (every 1024 T-cycles)
 *            1   |     3      | 262144 Hz  (every    16 T-cycles)
 *            2   |     5      |  65536 Hz  (every    64 T-cycles)
 *            3   |     7      |  16384 Hz  (every   256 T-cycles)
 *
 *    * The hardware ANDs the tapped bit with the enable bit and feeds the
 *      result into a falling-edge detector; TIMA increments on every 1 -> 0
 *      transition of that signal.  Because the detector watches a *level* and
 *      not a counter, anything that can drop the level — a DIV reset, changing
 *      the multiplexer selection, or clearing the enable bit — can itself make
 *      TIMA tick.  All of those paths funnel through timer_edge() below.
 *
 *    * TIMA overflow is delayed.  When TIMA wraps 0xFF -> 0x00 it *stays* 0x00
 *      for 4 T-cycles; only then is TMA copied in and the timer interrupt
 *      requested.  Software that writes TIMA inside that window cancels both
 *      the reload and the interrupt; software that writes TMA inside that
 *      window has the *new* TMA loaded.
 * ========================================================================== */
#include "gb.h"

/* Which bit of the 16-bit internal counter each TAC clock-select taps. */
static const u8 TIMER_TAP_BIT[4] = { 9, 3, 5, 7 };

/* Unused bits of TAC read back as 1. */
#define TAC_UNUSED 0xF8u

/* --------------------------------------------------------------------------
 *  TIMA increment, including the delayed-overflow behaviour.
 * ------------------------------------------------------------------------ */
static void timer_inc_tima(GB *gb)
{
    if (gb->timer.tima == 0xFF) {
        /* Overflow: TIMA reads back as 0 for the next 4 T-cycles, and only
         * after that is TMA loaded and the interrupt raised. */
        gb->timer.tima           = 0x00;
        gb->timer.overflowing    = true;
        gb->timer.overflow_delay = 4;
    } else {
        gb->timer.tima = (u8)(gb->timer.tima + 1u);
    }
}

/* --------------------------------------------------------------------------
 *  Recompute the (tapped_bit AND enable) signal and fire TIMA on a 1 -> 0
 *  transition.  Must be called after *every* change to div or tac.
 * ------------------------------------------------------------------------ */
static void timer_edge(GB *gb)
{
    u8   tap    = TIMER_TAP_BIT[gb->timer.tac & 0x03u];   /* index is 0..3   */
    bool bit_hi = ((gb->timer.div >> tap) & 1u) != 0u;
    bool enable = (gb->timer.tac & 0x04u) != 0u;
    bool level  = bit_hi && enable;

    if (gb->timer.last_and && !level)
        timer_inc_tima(gb);

    gb->timer.last_and = level;
}

/* --------------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------------ */
void timer_reset(GB *gb)
{
    if (!gb)
        return;

    /* Post-boot-ROM values.  DMG leaves the internal counter such that DIV
     * reads 0xAB; CGB's boot ROM is a different length and leaves 0x1EA0. */
    gb->timer.div            = gb->cgb_hw ? 0x1EA0u : 0xABCCu;
    gb->timer.tima           = 0x00;
    gb->timer.tma            = 0x00;
    gb->timer.tac            = TAC_UNUSED;   /* timer disabled, select 0     */
    gb->timer.last_and       = false;        /* enable bit is clear => 0     */
    gb->timer.overflow_delay = 0;
    gb->timer.overflowing    = false;
}

void timer_tick(GB *gb, int t)
{
    int i;

    if (!gb || t <= 0)
        return;

    /* One T-cycle at a time: at the 262144 Hz setting the tapped bit toggles
     * every 8 T-cycles, and a DIV write can land anywhere, so batching would
     * lose edges.  `t` is 4 (or 8 in double speed) in practice. */
    for (i = 0; i < t; i++) {
        /* 1. Retire a pending overflow *before* advancing the counter, so the
         *    reload lands exactly 4 T-cycles after the wrap. */
        if (gb->timer.overflowing) {
            if (gb->timer.overflow_delay > 0)
                gb->timer.overflow_delay--;
            if (gb->timer.overflow_delay == 0) {
                gb->timer.tima        = gb->timer.tma;
                gb->timer.overflowing = false;
                gb_request_interrupt(gb, INT_TIMER);
            }
        }

        /* 2. Advance the free-running system counter. */
        gb->timer.div = (u16)(gb->timer.div + 1u);

        /* 3. Falling-edge detect on (tapped bit & enable). */
        timer_edge(gb);
    }
}

u8 timer_read(GB *gb, u16 addr)
{
    if (!gb)
        return 0xFF;

    switch (addr) {
    case 0xFF04: return (u8)(gb->timer.div >> 8);
    case 0xFF05: return gb->timer.tima;
    case 0xFF06: return gb->timer.tma;
    case 0xFF07: return (u8)(gb->timer.tac | TAC_UNUSED);
    default:     return 0xFF;
    }
}

void timer_write(GB *gb, u16 addr, u8 val)
{
    if (!gb)
        return;

    switch (addr) {
    case 0xFF04:
        /* Any write clears the whole 16-bit counter.  If the tapped bit was
         * high this drops the signal and therefore ticks TIMA — the classic
         * "resetting DIV can increment TIMA" quirk. */
        gb->timer.div = 0;
        timer_edge(gb);
        break;

    case 0xFF05:
        if (gb->timer.overflowing) {
            /* Write during the 4-cycle reload window: abort the reload and
             * the pending interrupt, keeping the written value. */
            gb->timer.overflowing    = false;
            gb->timer.overflow_delay = 0;
        }
        gb->timer.tima = val;
        break;

    case 0xFF06:
        /* If a reload is pending, it will pick up this new value (the reload
         * reads gb->timer.tma at the moment it fires). */
        gb->timer.tma = val;
        break;

    case 0xFF07:
        /* Changing the multiplexer selection or clearing the enable bit can
         * both produce a falling edge, so re-run the detector. */
        gb->timer.tac = (u8)((val & 0x07u) | TAC_UNUSED);
        timer_edge(gb);
        break;

    default:
        break;
    }
}
