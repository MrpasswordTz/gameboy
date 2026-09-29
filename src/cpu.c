/* =============================================================================
 *  cpu.c — Sharp SM83 (LR35902) CPU core.
 *
 *  Timing model (see gb.h):
 *    - mmu_read()/mmu_write() each perform exactly one M-cycle: they call
 *      gb_tick(gb, 4) BEFORE the bus access.  Therefore this file never adds
 *      cycles for memory traffic; it only calls gb_tick(gb, 4) for the CPU's
 *      *internal* M-cycles (16-bit INC/DEC, ADD HL,rr, PUSH, taken branches,
 *      LD SP,HL, the SP+r8 adder, ...).
 *    - Every entry point measures its own duration as the delta of gb->cycles,
 *      so the returned T-cycle count can never disagree with what was actually
 *      ticked into the rest of the machine.
 *
 *  All 256 base opcodes are implemented explicitly.  The 0xCB page lives in
 *  cpu_cb.c and does its own fetching/ticking.
 * ========================================================================== */

#include <string.h>

#include "gb.h"

/* ============================ small utilities ============================= */

/* T-cycles consumed since `start`, as seen by the rest of the machine. */
static int elapsed(const GB *gb, u64 start)
{
    u64 d = gb->cycles - start;
    /* A step always performs at least one opcode fetch (4 T).  The guard only
     * exists so a mis-wired gb_tick() can never make the frame loop spin. */
    return d != 0 ? (int)d : 4;
}

static void flags_set(CPU *c, bool z, bool n, bool h, bool cy)
{
    c->f = (u8)((z ? FLAG_Z : 0) | (n ? FLAG_N : 0) |
                (h ? FLAG_H : 0) | (cy ? FLAG_C : 0));
}

static bool flag_c(const CPU *c) { return (c->f & FLAG_C) != 0; }
static bool flag_z(const CPU *c) { return (c->f & FLAG_Z) != 0; }

/* gb.h's BC(x) macro expands to rp_get((x)->b, (x)->c): the preprocessor also
 * rewrites the *member* name `c`, so the macro only works when its argument is
 * literally spelled `c`.  Use this accessor instead of BC() everywhere.
 * (DE/HL/AF are unaffected — none of their members collide with the parameter.) */
static u16 reg_bc(const CPU *c) { return rp_get(c->b, c->c); }

/* ------------------------------- fetching -------------------------------- */

/* Operand fetch: always advances PC.  Ticks 4 T via mmu_read(). */
static u8 fetch8(GB *gb)
{
    u8 v = mmu_read(gb, gb->cpu.pc);
    gb->cpu.pc = (u16)(gb->cpu.pc + 1);
    return v;
}

static u16 fetch16(GB *gb)
{
    u8 lo = fetch8(gb);
    u8 hi = fetch8(gb);
    return (u16)((u16)hi << 8 | lo);
}

/* Opcode fetch: honours (and consumes) the HALT bug, where PC fails to
 * increment once so the byte following HALT is read twice. */
static u8 fetch_opcode(GB *gb)
{
    u8 v = mmu_read(gb, gb->cpu.pc);
    if (gb->cpu.halt_bug)
        gb->cpu.halt_bug = false;
    else
        gb->cpu.pc = (u16)(gb->cpu.pc + 1);
    return v;
}

/* ------------------------------- stack ----------------------------------- */

static void push16(GB *gb, u16 v)
{
    CPU *c = &gb->cpu;
    c->sp = (u16)(c->sp - 1);
    mmu_write(gb, c->sp, (u8)(v >> 8));
    c->sp = (u16)(c->sp - 1);
    mmu_write(gb, c->sp, (u8)(v & 0xFF));
}

static u16 pop16(GB *gb)
{
    CPU *c = &gb->cpu;
    u8 lo = mmu_read(gb, c->sp);
    c->sp = (u16)(c->sp + 1);
    u8 hi = mmu_read(gb, c->sp);
    c->sp = (u16)(c->sp + 1);
    return (u16)((u16)hi << 8 | lo);
}

/* ============================ 8-bit ALU =================================== */

static void alu_add(CPU *c, u8 v)
{
    u16 r = (u16)((u16)c->a + v);
    flags_set(c, (r & 0xFF) == 0, false,
              ((c->a & 0x0F) + (v & 0x0F)) > 0x0F, r > 0xFF);
    c->a = (u8)r;
}

static void alu_adc(CPU *c, u8 v)
{
    unsigned cy = flag_c(c) ? 1u : 0u;
    unsigned r  = (unsigned)c->a + v + cy;
    flags_set(c, (r & 0xFF) == 0, false,
              ((unsigned)(c->a & 0x0F) + (v & 0x0F) + cy) > 0x0F, r > 0xFF);
    c->a = (u8)r;
}

static void alu_sub(CPU *c, u8 v)
{
    u8 r = (u8)(c->a - v);
    flags_set(c, r == 0, true, (c->a & 0x0F) < (v & 0x0F), c->a < v);
    c->a = r;
}

static void alu_sbc(CPU *c, u8 v)
{
    int cy = flag_c(c) ? 1 : 0;
    int r  = (int)c->a - (int)v - cy;
    flags_set(c, (u8)r == 0, true,
              ((int)(c->a & 0x0F) - (int)(v & 0x0F) - cy) < 0, r < 0);
    c->a = (u8)r;
}

