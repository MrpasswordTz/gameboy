/* =============================================================================
 *  cpu_cb.c — the 0xCB-prefixed opcode page (all 256 sub-opcodes).
 *
 *  Entered from cpu.c *after* the 0xCB prefix byte has already been fetched
 *  (that fetch performed its own mmu_read, i.e. 4 T-cycles).  This file fetches
 *  the sub-opcode, decodes it and executes it.  No internal gb_tick() calls are
 *  made anywhere here: the CB page needs none beyond its bus accesses, and
 *  mmu_read()/mmu_write() already tick 4 T-cycles each.
 *
 *  Resulting timings (prefix 4 + sub-opcode 4 + accesses):
 *      CB <op> r          ->  8 T-cycles
 *      CB BIT b,(HL)      -> 12 T-cycles   (one read, no write-back)
 *      CB <op> (HL)       -> 16 T-cycles   (read + write)
 *
 *  Flag semantics follow Pan Docs exactly; F's low nibble is always kept zero.
 * ========================================================================== */

#include "gb.h"

/* ------------------------------------------------------------------ flags -- */

/* Write Z/N/H/C wholesale.  The low nibble of F is hardwired to zero on the
 * real Sharp SM83, so we never let anything leak into it. */
static inline void cb_set_flags(GB *gb, bool z, bool n, bool h, bool c)
{
    u8 f = 0;
    if (z) f |= FLAG_Z;
    if (n) f |= FLAG_N;
    if (h) f |= FLAG_H;
    if (c) f |= FLAG_C;
    gb->cpu.f = f;                       /* low nibble implicitly 0 */
}

static inline bool cb_carry(const GB *gb)
{
    return (gb->cpu.f & FLAG_C) != 0;
}

/* --------------------------------------------------------- operand access -- */
/* Operand encoding (low 3 bits of the sub-opcode):
 *      0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A                                    */

static u8 cb_read_operand(GB *gb, int idx)
{
    CPU *c = &gb->cpu;

    switch (idx & 7) {
    case 0: return c->b;
    case 1: return c->c;
    case 2: return c->d;
    case 3: return c->e;
    case 4: return c->h;
    case 5: return c->l;
    case 6: return mmu_read(gb, HL(c));   /* ticks 4 */
    case 7: return c->a;
    default: return 0;                    /* unreachable: idx is masked */
    }
}

static void cb_write_operand(GB *gb, int idx, u8 val)
{
    CPU *c = &gb->cpu;

    switch (idx & 7) {
    case 0: c->b = val; break;
    case 1: c->c = val; break;
    case 2: c->d = val; break;
    case 3: c->e = val; break;
    case 4: c->h = val; break;
    case 5: c->l = val; break;
    case 6: mmu_write(gb, HL(c), val); break;   /* ticks 4 */
    case 7: c->a = val; break;
    default: break;                             /* unreachable */
    }
}

/* ------------------------------------------------------------- operations -- */
/* Every rotate/shift: Z = (result == 0), N = 0, H = 0, C = bit shifted out.   */

/* RLC — rotate left, bit 7 -> bit 0 and -> carry. */
static u8 cb_op_rlc(GB *gb, u8 v)
{
    u8 carry = (u8)(v >> 7);
    u8 r     = (u8)((u8)(v << 1) | carry);
    cb_set_flags(gb, r == 0, false, false, carry != 0);
    return r;
}

/* RRC — rotate right, bit 0 -> bit 7 and -> carry. */
static u8 cb_op_rrc(GB *gb, u8 v)
{
    u8 carry = (u8)(v & 0x01);
    u8 r     = (u8)((u8)(v >> 1) | (u8)(carry << 7));
    cb_set_flags(gb, r == 0, false, false, carry != 0);
    return r;
}

/* RL — rotate left *through* carry: old C becomes bit 0, bit 7 becomes C. */
static u8 cb_op_rl(GB *gb, u8 v)
{
    u8 old = cb_carry(gb) ? 1u : 0u;
    u8 carry = (u8)(v >> 7);
    u8 r     = (u8)((u8)(v << 1) | old);
    cb_set_flags(gb, r == 0, false, false, carry != 0);
    return r;
}

/* RR — rotate right through carry: old C becomes bit 7, bit 0 becomes C. */
static u8 cb_op_rr(GB *gb, u8 v)
{
    u8 old = cb_carry(gb) ? 0x80u : 0x00u;
    u8 carry = (u8)(v & 0x01);
    u8 r     = (u8)((u8)(v >> 1) | old);
    cb_set_flags(gb, r == 0, false, false, carry != 0);
    return r;
}

/* SLA — arithmetic shift left; bit 0 cleared, bit 7 -> carry. */
static u8 cb_op_sla(GB *gb, u8 v)
{
    u8 carry = (u8)(v >> 7);
    u8 r     = (u8)(v << 1);
    cb_set_flags(gb, r == 0, false, false, carry != 0);
    return r;
}

