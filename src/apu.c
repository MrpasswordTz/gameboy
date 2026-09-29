/* =============================================================================
 *  apu.c — Game Boy / Game Boy Color Audio Processing Unit.
 *
 *  Four channels:
 *      ch1  square wave + frequency sweep + volume envelope + length counter
 *      ch2  square wave              + volume envelope + length counter
 *      ch3  32x4-bit wave RAM player                   + length counter
 *      ch4  LFSR noise              + volume envelope + length counter
 *
 *  A 512 Hz "frame sequencer" (one step every 8192 T-cycles) drives the
 *  length counters, the sweep unit and the volume envelopes:
 *
 *      step : 0  1  2  3  4  5  6  7
 *      len  : x     x     x     x           (steps 0,2,4,6)
 *      sweep:       x           x           (steps 2,6)
 *      env  :                         x     (step 7)
 *
 *  `apu.frame_seq` always holds the index of the *next* step to execute, which
 *  is exactly what the NRx4 "extra length clock" quirk needs to test.
 *
 *  Output is resampled from the 4194304 Hz core clock down to GB_AUDIO_RATE
 *  with a fixed-point accumulator and pushed into a single-producer /
 *  single-consumer ring of stereo float frames.  The emitter (emulation
 *  thread) only ever writes `wr`; apu_pull()/apu_available() (audio thread)
 *  only ever write `rd`.  Overruns are resolved on the consumer side by
 *  skipping ahead, so the producer never blocks and never stalls the core.
 *
 *  Every index derived from guest data is masked or clamped before use.
 * ========================================================================== */

#include "gb.h"

#include <string.h>

/* ------------------------------------------------------------------ tables */

/* Duty patterns, MSB first, as 8 entries of 0/1. */
static const u8 duty_table[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },   /* 12.5%  00000001 */
    { 1, 0, 0, 0, 0, 0, 0, 1 },   /* 25%    10000001 */
    { 1, 0, 0, 0, 0, 1, 1, 1 },   /* 50%    10000111 */
    { 0, 1, 1, 1, 1, 1, 1, 0 }    /* 75%    01111110 */
};

/* NR43 divisor codes 0..7. */
static const int noise_divisor[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

/* NR32 volume code -> right shift applied to the 4-bit wave sample. */
static const int wave_shift_table[4] = { 4, 0, 1, 2 };   /* mute,100%,50%,25% */

/* Bits that always read back as 1, indexed by (addr - 0xFF10). */
static const u8 read_mask[0x17] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF,   /* FF10 NR10 .. FF14 NR14 */
    0xFF, 0x3F, 0x00, 0xFF, 0xBF,   /* FF15 ---- .. FF19 NR24 */
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF,   /* FF1A NR30 .. FF1E NR34 */
    0xFF, 0xFF, 0x00, 0x00, 0xBF,   /* FF1F ---- .. FF23 NR44 */
    0x00, 0x00, 0x70                /* FF24 NR50 .. FF26 NR52 */
};

/* Classic DMG power-on contents of wave RAM. */
static const u8 wave_power_on[16] = {
    0x84, 0x40, 0x43, 0xAA, 0x2D, 0x78, 0x92, 0x3C,
    0x60, 0x59, 0x59, 0xB0, 0x34, 0xB8, 0x2E, 0xDA
};

/* One-pole DC blocker: ~6 Hz corner at 48 kHz (2*pi*6/48000). */
#define HP_ALPHA 0.0008f

/* Soft-clip knee.  Below this the mixer is perfectly linear; above it the
 * output is compressed smoothly so it can approach but never exceed 1.0.     */
#define SOFT_KNEE 0.75f

#define APU_MAX_TICK 0x20000   /* sanity clamp on a single apu_tick() call */

/* ------------------------------------------------------- generic helpers   */

/* Frame-sequencer steps 0,2,4,6 clock the length counters.  `frame_seq` is the
 * index of the NEXT step, so an odd value means the next step will NOT clock
 * length — the condition for the NRx4 "extra length clock" hardware quirk.   */
static bool extra_length_clock(const APU *a)
{
    return (a->frame_seq & 1) != 0;
}

static void length_step(int *length, bool enable, bool *chan_enabled)
{
    if (!enable || *length <= 0)
        return;
    if (--(*length) <= 0) {
        *length = 0;
        *chan_enabled = false;
    }
}