static void alu_and(CPU *c, u8 v)
{
    c->a = (u8)(c->a & v);
    flags_set(c, c->a == 0, false, true, false);
}

static void alu_xor(CPU *c, u8 v)
{
    c->a = (u8)(c->a ^ v);
    flags_set(c, c->a == 0, false, false, false);
}

static void alu_or(CPU *c, u8 v)
{
    c->a = (u8)(c->a | v);
    flags_set(c, c->a == 0, false, false, false);
}

/* CP is SUB without writing the result back. */
static void alu_cp(CPU *c, u8 v)
{
    flags_set(c, c->a == v, true, (c->a & 0x0F) < (v & 0x0F), c->a < v);
}

static u8 alu_inc8(CPU *c, u8 v)
{
    u8 r = (u8)(v + 1);
    c->f = (u8)((c->f & FLAG_C)
                | (r == 0 ? FLAG_Z : 0)
                | ((v & 0x0F) == 0x0F ? FLAG_H : 0));
    return r;
}

static u8 alu_dec8(CPU *c, u8 v)
{
    u8 r = (u8)(v - 1);
    c->f = (u8)((c->f & FLAG_C) | FLAG_N
                | (r == 0 ? FLAG_Z : 0)
                | ((v & 0x0F) == 0x00 ? FLAG_H : 0));
    return r;
}

/* ADD HL,rr — Z untouched, H from bit 11, C from bit 15.  One internal cycle. */
static void alu_add16(GB *gb, u16 v)
{
    CPU *c = &gb->cpu;
    u16 hl = HL(c);
    u32 r  = (u32)hl + v;
    c->f = (u8)((c->f & FLAG_Z)
                | (((hl & 0x0FFF) + (v & 0x0FFF)) > 0x0FFF ? FLAG_H : 0)
                | (r > 0xFFFFu ? FLAG_C : 0));
    set_hl(c, (u16)r);
    gb_tick(gb, 4);
}

/* Shared operand+flag work of 0xE8 (ADD SP,r8) and 0xF8 (LD HL,SP+r8).
 * Z and N are cleared; H and C come from the LOW BYTE addition. */
static u16 alu_sp_offset(GB *gb)
{
    CPU *c   = &gb->cpu;
    u8   raw = fetch8(gb);            /* 4 T */
    s8   off = (s8)raw;
    u16  sp  = c->sp;
    flags_set(c, false, false,
              ((sp & 0x0F) + (raw & 0x0F)) > 0x0F,
              ((sp & 0xFF) + raw) > 0xFF);
    return (u16)((int)sp + (int)off);
}

/* Decimal adjust after an 8-bit BCD add/sub.  C is only ever set here. */
static void alu_daa(CPU *c)
{
    u8  f = c->f;
    int a = c->a;

    if (!(f & FLAG_N)) {
        if ((f & FLAG_C) || a > 0x99) { a += 0x60; f = (u8)(f | FLAG_C); }
        if ((f & FLAG_H) || (a & 0x0F) > 0x09) { a += 0x06; }
    } else {
        if (f & FLAG_C) a -= 0x60;
        if (f & FLAG_H) a -= 0x06;
    }
    a &= 0xFF;
    f = (u8)(f & ~(FLAG_H | FLAG_Z));
    if (a == 0) f = (u8)(f | FLAG_Z);
    c->a = (u8)a;
    c->f = f;
}

/* ------------- accumulator rotates (Z is ALWAYS cleared, unlike CB) ------- */

static void alu_rlca(CPU *c)
{
    u8 cy = (u8)(c->a >> 7);
    c->a  = (u8)((c->a << 1) | cy);
    flags_set(c, false, false, false, cy != 0);
}

static void alu_rrca(CPU *c)
{
    u8 cy = (u8)(c->a & 0x01);
    c->a  = (u8)((c->a >> 1) | (u8)(cy << 7));
    flags_set(c, false, false, false, cy != 0);
}

static void alu_rla(CPU *c)
{
    u8 in = flag_c(c) ? 1u : 0u;
    u8 cy = (u8)(c->a >> 7);
    c->a  = (u8)((c->a << 1) | in);
    flags_set(c, false, false, false, cy != 0);
}

static void alu_rra(CPU *c)
{
    u8 in = flag_c(c) ? 0x80u : 0x00u;
    u8 cy = (u8)(c->a & 0x01);
    c->a  = (u8)((c->a >> 1) | in);
    flags_set(c, false, false, false, cy != 0);
}

/* ========================== control transfer ============================== */

/* JR cc,r8 — 8 T not taken, 12 T taken. */
static void op_jr(GB *gb, bool cond)
{
    s8 off = (s8)fetch8(gb);
    if (cond) {
        gb->cpu.pc = (u16)((int)gb->cpu.pc + (int)off);
        gb_tick(gb, 4);
    }
}

/* JP cc,a16 — 12 T not taken, 16 T taken. */
static void op_jp(GB *gb, bool cond)
{
    u16 target = fetch16(gb);
    if (cond) {
        gb->cpu.pc = target;
        gb_tick(gb, 4);
    }
}