/* SRA — arithmetic shift right; bit 7 preserved, bit 0 -> carry. */
static u8 cb_op_sra(GB *gb, u8 v)
{
    u8 carry = (u8)(v & 0x01);
    u8 r     = (u8)((u8)(v >> 1) | (u8)(v & 0x80));
    cb_set_flags(gb, r == 0, false, false, carry != 0);
    return r;
}

/* SWAP — exchange the two nibbles; N = H = C = 0. */
static u8 cb_op_swap(GB *gb, u8 v)
{
    u8 r = (u8)((u8)(v >> 4) | (u8)(v << 4));
    cb_set_flags(gb, r == 0, false, false, false);
    return r;
}

/* SRL — logical shift right; bit 7 cleared, bit 0 -> carry. */
static u8 cb_op_srl(GB *gb, u8 v)
{
    u8 carry = (u8)(v & 0x01);
    u8 r     = (u8)(v >> 1);
    cb_set_flags(gb, r == 0, false, false, carry != 0);
    return r;
}

/* BIT b,x — Z = (bit clear), N = 0, H = 1, C untouched.  Writes nothing back. */
static void cb_op_bit(GB *gb, u8 v, int bit)
{
    u8 f = (u8)(gb->cpu.f & FLAG_C);       /* carry survives, low nibble 0 */
    f |= FLAG_H;
    if ((v & (u8)(1u << (bit & 7))) == 0) f |= FLAG_Z;
    gb->cpu.f = f;
}

/* RES b,x — clear bit; no flags affected. */
static u8 cb_op_res(u8 v, int bit)
{
    return (u8)(v & (u8)~(u8)(1u << (bit & 7)));
}

/* SET b,x — set bit; no flags affected. */
static u8 cb_op_set(u8 v, int bit)
{
    return (u8)(v | (u8)(1u << (bit & 7)));
}

/* ------------------------------------------------------------- dispatcher -- */

void cpu_exec_cb(GB *gb)
{
    /* 1. Fetch the sub-opcode (ticks 4). */
    u8 op = mmu_read(gb, gb->cpu.pc++);

    int operand = op & 0x07;          /* 0..7  -> B C D E H L (HL) A          */
    int group   = (op >> 3) & 0x07;   /* op/bit selector within the quadrant  */

    /* 2. Read the operand (ticks 4 more when it is (HL)). */
    u8 v = cb_read_operand(gb, operand);
    u8 r;

    if (op < 0x40) {
        /* 0x00-0x3F: rotates, shifts and SWAP. */
        switch (group) {
        case 0: r = cb_op_rlc (gb, v); break;   /* 0x00-0x07 RLC  */
        case 1: r = cb_op_rrc (gb, v); break;   /* 0x08-0x0F RRC  */
        case 2: r = cb_op_rl  (gb, v); break;   /* 0x10-0x17 RL   */
        case 3: r = cb_op_rr  (gb, v); break;   /* 0x18-0x1F RR   */
        case 4: r = cb_op_sla (gb, v); break;   /* 0x20-0x27 SLA  */
        case 5: r = cb_op_sra (gb, v); break;   /* 0x28-0x2F SRA  */
        case 6: r = cb_op_swap(gb, v); break;   /* 0x30-0x37 SWAP */
        case 7: r = cb_op_srl (gb, v); break;   /* 0x38-0x3F SRL  */
        default: r = v; break;                  /* unreachable    */
        }
        cb_write_operand(gb, operand, r);       /* ticks 4 when (HL) */
    } else if (op < 0x80) {
        /* 0x40-0x7F: BIT b,x — flags only, deliberately no write-back, which
         * is what makes BIT b,(HL) 12 T-cycles instead of 16.               */
        cb_op_bit(gb, v, group);
    } else if (op < 0xC0) {
        /* 0x80-0xBF: RES b,x */
        r = cb_op_res(v, group);
        cb_write_operand(gb, operand, r);
    } else {
        /* 0xC0-0xFF: SET b,x */
        r = cb_op_set(v, group);
        cb_write_operand(gb, operand, r);
    }
}

/* =============================================================================
 *  Coverage note: the four ranges above partition 0x00-0xFF exhaustively
 *  (0x00-0x3F / 0x40-0x7F / 0x80-0xBF / 0xC0-0xFF), and inside each range the
 *  3-bit `group` and 3-bit `operand` fields are fully enumerated, so all 256
 *  sub-opcodes are implemented.  There are no illegal CB opcodes on the SM83.
 *
 *  Memory safety: no array is indexed in this file.  `operand` and the bit
 *  index are masked to 0..7 at every use site, and the only guest-controlled
 *  address ever touched is HL, which is a full 16-bit value handled (and
 *  bounds-checked) by the MMU.
 * ========================================================================== */