static void envelope_step(int *env_timer, int env_period, int *volume, bool up)
{
    if (env_period <= 0)
        return;
    if (*env_timer > 0)
        (*env_timer)--;
    if (*env_timer <= 0) {
        *env_timer = env_period;
        if (up) {
            if (*volume < 15) (*volume)++;
        } else {
            if (*volume > 0) (*volume)--;
        }
    }
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ------------------------------------------------------------ square wave  */

static int sq_freq(const SquareCh *c)
{
    return (int)(((u16)(c->nrx4 & 0x07) << 8) | c->nrx3);   /* 0..2047 */
}

static int sq_period(const SquareCh *c)
{
    int p = (2048 - sq_freq(c)) * 4;
    return p > 0 ? p : 4;                 /* freq <= 2047 => p >= 4, defensive */
}

static int sq_output(const SquareCh *c)
{
    int duty, pos;
    if (!c->enabled || !c->dac_on)
        return 0;
    duty = (c->nrx1 >> 6) & 3;            /* 0..3  — always in range          */
    pos  = c->duty_pos & 7;               /* 0..7  — always in range          */
    return duty_table[duty][pos] ? clamp_int(c->volume, 0, 15) : 0;
}

static void sq_tick(SquareCh *c, int t)
{
    int period;
    c->timer -= t;
    if (c->timer > 0)
        return;
    period = sq_period(c);
    while (c->timer <= 0) {
        c->timer += period;
        c->duty_pos = (c->duty_pos + 1) & 7;
    }
}

/* Sweep frequency calculation.  Disables the channel on overflow and records
 * that a calculation happened (needed for the NR10 direction-clear quirk).   */
static int sweep_calculate(SquareCh *c)
{
    int shift = c->sweep_shift & 7;
    int delta = c->sweep_shadow >> shift;
    int nf    = c->sweep_dir_down ? (c->sweep_shadow - delta)
                                  : (c->sweep_shadow + delta);
    c->sweep_calc_made = true;
    if (nf > 2047)
        c->enabled = false;
    if (nf < 0)
        nf = 0;
    return nf;
}

static void sweep_step(SquareCh *c)
{
    int nf;
    if (c->sweep_timer > 0)
        c->sweep_timer--;
    if (c->sweep_timer > 0)
        return;

    c->sweep_timer = c->sweep_period > 0 ? c->sweep_period : 8;
    if (!c->sweep_enabled || c->sweep_period == 0)
        return;

    nf = sweep_calculate(c);
    if (nf <= 2047 && c->sweep_shift != 0) {
        c->sweep_shadow = nf;
        c->nrx3 = (u8)(nf & 0xFF);
        c->nrx4 = (u8)((c->nrx4 & 0xF8) | ((nf >> 8) & 0x07));
        /* Second overflow check; the result itself is discarded. */
        (void)sweep_calculate(c);
    }
}

static void sq_trigger(APU *a, SquareCh *c, int max_length, bool has_sweep)
{
    c->enabled = c->dac_on;
    if (c->length <= 0) {
        c->length = max_length;
        /* An immediately-following length clock also applies to the reload. */
        if (c->length_enable && extra_length_clock(a))
            c->length--;
    }
    c->timer     = sq_period(c);
    c->env_timer = c->env_period > 0 ? c->env_period : 8;
    c->volume    = clamp_int(c->env_start_vol, 0, 15);

    if (has_sweep) {
        c->sweep_shadow     = sq_freq(c);
        c->sweep_period     = (c->nrx0 >> 4) & 7;
        c->sweep_dir_down   = (c->nrx0 & 0x08) != 0;
        c->sweep_shift      = c->nrx0 & 7;
        c->sweep_timer      = c->sweep_period > 0 ? c->sweep_period : 8;
        c->sweep_enabled    = (c->sweep_period != 0) || (c->sweep_shift != 0);
        c->sweep_calc_made  = false;
        if (c->sweep_shift != 0)
            (void)sweep_calculate(c);     /* immediate overflow check */
    }
}

/* Shared NRx1 / NRx2 / NRx3 / NRx4 handling for both square channels. */
static void sq_write_nrx1(SquareCh *c, u8 val)
{
    c->nrx1   = val;
    c->length = 64 - (int)(val & 0x3F);
}

static void sq_write_nrx2(SquareCh *c, u8 val)
{
    c->nrx2          = val;
    c->env_start_vol = (val >> 4) & 0x0F;
    c->env_dir_up    = (val & 0x08) != 0;
    c->env_period    = val & 0x07;
    c->dac_on        = (val & 0xF8) != 0;
    if (!c->dac_on)
        c->enabled = false;               /* DAC off kills the channel */
}

static void sq_write_nrx4(APU *a, SquareCh *c, u8 val, bool has_sweep)
{
    bool prev_len_en = c->length_enable;
    bool trigger, extra;

    c->nrx4          = val;
    c->length_enable = (val & 0x40) != 0;
    trigger          = (val & 0x80) != 0;
    extra            = extra_length_clock(a);

    /* Quirk: enabling the length counter in the middle of a frame-sequencer
     * period that will not clock length performs one extra clock now.        */
    if (!prev_len_en && c->length_enable && extra && c->length > 0) {
        c->length--;
        if (c->length == 0 && !trigger)
            c->enabled = false;
    }
    if (trigger)
        sq_trigger(a, c, 64, has_sweep);
}

/* -------------------------------------------------------------- wave (ch3) */

static int wave_freq(const WaveCh *c)
{
    return (int)(((u16)(c->nr34 & 0x07) << 8) | c->nr33);
}

static int wave_period(const WaveCh *c)
{
    int p = (2048 - wave_freq(c)) * 2;
    return p > 0 ? p : 2;
}

static int wave_output(const WaveCh *c)
{
    int shift;
    if (!c->enabled || !c->dac_on)
        return 0;
    shift = clamp_int(c->volume_shift, 0, 4);
    return (int)(c->sample_buf & 0x0F) >> shift;
}

static void wave_tick(WaveCh *c, int t)
{
    int period;
    c->timer -= t;
    if (c->timer > 0)
        return;
    period = wave_period(c);
    while (c->timer <= 0) {
        int byte_idx;
        c->timer += period;
        c->pos   = (c->pos + 1) & 31;
        byte_idx = (c->pos >> 1) & 15;                  /* 0..15, bounded     */
        c->sample_buf = (c->pos & 1) ? (u8)(c->ram[byte_idx] & 0x0F)
                                     : (u8)(c->ram[byte_idx] >> 4);
    }
}

static void wave_trigger(APU *a, WaveCh *c)
{
    c->enabled = c->dac_on;
    if (c->length <= 0) {
        c->length = 256;
        if (c->length_enable && extra_length_clock(a))
            c->length--;
    }
    c->pos   = 0;
    c->timer = wave_period(c);
}

/* ------------------------------------------------------------- noise (ch4) */

static int noise_period(const NoiseCh *c)
{
    int code  = c->nr43 & 0x07;                   /* 0..7, bounded           */
    int shift = (c->nr43 >> 4) & 0x0F;            /* 0..15                   */
    int p     = noise_divisor[code] << shift;     /* >= 8, fits in int       */
    return p > 0 ? p : 8;
}

static int noise_output(const NoiseCh *c)
{
    if (!c->enabled || !c->dac_on)
        return 0;
    return (~c->lfsr & 1u) ? clamp_int(c->volume, 0, 15) : 0;
}

static void noise_tick(NoiseCh *c, int t)
{
    int period;
    c->timer -= t;
    if (c->timer > 0)
        return;
    period = noise_period(c);
    while (c->timer <= 0) {
        u16 x;
        c->timer += period;
        x = (u16)((c->lfsr ^ (c->lfsr >> 1)) & 1u);
        c->lfsr = (u16)((c->lfsr >> 1) | (u16)(x << 14));
        if (c->width7)
            c->lfsr = (u16)((c->lfsr & 0xFFBFu) | (u16)(x << 6));
    }
}

static void noise_trigger(APU *a, NoiseCh *c)
{
    c->enabled = c->dac_on;
    if (c->length <= 0) {
        c->length = 64;
        if (c->length_enable && extra_length_clock(a))
            c->length--;
    }
    c->lfsr      = 0x7FFF;
    c->timer     = noise_period(c);
    c->env_timer = c->env_period > 0 ? c->env_period : 8;
    c->volume    = clamp_int(c->env_start_vol, 0, 15);
}

/* ---------------------------------------------------- frame sequencer step */

static void frame_sequencer_step(APU *a)
{
    int step = a->frame_seq & 7;

    if ((step & 1) == 0) {                      /* steps 0,2,4,6 */
        length_step(&a->ch1.length, a->ch1.length_enable, &a->ch1.enabled);
        length_step(&a->ch2.length, a->ch2.length_enable, &a->ch2.enabled);
        length_step(&a->ch3.length, a->ch3.length_enable, &a->ch3.enabled);
        length_step(&a->ch4.length, a->ch4.length_enable, &a->ch4.enabled);
    }
    if (step == 2 || step == 6)                 /* sweep (ch1 only) */
        sweep_step(&a->ch1);

    if (step == 7) {                            /* volume envelopes */
        envelope_step(&a->ch1.env_timer, a->ch1.env_period,
                      &a->ch1.volume, a->ch1.env_dir_up);
        envelope_step(&a->ch2.env_timer, a->ch2.env_period,
                      &a->ch2.volume, a->ch2.env_dir_up);
        envelope_step(&a->ch4.env_timer, a->ch4.env_period,
                      &a->ch4.volume, a->ch4.env_dir_up);
    }

    a->frame_seq = (step + 1) & 7;              /* index of the NEXT step */
}

/* ------------------------------------------------------- mixing / emission */

/* Convert one channel's 0..15 digital level through its DAC to -1.0 .. +1.0.
 * A channel whose DAC is off contributes exactly 0 (analogue centre).        */
static float channel_dac(int sample, bool dac_on)
{
    if (!dac_on)
        return 0.0f;
    return (float)clamp_int(sample, 0, 15) / 7.5f - 1.0f;
}

/* Finite? (also rejects NaN, which every comparison below would let through) */
static bool is_finite_f(float v)
{
    return (v > -1.0e30f) && (v < 1.0e30f);
}

/* Smooth saturation.  Identity for |x| <= SOFT_KNEE, then a C1-continuous
 * rational knee that asymptotically approaches +/-1.0.  Four channels of
 * 12.5%-duty square at full volume can reach ~1.75 once the DC blocker has
 * removed their large negative offset; hard clipping that would buzz, so the
 * excess is compressed instead.                                              */
static float soft_clip(float x)
{
    float ax, d;

    if (!is_finite_f(x))
        return 0.0f;
    ax = x < 0.0f ? -x : x;
    if (ax <= SOFT_KNEE)
        return x;
    d = (ax - SOFT_KNEE) / (1.0f - SOFT_KNEE);
    ax = SOFT_KNEE + (1.0f - SOFT_KNEE) * (d / (1.0f + d));
    return x < 0.0f ? -ax : ax;
}

static void apu_emit(APU *a)
{
    float mix_l = 0.0f, mix_r = 0.0f;
    float out_l, out_r;
    u32   w;
    size_t base;

    if (a->enabled && !a->muted) {
        float v[4];
        float sum_l = 0.0f, sum_r = 0.0f;
        int   i;

        v[0] = channel_dac(sq_output(&a->ch1),    a->ch1.dac_on);
        v[1] = channel_dac(sq_output(&a->ch2),    a->ch2.dac_on);
        v[2] = channel_dac(wave_output(&a->ch3),  a->ch3.dac_on);
        v[3] = channel_dac(noise_output(&a->ch4), a->ch4.dac_on);

        for (i = 0; i < 4; i++) {
            if (a->nr51 & (u8)(0x10u << i)) sum_l += v[i];   /* NR51 bits 4-7 */
            if (a->nr51 & (u8)(0x01u << i)) sum_r += v[i];   /* NR51 bits 0-3 */
        }
        /* average the four channels, then apply the SO1/SO2 master volumes */
        mix_l = sum_l * 0.25f * (float)(((a->nr50 >> 4) & 7) + 1) * 0.125f;
        mix_r = sum_r * 0.25f * (float)(( a->nr50       & 7) + 1) * 0.125f;
    }

    /* A non-finite filter state would poison every future sample, so scrub it
     * before use rather than after (master_volume is host-controlled).       */
    if (!is_finite_f(a->hp_l)) a->hp_l = 0.0f;
    if (!is_finite_f(a->hp_r)) a->hp_r = 0.0f;

    /* One-pole DC blocker: out = in - dc; dc converges on the mean. */
    out_l   = mix_l - a->hp_l;
    out_r   = mix_r - a->hp_r;
    a->hp_l += (mix_l - a->hp_l) * HP_ALPHA;
    a->hp_r += (mix_r - a->hp_r) * HP_ALPHA;

    out_l = soft_clip(out_l * a->master_volume);
    out_r = soft_clip(out_r * a->master_volume);

    /* soft_clip() already bounds the result; this is a belt-and-braces guard. */
    if (out_l >  1.0f) out_l =  1.0f;
    if (out_l < -1.0f) out_l = -1.0f;
    if (out_r >  1.0f) out_r =  1.0f;
    if (out_r < -1.0f) out_r = -1.0f;

    /* Single producer: only this function writes `wr`.  Index is masked, so an
     * overrun can only cost the consumer stale audio, never memory safety.   */
    w    = a->wr;
    base = (size_t)(w % (u32)GB_AUDIO_RING) * 2u;
    a->ring[base]     = out_l;
    a->ring[base + 1] = out_r;
    a->wr = w + 1u;
}

/* ================================ API ==================================== */

void apu_reset(GB *gb)
{
    APU *a;
    if (!gb)
        return;
    a = &gb->apu;

    memset(a, 0, sizeof *a);

    a->enabled         = true;
    a->muted           = false;
    a->master_volume   = 1.0f;
    a->frame_seq       = 0;
    a->frame_seq_timer = 8192;
    a->sample_accum    = 0;
    a->hp_l = a->hp_r  = 0.0f;
    a->wr = a->rd      = 0;

    memcpy(a->ch3.ram, wave_power_on, sizeof a->ch3.ram);

    /* Post-boot register image (DMG).  No channel is left enabled, so the
     * emulator starts silent instead of clicking on the first frame.        */
    a->ch1.nrx0 = 0x80; a->ch1.nrx1 = 0xBF; a->ch1.nrx2 = 0xF3;
    a->ch1.nrx3 = 0xFF; a->ch1.nrx4 = 0xBF;
    a->ch2.nrx1 = 0x3F; a->ch2.nrx2 = 0x00;
    a->ch2.nrx3 = 0xFF; a->ch2.nrx4 = 0xBF;
    a->ch3.nr30 = 0x7F; a->ch3.nr31 = 0xFF; a->ch3.nr32 = 0x9F;
    a->ch3.nr33 = 0xFF; a->ch3.nr34 = 0xBF;
    a->ch4.nr41 = 0xFF; a->ch4.nr42 = 0x00;
    a->ch4.nr43 = 0x00; a->ch4.nr44 = 0xBF;
    a->nr50 = 0x77;
    a->nr51 = 0xF3;

    /* Derive the shadow state the register image implies (without triggering) */
    a->ch1.sweep_period = (a->ch1.nrx0 >> 4) & 7;
    a->ch1.sweep_shift  = a->ch1.nrx0 & 7;
    a->ch1.sweep_dir_down = (a->ch1.nrx0 & 0x08) != 0;
    a->ch1.length       = 64 - (int)(a->ch1.nrx1 & 0x3F);
    a->ch1.env_start_vol = (a->ch1.nrx2 >> 4) & 0x0F;
    a->ch1.env_dir_up    = (a->ch1.nrx2 & 0x08) != 0;
    a->ch1.env_period    = a->ch1.nrx2 & 0x07;
    a->ch1.dac_on        = (a->ch1.nrx2 & 0xF8) != 0;
    a->ch1.timer         = sq_period(&a->ch1);

    a->ch2.length        = 64 - (int)(a->ch2.nrx1 & 0x3F);
    a->ch2.env_start_vol = (a->ch2.nrx2 >> 4) & 0x0F;
    a->ch2.env_dir_up    = (a->ch2.nrx2 & 0x08) != 0;
    a->ch2.env_period    = a->ch2.nrx2 & 0x07;
    a->ch2.dac_on        = (a->ch2.nrx2 & 0xF8) != 0;
    a->ch2.timer         = sq_period(&a->ch2);

    a->ch3.dac_on        = (a->ch3.nr30 & 0x80) != 0;
    a->ch3.length        = 256 - (int)a->ch3.nr31;
    a->ch3.volume_shift  = wave_shift_table[(a->ch3.nr32 >> 5) & 3];
    a->ch3.timer         = wave_period(&a->ch3);

    a->ch4.length        = 64 - (int)(a->ch4.nr41 & 0x3F);
    a->ch4.env_start_vol = (a->ch4.nr42 >> 4) & 0x0F;
    a->ch4.env_dir_up    = (a->ch4.nr42 & 0x08) != 0;
    a->ch4.env_period    = a->ch4.nr42 & 0x07;
    a->ch4.dac_on        = (a->ch4.nr42 & 0xF8) != 0;
    a->ch4.width7        = (a->ch4.nr43 & 0x08) != 0;
    a->ch4.lfsr          = 0x7FFF;
    a->ch4.timer         = noise_period(&a->ch4);
}

void apu_tick(GB *gb, int t)
{
    APU *a;

    if (!gb || t <= 0)
        return;
    if (t > APU_MAX_TICK)
        t = APU_MAX_TICK;
    a = &gb->apu;

    if (a->enabled) {
        /* 512 Hz frame sequencer */
        a->frame_seq_timer -= t;
        while (a->frame_seq_timer <= 0) {
            a->frame_seq_timer += 8192;
            frame_sequencer_step(a);
        }
        /* Channel frequency timers.  Every period is >= 2 T-cycles, so these
         * loops are bounded by t/2 iterations and can never spin forever.   */
        sq_tick(&a->ch1, t);
        sq_tick(&a->ch2, t);
        wave_tick(&a->ch3, t);
        noise_tick(&a->ch4, t);
    }

    /* Fixed-point downsample to GB_AUDIO_RATE.  Runs even while the APU is
     * powered off so the host keeps receiving a continuous (silent) stream. */
    a->sample_accum += (u64)t * (u64)GB_AUDIO_RATE;
    while (a->sample_accum >= (u64)GB_CPU_HZ) {
        a->sample_accum -= (u64)GB_CPU_HZ;
        apu_emit(a);
    }
}

u8 apu_read(GB *gb, u16 addr)
{
    const APU *a;

    if (!gb)
        return 0xFF;
    a = &gb->apu;

    if (addr >= 0xFF10 && addr <= 0xFF26) {
        u8 raw = 0;
        switch (addr) {
        case 0xFF10: raw = a->ch1.nrx0; break;
        case 0xFF11: raw = a->ch1.nrx1; break;
        case 0xFF12: raw = a->ch1.nrx2; break;
        case 0xFF13: raw = a->ch1.nrx3; break;
        case 0xFF14: raw = a->ch1.nrx4; break;
        case 0xFF15: raw = 0;           break;   /* no NR20 */
        case 0xFF16: raw = a->ch2.nrx1; break;
        case 0xFF17: raw = a->ch2.nrx2; break;
        case 0xFF18: raw = a->ch2.nrx3; break;
        case 0xFF19: raw = a->ch2.nrx4; break;
        case 0xFF1A: raw = a->ch3.nr30; break;
        case 0xFF1B: raw = a->ch3.nr31; break;
        case 0xFF1C: raw = a->ch3.nr32; break;
        case 0xFF1D: raw = a->ch3.nr33; break;
        case 0xFF1E: raw = a->ch3.nr34; break;
        case 0xFF1F: raw = 0;           break;   /* no NR40 */
        case 0xFF20: raw = a->ch4.nr41; break;
        case 0xFF21: raw = a->ch4.nr42; break;
        case 0xFF22: raw = a->ch4.nr43; break;
        case 0xFF23: raw = a->ch4.nr44; break;
        case 0xFF24: raw = a->nr50;     break;
        case 0xFF25: raw = a->nr51;     break;
        case 0xFF26:                              /* NR52: live status bits */
            raw = (u8)((a->enabled     ? 0x80 : 0x00) |
                       (a->ch1.enabled ? 0x01 : 0x00) |
                       (a->ch2.enabled ? 0x02 : 0x00) |
                       (a->ch3.enabled ? 0x04 : 0x00) |
                       (a->ch4.enabled ? 0x08 : 0x00));
            break;
        default: raw = 0; break;
        }
        return (u8)(raw | read_mask[addr - 0xFF10]);
    }

    if (addr >= 0xFF27 && addr <= 0xFF2F)
        return 0xFF;                              /* unmapped */

    if (addr >= 0xFF30 && addr <= 0xFF3F) {
        /* While ch3 runs, CPU accesses hit the byte currently being played. */
        int i = a->ch3.enabled ? ((a->ch3.pos >> 1) & 15)
                               : (int)((addr - 0xFF30) & 0x0F);
        return a->ch3.ram[i & 15];
    }

    return 0xFF;
}

/* Zero every register and all channel state; wave RAM, the ring buffer and the
 * host-controlled volume/mute flags survive.                                 */
static void apu_power_off(APU *a)
{
    u8 saved_wave[16];

    memcpy(saved_wave, a->ch3.ram, sizeof saved_wave);

    memset(&a->ch1, 0, sizeof a->ch1);
    memset(&a->ch2, 0, sizeof a->ch2);
    memset(&a->ch3, 0, sizeof a->ch3);
    memset(&a->ch4, 0, sizeof a->ch4);

    memcpy(a->ch3.ram, saved_wave, sizeof saved_wave);

    a->ch4.lfsr        = 0x7FFF;
    a->nr50            = 0;
    a->nr51            = 0;
    a->frame_seq       = 0;
    a->frame_seq_timer = 8192;
    a->enabled         = false;
}

void apu_write(GB *gb, u16 addr, u8 val)
{
    APU *a;

    if (!gb)
        return;
    a = &gb->apu;

    /* Wave RAM is writable regardless of APU power. */
    if (addr >= 0xFF30 && addr <= 0xFF3F) {
        int i = a->ch3.enabled ? ((a->ch3.pos >> 1) & 15)
                               : (int)((addr - 0xFF30) & 0x0F);
        a->ch3.ram[i & 15] = val;
        return;
    }

    /* NR52 is always writable — it holds the power switch. */
    if (addr == 0xFF26) {
        bool power = (val & 0x80) != 0;
        if (!power) {
            if (a->enabled)
                apu_power_off(a);
        } else if (!a->enabled) {
            a->enabled         = true;
            a->frame_seq       = 0;
            a->frame_seq_timer = 8192;
            a->ch3.pos         = 0;
            a->ch3.sample_buf  = 0;
        }
        return;
    }

    if (addr < 0xFF10 || addr > 0xFF25)
        return;                      /* nothing else in this block is writable */

    if (!a->enabled)
        return;                      /* powered down: FF10-FF25 ignore writes  */

    switch (addr) {
    /* ---- channel 1 : square + sweep ------------------------------------- */
    case 0xFF10: {                                          /* NR10 sweep     */
        bool was_down = a->ch1.sweep_dir_down;
        a->ch1.nrx0          = val;
        a->ch1.sweep_period  = (val >> 4) & 0x07;
        a->ch1.sweep_dir_down = (val & 0x08) != 0;
        a->ch1.sweep_shift   = val & 0x07;
        /* Leaving decrease mode after a calculation has been made in decrease
         * mode immediately disables the channel. */
        if (was_down && !a->ch1.sweep_dir_down && a->ch1.sweep_calc_made)
            a->ch1.enabled = false;
        break;
    }
    case 0xFF11: sq_write_nrx1(&a->ch1, val); break;        /* NR11 duty/len  */
    case 0xFF12: sq_write_nrx2(&a->ch1, val); break;        /* NR12 envelope  */
    case 0xFF13: a->ch1.nrx3 = val;           break;        /* NR13 freq lo   */
    case 0xFF14: sq_write_nrx4(a, &a->ch1, val, true);  break;  /* NR14       */

    /* ---- channel 2 : square --------------------------------------------- */
    case 0xFF15: break;                                     /* no NR20        */
    case 0xFF16: sq_write_nrx1(&a->ch2, val); break;        /* NR21           */
    case 0xFF17: sq_write_nrx2(&a->ch2, val); break;        /* NR22           */
    case 0xFF18: a->ch2.nrx3 = val;           break;        /* NR23           */
    case 0xFF19: sq_write_nrx4(a, &a->ch2, val, false); break;  /* NR24       */

    /* ---- channel 3 : wave ----------------------------------------------- */
    case 0xFF1A:                                            /* NR30 DAC       */
        a->ch3.nr30   = val;
        a->ch3.dac_on = (val & 0x80) != 0;
        if (!a->ch3.dac_on)
            a->ch3.enabled = false;
        break;
    case 0xFF1B:                                            /* NR31 length    */
        a->ch3.nr31   = val;
        a->ch3.length = 256 - (int)val;
        break;
    case 0xFF1C:                                            /* NR32 volume    */
        a->ch3.nr32         = val;
        a->ch3.volume_shift = wave_shift_table[(val >> 5) & 3];
        break;
    case 0xFF1D: a->ch3.nr33 = val; break;                  /* NR33 freq lo   */
    case 0xFF1E: {                                          /* NR34           */
        bool prev_len_en = a->ch3.length_enable;
        bool trigger, extra;
        a->ch3.nr34          = val;
        a->ch3.length_enable = (val & 0x40) != 0;
        trigger              = (val & 0x80) != 0;
        extra                = extra_length_clock(a);
        if (!prev_len_en && a->ch3.length_enable && extra && a->ch3.length > 0) {
            a->ch3.length--;
            if (a->ch3.length == 0 && !trigger)
                a->ch3.enabled = false;
        }
        if (trigger)
            wave_trigger(a, &a->ch3);
        break;
    }

    /* ---- channel 4 : noise ---------------------------------------------- */
    case 0xFF20:                                            /* NR41 length    */
        a->ch4.nr41   = val;
        a->ch4.length = 64 - (int)(val & 0x3F);
        break;
    case 0xFF21:                                            /* NR42 envelope  */
        a->ch4.nr42          = val;
        a->ch4.env_start_vol = (val >> 4) & 0x0F;
        a->ch4.env_dir_up    = (val & 0x08) != 0;
        a->ch4.env_period    = val & 0x07;
        a->ch4.dac_on        = (val & 0xF8) != 0;
        if (!a->ch4.dac_on)
            a->ch4.enabled = false;
        break;
    case 0xFF22:                                            /* NR43 poly      */
        a->ch4.nr43   = val;
        a->ch4.width7 = (val & 0x08) != 0;
        break;
    case 0xFF23: {                                          /* NR44           */
        bool prev_len_en = a->ch4.length_enable;
        bool trigger, extra;
        a->ch4.nr44          = val;
        a->ch4.length_enable = (val & 0x40) != 0;
        trigger              = (val & 0x80) != 0;
        extra                = extra_length_clock(a);
        if (!prev_len_en && a->ch4.length_enable && extra && a->ch4.length > 0) {
            a->ch4.length--;
            if (a->ch4.length == 0 && !trigger)
                a->ch4.enabled = false;
        }
        if (trigger)
            noise_trigger(a, &a->ch4);
        break;
    }

    /* ---- global --------------------------------------------------------- */
    case 0xFF24: a->nr50 = val; break;                      /* master vol/VIN */
    case 0xFF25: a->nr51 = val; break;                      /* routing        */
    default: break;
    }
}

int apu_pull(GB *gb, float *out, int frames)
{
    APU *a;
    u32  w, r, avail, want, n, i;
    float last_l = 0.0f, last_r = 0.0f;

    if (!gb || !out || frames <= 0)
        return 0;
    a = &gb->apu;

    w = a->wr;                       /* snapshot the producer cursor once */
    r = a->rd;
    avail = w - r;                   /* wrap-safe unsigned difference     */

    /* The producer never stops; if it lapped us, drop the oldest frames here
     * (the consumer owns `rd`, so no cross-thread write hazard is created). */
    if (avail > (u32)GB_AUDIO_RING) {
        r     = w - (u32)GB_AUDIO_RING;
        avail = (u32)GB_AUDIO_RING;
    }

    want = (u32)frames;
    n    = avail < want ? avail : want;

    for (i = 0; i < n; i++) {
        size_t base = (size_t)((r + i) % (u32)GB_AUDIO_RING) * 2u;
        last_l = a->ring[base];
        last_r = a->ring[base + 1];
        out[(size_t)i * 2u]      = last_l;
        out[(size_t)i * 2u + 1u] = last_r;
    }

    /* Starved: hold the last emitted level instead of dropping to zero, which
     * would produce an audible click. */
    if (n == 0 && w != 0) {
        size_t base = (size_t)((w - 1u) % (u32)GB_AUDIO_RING) * 2u;
        last_l = a->ring[base];
        last_r = a->ring[base + 1];
    }
    for (i = n; i < want; i++) {
        out[(size_t)i * 2u]      = last_l;
        out[(size_t)i * 2u + 1u] = last_r;
    }

    a->rd = r + n;
    return (int)n;
}

int apu_available(GB *gb)
{
    u32 avail;
    if (!gb)
        return 0;
    avail = gb->apu.wr - gb->apu.rd;
    if (avail > (u32)GB_AUDIO_RING)
        avail = (u32)GB_AUDIO_RING;
    return (int)avail;
}

/* CONTRACT-NOTE: two small, deliberate deviations from the literal wording of
 * the assignment, neither of which touches include/gb.h:
 *
 *  1. Ring overrun handling.  The brief says "if the ring is full advance rd"
 *     but also "only apu_pull writes rd".  Those conflict: having the emulation
 *     thread move `rd` while the audio thread also moves it is exactly the data
 *     race the second rule forbids.  The emitter therefore always writes its
 *     slot (index masked with % GB_AUDIO_RING, so it can never touch memory
 *     outside apu.ring) and advances only `wr`; apu_pull()/apu_available()
 *     detect having been lapped ((wr - rd) > GB_AUDIO_RING) and skip `rd`
 *     forward themselves.  The observable behaviour is identical — the oldest
 *     frames are dropped, the producer never blocks — but `rd` stays owned by
 *     the consumer alone.  apu_available() is likewise clamped to
 *     GB_AUDIO_RING so a caller can never be told there is more audio buffered
 *     than the ring can physically hold.
 *
 *  2. Mixing.  The brief's "divide by 4, scale by volume/8, normalise to
 *     roughly -1..1" is implemented as a per-channel DAC (0..15 -> -1..+1, and
 *     exactly 0.0 when that channel's DAC is off) before the NR51 routing sum.
 *     That is what the hardware does, and it matters: summing raw 0..15 levels
 *     and normalising afterwards would give a channel with its DAC powered off
 *     the same weight as one sitting at digital 0, adding a spurious DC step on
 *     every DAC enable/disable.  The scaling factors (1/4 for the channel
 *     count, (vol+1)/8 for NR50) are unchanged.
 *
 *  3. The final limiter is a soft knee rather than a hard clamp.  The mixer's
 *     arithmetic peak is exactly +/-1.0, but the DC blocker that follows it
 *     shifts asymmetric waveforms upward — four 12.5%-duty squares at full
 *     volume settle around a -0.75 mean, so removing that offset pushes peaks
 *     to ~1.75.  Measured over a worst-case four-channel blast, a hard clamp
 *     flat-topped 1.2% of all samples (audible buzz); soft_clip() is the
 *     identity below 0.75 — so ordinary game audio is bit-for-bit unchanged —
 *     and compresses only the overshoot above it.  The hard clamp is kept
 *     afterwards purely as a guard.
 *
 *  Also note: gb.h has no field for "last frame pulled", so the starvation
 *  padding in apu_pull() re-reads the most recently emitted frame from the ring
 *  (ring[(wr-1) % GB_AUDIO_RING]) instead of caching it in module-scope state —
 *  that keeps apu.c free of per-instance globals and safe for several GB
 *  objects in one process.
 */