/* CALL cc,a16 — 12 T not taken, 24 T taken. */
static void op_call(GB *gb, bool cond)
{
    u16 target = fetch16(gb);
    if (cond) {
        gb_tick(gb, 4);
        push16(gb, gb->cpu.pc);
        gb->cpu.pc = target;
    }
}

/* RET / RETI — 16 T. */
static void op_ret(GB *gb)
{
    gb->cpu.pc = pop16(gb);
    gb_tick(gb, 4);
}

/* RET cc — 8 T not taken, 20 T taken. */
static void op_ret_cond(GB *gb, bool cond)
{
    gb_tick(gb, 4);                    /* condition evaluation M-cycle */
    if (cond)
        op_ret(gb);
}

/* RST n — 16 T. */
static void op_rst(GB *gb, u16 vec)
{
    gb_tick(gb, 4);
    push16(gb, gb->cpu.pc);
    gb->cpu.pc = vec;
}

/* ============================== reset ==================================== */

void cpu_reset(GB *gb)
{
    CPU *c;

    if (!gb) return;
    c = &gb->cpu;
    memset(c, 0, sizeof *c);

    if (gb->mmu.boot_active) {
        /* A real boot ROM was supplied: it sets up everything itself. */
        c->pc = 0x0000;
        c->sp = 0x0000;
        return;
    }

    if (gb->cgb_hw) {
        c->a = 0x11; c->f = 0x80;
        c->b = 0x00; c->c = 0x00;
        c->d = 0xFF; c->e = 0x56;
        c->h = 0x00; c->l = 0x0D;
    } else {
        c->a = 0x01; c->f = 0xB0;
        c->b = 0x00; c->c = 0x13;
        c->d = 0x00; c->e = 0xD8;
        c->h = 0x01; c->l = 0x4D;
    }
    c->sp = 0xFFFE;
    c->pc = 0x0100;
}

/* ============================ interrupts ================================= */

int cpu_handle_interrupts(GB *gb)
{
    u64  start;
    CPU *c;
    u8   pending;
    u16  saved_pc;
    u16  vector;
    int  bit;

    if (!gb) return 0;

    start   = gb->cycles;
    c       = &gb->cpu;
    pending = (u8)(gb->mmu.ie & gb->mmu.ifr & 0x1F);

    /* HALT is released by a pending interrupt even when IME is clear. */
    if (pending != 0)
        c->halted = false;

    if (!c->ime || pending == 0)
        return 0;

    c->ime         = false;
    c->ime_pending = false;

    /* 5 M-cycles total: 2 internal, 2 stack writes, 1 for loading PC. */
    gb_tick(gb, 4);
    gb_tick(gb, 4);

    saved_pc = c->pc;
    c->sp = (u16)(c->sp - 1);
    mmu_write(gb, c->sp, (u8)(saved_pc >> 8));

    /* The high-byte push can land on 0xFFFF and rewrite IE, and the internal
     * cycles above may have raised or cleared IF.  Hardware latches the vector
     * here, after that write — if nothing is pending any more the dispatch is
     * "cancelled" and control goes to 0x0000. */
    pending = (u8)(gb->mmu.ie & gb->mmu.ifr & 0x1F);
    vector  = 0x0000;
    if (pending != 0) {
        for (bit = 0; bit < 5; bit++) {
            if (pending & (u8)(1u << bit)) {
                gb->mmu.ifr = (u8)(gb->mmu.ifr & ~(1u << bit));
                vector = (u16)(0x0040 + bit * 8);
                break;
            }
        }
    }

    c->sp = (u16)(c->sp - 1);
    mmu_write(gb, c->sp, (u8)(saved_pc & 0xFF));

    c->pc = vector;
    gb_tick(gb, 4);

    return elapsed(gb, start);
}

/* ============================== one step ================================= */

int cpu_step(GB *gb)
{
    u64  start;
    CPU *cp;
    bool ei_latch;
    u16  op_pc;
    u8   op;
    int  serviced;

    if (!gb) return 4;

    start = gb->cycles;
    cp    = &gb->cpu;

    /* ---------------------------------------------------------- STOP mode */
    if (cp->stopped) {
        if (gb->cgb_hw && gb->speed_switch) {
            int i;
            gb->speed_switch = false;
            cp->stopped      = false;
            gb->speed        = (gb->speed == 2) ? 1 : 2;
            /* The clock is halted for 2050 M-cycles while the PLL relocks. */
            for (i = 0; i < 2050; i++)
                gb_tick(gb, 4);
        } else {
            /* A joypad press wakes the CPU out of STOP. */
            if (gb->joypad.buttons != 0)
                cp->stopped = false;
            gb_tick(gb, 4);
        }
        return elapsed(gb, start);
    }

    /* -------------------------------------------------------- interrupts */
    serviced = cpu_handle_interrupts(gb);
    if (serviced != 0)
        return serviced;

    /* ------------------------------------------------------------- HALT  */
    if (cp->halted) {
        gb_tick(gb, 4);
        return elapsed(gb, start);
    }

    /* EI takes effect only *after* the instruction that follows it. */
    ei_latch = cp->ime_pending;

    op_pc = cp->pc;
    op    = fetch_opcode(gb);

    switch (op) {

    /* ------------------------------------------------------------ 0x0x */
    case 0x00: /* NOP */
        break;
    case 0x01: /* LD BC,d16 */
        set_bc(cp, fetch16(gb));
        break;
    case 0x02: /* LD (BC),A */
        mmu_write(gb, reg_bc(cp), cp->a);
        break;
    case 0x03: /* INC BC */
        set_bc(cp, (u16)(reg_bc(cp) + 1));
        gb_tick(gb, 4);
        break;
    case 0x04: /* INC B */
        cp->b = alu_inc8(cp, cp->b);
        break;
    case 0x05: /* DEC B */
        cp->b = alu_dec8(cp, cp->b);
        break;
    case 0x06: /* LD B,d8 */
        cp->b = fetch8(gb);
        break;
    case 0x07: /* RLCA */
        alu_rlca(cp);
        break;
    case 0x08: { /* LD (a16),SP — 20 T, low byte first */
        u16 addr = fetch16(gb);
        mmu_write(gb, addr, (u8)(cp->sp & 0xFF));
        mmu_write(gb, (u16)(addr + 1), (u8)(cp->sp >> 8));
    } break;
    case 0x09: /* ADD HL,BC */
        alu_add16(gb, reg_bc(cp));
        break;
    case 0x0A: /* LD A,(BC) */
        cp->a = mmu_read(gb, reg_bc(cp));
        break;
    case 0x0B: /* DEC BC */
        set_bc(cp, (u16)(reg_bc(cp) - 1));
        gb_tick(gb, 4);
        break;
    case 0x0C: /* INC C */
        cp->c = alu_inc8(cp, cp->c);
        break;
    case 0x0D: /* DEC C */
        cp->c = alu_dec8(cp, cp->c);
        break;
    case 0x0E: /* LD C,d8 */
        cp->c = fetch8(gb);
        break;
    case 0x0F: /* RRCA */
        alu_rrca(cp);
        break;

    /* ------------------------------------------------------------ 0x1x */
    case 0x10: /* STOP — 4 T; the following byte is skipped, not fetched */
        cp->pc      = (u16)(cp->pc + 1);
        cp->stopped = true;
        break;
    case 0x11: /* LD DE,d16 */
        set_de(cp, fetch16(gb));
        break;
    case 0x12: /* LD (DE),A */
        mmu_write(gb, DE(cp), cp->a);
        break;
    case 0x13: /* INC DE */
        set_de(cp, (u16)(DE(cp) + 1));
        gb_tick(gb, 4);
        break;
    case 0x14: /* INC D */
        cp->d = alu_inc8(cp, cp->d);
        break;
    case 0x15: /* DEC D */
        cp->d = alu_dec8(cp, cp->d);
        break;
    case 0x16: /* LD D,d8 */
        cp->d = fetch8(gb);
        break;
    case 0x17: /* RLA */
        alu_rla(cp);
        break;
    case 0x18: /* JR r8 */
        op_jr(gb, true);
        break;
    case 0x19: /* ADD HL,DE */
        alu_add16(gb, DE(cp));
        break;
    case 0x1A: /* LD A,(DE) */
        cp->a = mmu_read(gb, DE(cp));
        break;
    case 0x1B: /* DEC DE */
        set_de(cp, (u16)(DE(cp) - 1));
        gb_tick(gb, 4);
        break;
    case 0x1C: /* INC E */
        cp->e = alu_inc8(cp, cp->e);
        break;
    case 0x1D: /* DEC E */
        cp->e = alu_dec8(cp, cp->e);
        break;
    case 0x1E: /* LD E,d8 */
        cp->e = fetch8(gb);
        break;
    case 0x1F: /* RRA */
        alu_rra(cp);
        break;

    /* ------------------------------------------------------------ 0x2x */
    case 0x20: /* JR NZ,r8 */
        op_jr(gb, !flag_z(cp));
        break;
    case 0x21: /* LD HL,d16 */
        set_hl(cp, fetch16(gb));
        break;
    case 0x22: { /* LD (HL+),A */
        u16 hl = HL(cp);
        mmu_write(gb, hl, cp->a);
        set_hl(cp, (u16)(hl + 1));
    } break;
    case 0x23: /* INC HL */
        set_hl(cp, (u16)(HL(cp) + 1));
        gb_tick(gb, 4);
        break;
    case 0x24: /* INC H */
        cp->h = alu_inc8(cp, cp->h);
        break;
    case 0x25: /* DEC H */
        cp->h = alu_dec8(cp, cp->h);
        break;
    case 0x26: /* LD H,d8 */
        cp->h = fetch8(gb);
        break;
    case 0x27: /* DAA */
        alu_daa(cp);
        break;
    case 0x28: /* JR Z,r8 */
        op_jr(gb, flag_z(cp));
        break;
    case 0x29: /* ADD HL,HL */
        alu_add16(gb, HL(cp));
        break;
    case 0x2A: { /* LD A,(HL+) */
        u16 hl = HL(cp);
        cp->a = mmu_read(gb, hl);
        set_hl(cp, (u16)(hl + 1));
    } break;
    case 0x2B: /* DEC HL */
        set_hl(cp, (u16)(HL(cp) - 1));
        gb_tick(gb, 4);
        break;
    case 0x2C: /* INC L */
        cp->l = alu_inc8(cp, cp->l);
        break;
    case 0x2D: /* DEC L */
        cp->l = alu_dec8(cp, cp->l);
        break;
    case 0x2E: /* LD L,d8 */
        cp->l = fetch8(gb);
        break;
    case 0x2F: /* CPL */
        cp->a = (u8)(~cp->a);
        cp->f = (u8)(cp->f | FLAG_N | FLAG_H);
        break;

    /* ------------------------------------------------------------ 0x3x */
    case 0x30: /* JR NC,r8 */
        op_jr(gb, !flag_c(cp));
        break;
    case 0x31: /* LD SP,d16 */
        cp->sp = fetch16(gb);
        break;
    case 0x32: { /* LD (HL-),A */
        u16 hl = HL(cp);
        mmu_write(gb, hl, cp->a);
        set_hl(cp, (u16)(hl - 1));
    } break;
    case 0x33: /* INC SP */
        cp->sp = (u16)(cp->sp + 1);
        gb_tick(gb, 4);
        break;
    case 0x34: { /* INC (HL) — 12 T */
        u16 hl  = HL(cp);
        u8  val = mmu_read(gb, hl);
        mmu_write(gb, hl, alu_inc8(cp, val));
    } break;
    case 0x35: { /* DEC (HL) — 12 T */
        u16 hl  = HL(cp);
        u8  val = mmu_read(gb, hl);
        mmu_write(gb, hl, alu_dec8(cp, val));
    } break;
    case 0x36: { /* LD (HL),d8 — 12 T */
        u8 val = fetch8(gb);
        mmu_write(gb, HL(cp), val);
    } break;
    case 0x37: /* SCF */
        cp->f = (u8)((cp->f & FLAG_Z) | FLAG_C);
        break;
    case 0x38: /* JR C,r8 */
        op_jr(gb, flag_c(cp));
        break;
    case 0x39: /* ADD HL,SP */
        alu_add16(gb, cp->sp);
        break;
    case 0x3A: { /* LD A,(HL-) */
        u16 hl = HL(cp);
        cp->a = mmu_read(gb, hl);
        set_hl(cp, (u16)(hl - 1));
    } break;
    case 0x3B: /* DEC SP */
        cp->sp = (u16)(cp->sp - 1);
        gb_tick(gb, 4);
        break;
    case 0x3C: /* INC A */
        cp->a = alu_inc8(cp, cp->a);
        break;
    case 0x3D: /* DEC A */
        cp->a = alu_dec8(cp, cp->a);
        break;
    case 0x3E: /* LD A,d8 */
        cp->a = fetch8(gb);
        break;
    case 0x3F: /* CCF */
        cp->f = (u8)((cp->f & FLAG_Z) | (flag_c(cp) ? 0 : FLAG_C));
        break;

    /* ------------------------------------------------ 0x40-0x7F: LD r,r' */
    case 0x40: break;                      /* LD B,B — functional NOP */
    case 0x41: cp->b = cp->c; break;
    case 0x42: cp->b = cp->d; break;
    case 0x43: cp->b = cp->e; break;
    case 0x44: cp->b = cp->h; break;
    case 0x45: cp->b = cp->l; break;
    case 0x46: cp->b = mmu_read(gb, HL(cp)); break;
    case 0x47: cp->b = cp->a; break;

    case 0x48: cp->c = cp->b; break;
    case 0x49: break;                      /* LD C,C — functional NOP */
    case 0x4A: cp->c = cp->d; break;
    case 0x4B: cp->c = cp->e; break;
    case 0x4C: cp->c = cp->h; break;
    case 0x4D: cp->c = cp->l; break;
    case 0x4E: cp->c = mmu_read(gb, HL(cp)); break;
    case 0x4F: cp->c = cp->a; break;

    case 0x50: cp->d = cp->b; break;
    case 0x51: cp->d = cp->c; break;
    case 0x52: break;                      /* LD D,D — functional NOP */
    case 0x53: cp->d = cp->e; break;
    case 0x54: cp->d = cp->h; break;
    case 0x55: cp->d = cp->l; break;
    case 0x56: cp->d = mmu_read(gb, HL(cp)); break;
    case 0x57: cp->d = cp->a; break;

    case 0x58: cp->e = cp->b; break;
    case 0x59: cp->e = cp->c; break;
    case 0x5A: cp->e = cp->d; break;
    case 0x5B: break;                      /* LD E,E — functional NOP */
    case 0x5C: cp->e = cp->h; break;
    case 0x5D: cp->e = cp->l; break;
    case 0x5E: cp->e = mmu_read(gb, HL(cp)); break;
    case 0x5F: cp->e = cp->a; break;

    case 0x60: cp->h = cp->b; break;
    case 0x61: cp->h = cp->c; break;
    case 0x62: cp->h = cp->d; break;
    case 0x63: cp->h = cp->e; break;
    case 0x64: break;                      /* LD H,H — functional NOP */
    case 0x65: cp->h = cp->l; break;
    case 0x66: cp->h = mmu_read(gb, HL(cp)); break;
    case 0x67: cp->h = cp->a; break;

    case 0x68: cp->l = cp->b; break;
    case 0x69: cp->l = cp->c; break;
    case 0x6A: cp->l = cp->d; break;
    case 0x6B: cp->l = cp->e; break;
    case 0x6C: cp->l = cp->h; break;
    case 0x6D: break;                      /* LD L,L — functional NOP */
    case 0x6E: cp->l = mmu_read(gb, HL(cp)); break;
    case 0x6F: cp->l = cp->a; break;

    case 0x70: mmu_write(gb, HL(cp), cp->b); break;
    case 0x71: mmu_write(gb, HL(cp), cp->c); break;
    case 0x72: mmu_write(gb, HL(cp), cp->d); break;
    case 0x73: mmu_write(gb, HL(cp), cp->e); break;
    case 0x74: mmu_write(gb, HL(cp), cp->h); break;
    case 0x75: mmu_write(gb, HL(cp), cp->l); break;
    case 0x76: { /* HALT — 4 T */
        /* An EI in the previous slot is about to take effect; hardware does not
         * exhibit the HALT bug in that case. */
        bool ime_now = cp->ime || ei_latch;
        if (!ime_now && (gb->mmu.ie & gb->mmu.ifr & 0x1F) != 0)
            cp->halt_bug = true;      /* PC fails to increment on next fetch */
        else
            cp->halted = true;
    } break;
    case 0x77: mmu_write(gb, HL(cp), cp->a); break;

    case 0x78: cp->a = cp->b; break;
    case 0x79: cp->a = cp->c; break;
    case 0x7A: cp->a = cp->d; break;
    case 0x7B: cp->a = cp->e; break;
    case 0x7C: cp->a = cp->h; break;
    case 0x7D: cp->a = cp->l; break;
    case 0x7E: cp->a = mmu_read(gb, HL(cp)); break;
    case 0x7F: break;                      /* LD A,A — functional NOP */

    /* ---------------------------------------------- 0x80-0xBF: ALU A,r   */
    case 0x80: alu_add(cp, cp->b); break;
    case 0x81: alu_add(cp, cp->c); break;
    case 0x82: alu_add(cp, cp->d); break;
    case 0x83: alu_add(cp, cp->e); break;
    case 0x84: alu_add(cp, cp->h); break;
    case 0x85: alu_add(cp, cp->l); break;
    case 0x86: alu_add(cp, mmu_read(gb, HL(cp))); break;
    case 0x87: alu_add(cp, cp->a); break;

    case 0x88: alu_adc(cp, cp->b); break;
    case 0x89: alu_adc(cp, cp->c); break;
    case 0x8A: alu_adc(cp, cp->d); break;
    case 0x8B: alu_adc(cp, cp->e); break;
    case 0x8C: alu_adc(cp, cp->h); break;
    case 0x8D: alu_adc(cp, cp->l); break;
    case 0x8E: alu_adc(cp, mmu_read(gb, HL(cp))); break;
    case 0x8F: alu_adc(cp, cp->a); break;

    case 0x90: alu_sub(cp, cp->b); break;
    case 0x91: alu_sub(cp, cp->c); break;
    case 0x92: alu_sub(cp, cp->d); break;
    case 0x93: alu_sub(cp, cp->e); break;
    case 0x94: alu_sub(cp, cp->h); break;
    case 0x95: alu_sub(cp, cp->l); break;
    case 0x96: alu_sub(cp, mmu_read(gb, HL(cp))); break;
    case 0x97: alu_sub(cp, cp->a); break;

    case 0x98: alu_sbc(cp, cp->b); break;
    case 0x99: alu_sbc(cp, cp->c); break;
    case 0x9A: alu_sbc(cp, cp->d); break;
    case 0x9B: alu_sbc(cp, cp->e); break;
    case 0x9C: alu_sbc(cp, cp->h); break;
    case 0x9D: alu_sbc(cp, cp->l); break;
    case 0x9E: alu_sbc(cp, mmu_read(gb, HL(cp))); break;
    case 0x9F: alu_sbc(cp, cp->a); break;

    case 0xA0: alu_and(cp, cp->b); break;
    case 0xA1: alu_and(cp, cp->c); break;
    case 0xA2: alu_and(cp, cp->d); break;
    case 0xA3: alu_and(cp, cp->e); break;
    case 0xA4: alu_and(cp, cp->h); break;
    case 0xA5: alu_and(cp, cp->l); break;
    case 0xA6: alu_and(cp, mmu_read(gb, HL(cp))); break;
    case 0xA7: alu_and(cp, cp->a); break;

    case 0xA8: alu_xor(cp, cp->b); break;
    case 0xA9: alu_xor(cp, cp->c); break;
    case 0xAA: alu_xor(cp, cp->d); break;
    case 0xAB: alu_xor(cp, cp->e); break;
    case 0xAC: alu_xor(cp, cp->h); break;
    case 0xAD: alu_xor(cp, cp->l); break;
    case 0xAE: alu_xor(cp, mmu_read(gb, HL(cp))); break;
    case 0xAF: alu_xor(cp, cp->a); break;

    case 0xB0: alu_or(cp, cp->b); break;
    case 0xB1: alu_or(cp, cp->c); break;
    case 0xB2: alu_or(cp, cp->d); break;
    case 0xB3: alu_or(cp, cp->e); break;
    case 0xB4: alu_or(cp, cp->h); break;
    case 0xB5: alu_or(cp, cp->l); break;
    case 0xB6: alu_or(cp, mmu_read(gb, HL(cp))); break;
    case 0xB7: alu_or(cp, cp->a); break;

    case 0xB8: alu_cp(cp, cp->b); break;
    case 0xB9: alu_cp(cp, cp->c); break;
    case 0xBA: alu_cp(cp, cp->d); break;
    case 0xBB: alu_cp(cp, cp->e); break;
    case 0xBC: alu_cp(cp, cp->h); break;
    case 0xBD: alu_cp(cp, cp->l); break;
    case 0xBE: alu_cp(cp, mmu_read(gb, HL(cp))); break;
    case 0xBF: alu_cp(cp, cp->a); break;

    /* ------------------------------------------------------------ 0xCx */
    case 0xC0: /* RET NZ */
        op_ret_cond(gb, !flag_z(cp));
        break;
    case 0xC1: /* POP BC — 12 T */
        set_bc(cp, pop16(gb));
        break;
    case 0xC2: /* JP NZ,a16 */
        op_jp(gb, !flag_z(cp));
        break;
    case 0xC3: /* JP a16 */
        op_jp(gb, true);
        break;
    case 0xC4: /* CALL NZ,a16 */
        op_call(gb, !flag_z(cp));
        break;
    case 0xC5: /* PUSH BC — 16 T */
        gb_tick(gb, 4);
        push16(gb, reg_bc(cp));
        break;
    case 0xC6: /* ADD A,d8 */
        alu_add(cp, fetch8(gb));
        break;
    case 0xC7: /* RST 00H */
        op_rst(gb, 0x0000);
        break;
    case 0xC8: /* RET Z */
        op_ret_cond(gb, flag_z(cp));
        break;
    case 0xC9: /* RET — 16 T */
        op_ret(gb);
        break;
    case 0xCA: /* JP Z,a16 */
        op_jp(gb, flag_z(cp));
        break;
    case 0xCB: /* PREFIX CB — cpu_cb.c fetches and ticks its own cycles */
        cpu_exec_cb(gb);
        break;
    case 0xCC: /* CALL Z,a16 */
        op_call(gb, flag_z(cp));
        break;
    case 0xCD: /* CALL a16 — 24 T */
        op_call(gb, true);
        break;
    case 0xCE: /* ADC A,d8 */
        alu_adc(cp, fetch8(gb));
        break;
    case 0xCF: /* RST 08H */
        op_rst(gb, 0x0008);
        break;

    /* ------------------------------------------------------------ 0xDx */
    case 0xD0: /* RET NC */
        op_ret_cond(gb, !flag_c(cp));
        break;
    case 0xD1: /* POP DE */
        set_de(cp, pop16(gb));
        break;
    case 0xD2: /* JP NC,a16 */
        op_jp(gb, !flag_c(cp));
        break;
    case 0xD4: /* CALL NC,a16 */
        op_call(gb, !flag_c(cp));
        break;
    case 0xD5: /* PUSH DE */
        gb_tick(gb, 4);
        push16(gb, DE(cp));
        break;
    case 0xD6: /* SUB d8 */
        alu_sub(cp, fetch8(gb));
        break;
    case 0xD7: /* RST 10H */
        op_rst(gb, 0x0010);
        break;
    case 0xD8: /* RET C */
        op_ret_cond(gb, flag_c(cp));
        break;
    case 0xD9: /* RETI — 16 T, IME restored immediately */
        op_ret(gb);
        cp->ime         = true;
        cp->ime_pending = false;
        break;
    case 0xDA: /* JP C,a16 */
        op_jp(gb, flag_c(cp));
        break;
    case 0xDC: /* CALL C,a16 */
        op_call(gb, flag_c(cp));
        break;
    case 0xDE: /* SBC A,d8 */
        alu_sbc(cp, fetch8(gb));
        break;
    case 0xDF: /* RST 18H */
        op_rst(gb, 0x0018);
        break;

    /* ------------------------------------------------------------ 0xEx */
    case 0xE0: { /* LDH (a8),A — 12 T */
        u8 lo = fetch8(gb);
        mmu_write(gb, (u16)(0xFF00 + lo), cp->a);
    } break;
    case 0xE1: /* POP HL */
        set_hl(cp, pop16(gb));
        break;
    case 0xE2: /* LD (C),A — 8 T */
        mmu_write(gb, (u16)(0xFF00 + cp->c), cp->a);
        break;
    case 0xE5: /* PUSH HL */
        gb_tick(gb, 4);
        push16(gb, HL(cp));
        break;
    case 0xE6: /* AND d8 */
        alu_and(cp, fetch8(gb));
        break;
    case 0xE7: /* RST 20H */
        op_rst(gb, 0x0020);
        break;
    case 0xE8: { /* ADD SP,r8 — 16 T */
        u16 res = alu_sp_offset(gb);
        gb_tick(gb, 4);
        gb_tick(gb, 4);
        cp->sp = res;
    } break;
    case 0xE9: /* JP (HL) — 4 T, no internal cycle */
        cp->pc = HL(cp);
        break;
    case 0xEA: { /* LD (a16),A — 16 T */
        u16 addr = fetch16(gb);
        mmu_write(gb, addr, cp->a);
    } break;
    case 0xEE: /* XOR d8 */
        alu_xor(cp, fetch8(gb));
        break;
    case 0xEF: /* RST 28H */
        op_rst(gb, 0x0028);
        break;

    /* ------------------------------------------------------------ 0xFx */
    case 0xF0: { /* LDH A,(a8) — 12 T */
        u8 lo = fetch8(gb);
        cp->a = mmu_read(gb, (u16)(0xFF00 + lo));
    } break;
    case 0xF1: /* POP AF — 12 T; the low nibble of F is not writable */
        set_af(cp, (u16)(pop16(gb) & 0xFFF0));
        break;
    case 0xF2: /* LD A,(C) — 8 T */
        cp->a = mmu_read(gb, (u16)(0xFF00 + cp->c));
        break;
    case 0xF3: /* DI — takes effect immediately */
        cp->ime         = false;
        cp->ime_pending = false;
        break;
    case 0xF5: /* PUSH AF */
        gb_tick(gb, 4);
        push16(gb, (u16)((u16)cp->a << 8 | (cp->f & 0xF0)));
        break;
    case 0xF6: /* OR d8 */
        alu_or(cp, fetch8(gb));
        break;
    case 0xF7: /* RST 30H */
        op_rst(gb, 0x0030);
        break;
    case 0xF8: { /* LD HL,SP+r8 — 12 T */
        u16 res = alu_sp_offset(gb);
        gb_tick(gb, 4);
        set_hl(cp, res);
    } break;
    case 0xF9: /* LD SP,HL — 8 T */
        cp->sp = HL(cp);
        gb_tick(gb, 4);
        break;
    case 0xFA: { /* LD A,(a16) — 16 T */
        u16 addr = fetch16(gb);
        cp->a = mmu_read(gb, addr);
    } break;
    case 0xFB: /* EI — IME is set after the NEXT instruction */
        cp->ime_pending = true;
        break;
    case 0xFE: /* CP d8 */
        alu_cp(cp, fetch8(gb));
        break;
    case 0xFF: /* RST 38H */
        op_rst(gb, 0x0038);
        break;

    /* ------------------------------------------------- illegal opcodes  */
    /* On real hardware these hang the CPU until a reset.  We reproduce the
     * lock-up without taking the emulator down: rewind PC to the offending
     * byte and halt, so the machine keeps ticking (video/audio stay alive)
     * and the debugger can see exactly where it died. */
    case 0xD3: case 0xDB: case 0xDD:
    case 0xE3: case 0xE4: case 0xEB: case 0xEC: case 0xED:
    case 0xF4: case 0xFC: case 0xFD:
        cp->illegal_op     = true;
        cp->illegal_opcode = op;
        cp->pc             = op_pc;
        cp->halted         = true;
        break;
    }

    /* The EI latched before this instruction now takes effect (unless the
     * instruction itself was DI / RETI / an interrupt dispatch). */
    if (ei_latch && cp->ime_pending) {
        cp->ime         = true;
        cp->ime_pending = false;
    }

    return elapsed(gb, start);
}

/* =============================================================================
 * CONTRACT-NOTE
 *
 * 1. Cycle accounting depends on gb_tick() advancing GB::cycles by its `t`
 *    argument.  Every entry point here returns (gb->cycles - start), exactly as
 *    specified, so if gb.c scales GB::cycles to "normal-speed units" while in
 *    CGB double-speed mode (as the GB::cycles comment and the gb_step() doc
 *    comment in gb.h both suggest), the values returned by cpu_step() and
 *    cpu_handle_interrupts() are scaled the same way.  At normal speed an
 *    interrupt dispatch is exactly 20 T and every instruction matches the
 *    documented DMG timings.  gb.c must NOT add cycles of its own on top of
 *    what cpu_step() returns, or the machine will run at half rate.
 *
 * 2. gb.h has no field for the CGB speed-switch delay, so cpu_step() performs
 *    the 2050 M-cycle stall inline (as 2050 gb_tick(gb,4) calls) when it finds
 *    cpu.stopped together with gb->speed_switch.  This makes a single
 *    cpu_step() return 8200 T; gb_run_frame() must therefore tolerate a step
 *    larger than one scanline.  cpu.c flips gb->speed and clears
 *    gb->speed_switch itself; mmu.c should only ever *set* gb->speed_switch
 *    from a KEY1 write and report gb->speed on a KEY1 read.
 *
 * 3. HALT executed in the instruction slot immediately after EI is treated as
 *    IME==1 (no HALT bug), matching hardware and keeping the extremely common
 *    "EI / HALT" main-loop idiom safe.  The HALT bug itself is still fully
 *    implemented for the IME==0 case via CPU::halt_bug.
 *
 * 4. Interrupt dispatch re-reads IE/IF after the high half of the PC push.
 *    That models the documented "interrupt cancellation" case (a push landing
 *    on 0xFFFF rewrites IE), in which the CPU vectors to 0x0000 and no IF bit
 *    is cleared.  The total is still exactly 5 M-cycles.
 *
 * 5. Illegal opcodes rewind PC to the offending byte before halting, so the
 *    lock-up is sticky: if an interrupt later clears CPU::halted, the same
 *    illegal byte is re-fetched and the CPU halts again, which is what the
 *    hardware lock-up looks like from the outside.  CPU::illegal_op and
 *    CPU::illegal_opcode are left set for the debugger/frontend.
 * ========================================================================== */
