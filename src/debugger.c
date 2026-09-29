/* =============================================================================
 *  debugger.c — SM83 disassembler, trace formatter and interactive debugger.
 *
 *  Nothing in this file may have side effects on the emulated machine other
 *  than the explicit commands the user types: the disassembler and the trace
 *  formatter use mmu_peek() exclusively so they never tick the hardware.
 *
 *  Every buffer here is bounded, every guest-derived index is masked to the
 *  16-bit address space, and the breakpoint table is clamped to its real
 *  capacity on every access (the array lives in `struct GB`, so a corrupted
 *  n_breakpoints must never be able to walk off the end of it).
 * ========================================================================== */
#include "gb.h"

#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/*  Small bounded string helpers                                              */
/* ========================================================================== */

/* Append `s` to buf[0..sz-1], keeping *pos as the current length. Always NUL
 * terminates. Silently truncates rather than overflowing. */
static void sb_add(char *buf, size_t sz, size_t *pos, const char *s)
{
    if (!buf || sz == 0 || !s) return;
    if (*pos >= sz) { buf[sz - 1] = '\0'; *pos = sz - 1; return; }
    while (*s != '\0' && *pos + 1 < sz)
        buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

static int is_space_ch(int ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' ||
           ch == '\r' || ch == '\v' || ch == '\f';
}

static char lower_ch(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

/* Case-insensitive compare, bounded by the NUL terminators. */
static bool str_ieq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a != '\0' && *b != '\0') {
        if (lower_ch(*a) != lower_ch(*b)) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ========================================================================== */
/*  Disassembler tables                                                       */
/* ========================================================================== */
/*
 *  Format tokens understood by dis_expand():
 *      %W  16-bit immediate at addr+1 .. addr+2   -> "$1234"
 *      %B   8-bit immediate at addr+1             -> "$FF"
 *      %H   8-bit high-page immediate at addr+1   -> "$FF00+$44"
 *      %R   8-bit signed relative, resolved       -> "$0123"
 *      %S   8-bit signed offset with sign         -> "+$05" / "-$02"
 *  A NULL format means "illegal opcode": printed as "DB $xx".
 */
typedef struct {
    const char *fmt;
    u8          len;      /* total instruction length in bytes (1..3) */
} DisOp;

static const DisOp base_ops[256] = {
    /* 0x00 */
    { "NOP",            1 }, { "LD BC,%W",       3 }, { "LD (BC),A",      1 },
    { "INC BC",         1 }, { "INC B",          1 }, { "DEC B",          1 },
    { "LD B,%B",        2 }, { "RLCA",           1 }, { "LD (%W),SP",     3 },
    { "ADD HL,BC",      1 }, { "LD A,(BC)",      1 }, { "DEC BC",         1 },
    { "INC C",          1 }, { "DEC C",          1 }, { "LD C,%B",        2 },
    { "RRCA",           1 },
    /* 0x10 */
    { "STOP",           2 }, { "LD DE,%W",       3 }, { "LD (DE),A",      1 },
    { "INC DE",         1 }, { "INC D",          1 }, { "DEC D",          1 },
    { "LD D,%B",        2 }, { "RLA",            1 }, { "JR %R",          2 },
    { "ADD HL,DE",      1 }, { "LD A,(DE)",      1 }, { "DEC DE",         1 },
    { "INC E",          1 }, { "DEC E",          1 }, { "LD E,%B",        2 },
    { "RRA",            1 },
    /* 0x20 */
    { "JR NZ,%R",       2 }, { "LD HL,%W",       3 }, { "LD (HL+),A",     1 },
    { "INC HL",         1 }, { "INC H",          1 }, { "DEC H",          1 },
    { "LD H,%B",        2 }, { "DAA",            1 }, { "JR Z,%R",        2 },
    { "ADD HL,HL",      1 }, { "LD A,(HL+)",     1 }, { "DEC HL",         1 },
    { "INC L",          1 }, { "DEC L",          1 }, { "LD L,%B",        2 },
    { "CPL",            1 },
    /* 0x30 */
    { "JR NC,%R",       2 }, { "LD SP,%W",       3 }, { "LD (HL-),A",     1 },
    { "INC SP",         1 }, { "INC (HL)",       1 }, { "DEC (HL)",       1 },
    { "LD (HL),%B",     2 }, { "SCF",            1 }, { "JR C,%R",        2 },
    { "ADD HL,SP",      1 }, { "LD A,(HL-)",     1 }, { "DEC SP",         1 },
    { "INC A",          1 }, { "DEC A",          1 }, { "LD A,%B",        2 },
    { "CCF",            1 },
    /* 0x40 */
    { "LD B,B",         1 }, { "LD B,C",         1 }, { "LD B,D",         1 },
    { "LD B,E",         1 }, { "LD B,H",         1 }, { "LD B,L",         1 },
    { "LD B,(HL)",      1 }, { "LD B,A",         1 }, { "LD C,B",         1 },
    { "LD C,C",         1 }, { "LD C,D",         1 }, { "LD C,E",         1 },
    { "LD C,H",         1 }, { "LD C,L",         1 }, { "LD C,(HL)",      1 },
    { "LD C,A",         1 },
    /* 0x50 */
    { "LD D,B",         1 }, { "LD D,C",         1 }, { "LD D,D",         1 },
    { "LD D,E",         1 }, { "LD D,H",         1 }, { "LD D,L",         1 },
    { "LD D,(HL)",      1 }, { "LD D,A",         1 }, { "LD E,B",         1 },
    { "LD E,C",         1 }, { "LD E,D",         1 }, { "LD E,E",         1 },
    { "LD E,H",         1 }, { "LD E,L",         1 }, { "LD E,(HL)",      1 },
    { "LD E,A",         1 },
    /* 0x60 */
    { "LD H,B",         1 }, { "LD H,C",         1 }, { "LD H,D",         1 },
    { "LD H,E",         1 }, { "LD H,H",         1 }, { "LD H,L",         1 },
    { "LD H,(HL)",      1 }, { "LD H,A",         1 }, { "LD L,B",         1 },
    { "LD L,C",         1 }, { "LD L,D",         1 }, { "LD L,E",         1 },
    { "LD L,H",         1 }, { "LD L,L",         1 }, { "LD L,(HL)",      1 },
    { "LD L,A",         1 },
    /* 0x70 */
    { "LD (HL),B",      1 }, { "LD (HL),C",      1 }, { "LD (HL),D",      1 },
    { "LD (HL),E",      1 }, { "LD (HL),H",      1 }, { "LD (HL),L",      1 },
    { "HALT",           1 }, { "LD (HL),A",      1 }, { "LD A,B",         1 },
    { "LD A,C",         1 }, { "LD A,D",         1 }, { "LD A,E",         1 },
    { "LD A,H",         1 }, { "LD A,L",         1 }, { "LD A,(HL)",      1 },
    { "LD A,A",         1 },
    /* 0x80 */
    { "ADD A,B",        1 }, { "ADD A,C",        1 }, { "ADD A,D",        1 },
    { "ADD A,E",        1 }, { "ADD A,H",        1 }, { "ADD A,L",        1 },
    { "ADD A,(HL)",     1 }, { "ADD A,A",        1 }, { "ADC A,B",        1 },
    { "ADC A,C",        1 }, { "ADC A,D",        1 }, { "ADC A,E",        1 },
    { "ADC A,H",        1 }, { "ADC A,L",        1 }, { "ADC A,(HL)",     1 },
    { "ADC A,A",        1 },
    /* 0x90 */
    { "SUB B",          1 }, { "SUB C",          1 }, { "SUB D",          1 },
    { "SUB E",          1 }, { "SUB H",          1 }, { "SUB L",          1 },
    { "SUB (HL)",       1 }, { "SUB A",          1 }, { "SBC A,B",        1 },
    { "SBC A,C",        1 }, { "SBC A,D",        1 }, { "SBC A,E",        1 },
    { "SBC A,H",        1 }, { "SBC A,L",        1 }, { "SBC A,(HL)",     1 },
    { "SBC A,A",        1 },
    /* 0xA0 */
    { "AND B",          1 }, { "AND C",          1 }, { "AND D",          1 },
    { "AND E",          1 }, { "AND H",          1 }, { "AND L",          1 },
    { "AND (HL)",       1 }, { "AND A",          1 }, { "XOR B",          1 },
    { "XOR C",          1 }, { "XOR D",          1 }, { "XOR E",          1 },
    { "XOR H",          1 }, { "XOR L",          1 }, { "XOR (HL)",       1 },
    { "XOR A",          1 },
    /* 0xB0 */
    { "OR B",           1 }, { "OR C",           1 }, { "OR D",           1 },
    { "OR E",           1 }, { "OR H",           1 }, { "OR L",           1 },
    { "OR (HL)",        1 }, { "OR A",           1 }, { "CP B",           1 },
    { "CP C",           1 }, { "CP D",           1 }, { "CP E",           1 },
    { "CP H",           1 }, { "CP L",           1 }, { "CP (HL)",        1 },
    { "CP A",           1 },
    /* 0xC0 */
    { "RET NZ",         1 }, { "POP BC",         1 }, { "JP NZ,%W",       3 },
    { "JP %W",          3 }, { "CALL NZ,%W",     3 }, { "PUSH BC",        1 },
    { "ADD A,%B",       2 }, { "RST $00",        1 }, { "RET Z",          1 },
    { "RET",            1 }, { "JP Z,%W",        3 }, { NULL,             1 },
    { "CALL Z,%W",      3 }, { "CALL %W",        3 }, { "ADC A,%B",       2 },
    { "RST $08",        1 },
    /* 0xD0 */
    { "RET NC",         1 }, { "POP DE",         1 }, { "JP NC,%W",       3 },
    { NULL,             1 }, { "CALL NC,%W",     3 }, { "PUSH DE",        1 },
    { "SUB %B",         2 }, { "RST $10",        1 }, { "RET C",          1 },
    { "RETI",           1 }, { "JP C,%W",        3 }, { NULL,             1 },
    { "CALL C,%W",      3 }, { NULL,             1 }, { "SBC A,%B",       2 },
    { "RST $18",        1 },
    /* 0xE0 */
    { "LDH (%H),A",     2 }, { "POP HL",         1 }, { "LD ($FF00+C),A", 1 },
    { NULL,             1 }, { NULL,             1 }, { "PUSH HL",        1 },
    { "AND %B",         2 }, { "RST $20",        1 }, { "ADD SP,%S",      2 },
    { "JP (HL)",        1 }, { "LD (%W),A",      3 }, { NULL,             1 },
    { NULL,             1 }, { NULL,             1 }, { "XOR %B",         2 },
    { "RST $28",        1 },
    /* 0xF0 */
    { "LDH A,(%H)",     2 }, { "POP AF",         1 }, { "LD A,($FF00+C)", 1 },
    { "DI",             1 }, { NULL,             1 }, { "PUSH AF",        1 },
    { "OR %B",          2 }, { "RST $30",        1 }, { "LD HL,SP%S",     2 },
    { "LD SP,HL",       1 }, { "LD A,(%W)",      3 }, { "EI",             1 },
    { NULL,             1 }, { NULL,             1 }, { "CP %B",          2 },
    { "RST $38",        1 }
};

static const char *const cb_rot_ops[8] = {
    "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL"
};

static const char *const cb_bit_ops[4] = { "???", "BIT", "RES", "SET" };

static const char *const r8_names[8] = {
    "B", "C", "D", "E", "H", "L", "(HL)", "A"
};

/* ========================================================================== */
/*  Disassembler                                                              */
/* ========================================================================== */

/* Peek with an explicit 16-bit wrap so a guest PC near 0xFFFF can never make
 * us read outside the address space. */
static u8 dis_peek(GB *gb, u32 addr)
{
    return mmu_peek(gb, (u16)(addr & 0xFFFFu));
}

/* Expand one format string into `dst` (bounded). */
static void dis_expand(GB *gb, u16 addr, const char *fmt, int len,
                       char *dst, size_t dst_sz)
{
    size_t pos = 0;
    char   num[24];

    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!fmt) return;

    while (*fmt != '\0') {
        if (*fmt != '%' || fmt[1] == '\0') {
            char one[2];
            one[0] = *fmt++;
            one[1] = '\0';
            sb_add(dst, dst_sz, &pos, one);
            continue;
        }
        fmt++;                       /* skip '%' */
        switch (*fmt++) {
        case 'W': {
            u32 lo = dis_peek(gb, (u32)addr + 1u);
            u32 hi = dis_peek(gb, (u32)addr + 2u);
            snprintf(num, sizeof num, "$%04X",
                     (unsigned)((hi << 8) | lo) & 0xFFFFu);
            sb_add(dst, dst_sz, &pos, num);
            break;
        }
        case 'B': {
            u8 v = dis_peek(gb, (u32)addr + 1u);
            snprintf(num, sizeof num, "$%02X", (unsigned)v);
            sb_add(dst, dst_sz, &pos, num);
            break;
        }
        case 'H': {
            u8 v = dis_peek(gb, (u32)addr + 1u);
            snprintf(num, sizeof num, "$FF00+$%02X", (unsigned)v);
            sb_add(dst, dst_sz, &pos, num);
            break;
        }
        case 'R': {
            s8  e   = (s8)dis_peek(gb, (u32)addr + 1u);
            u16 tgt = (u16)(((u32)addr + (u32)len + (u32)(s32)e) & 0xFFFFu);
            snprintf(num, sizeof num, "$%04X", (unsigned)tgt);
            sb_add(dst, dst_sz, &pos, num);
            break;
        }
        case 'S': {
            s8  e = (s8)dis_peek(gb, (u32)addr + 1u);
            int m = (e < 0) ? -(int)e : (int)e;
            snprintf(num, sizeof num, "%c$%02X", (e < 0) ? '-' : '+',
                     (unsigned)m & 0xFFu);
            sb_add(dst, dst_sz, &pos, num);
            break;
        }
        default:
            /* Unknown token: emit it verbatim so a table typo is visible. */
            sb_add(dst, dst_sz, &pos, "%");
            break;
        }
    }
}

int gb_disasm(GB *gb, u16 addr, char *out, size_t out_sz)
{
    char tmp[80];
    u8   op;

    if (out && out_sz > 0) out[0] = '\0';
    if (!gb) return 1;

    op = mmu_peek(gb, addr);

    /* ---- 0xCB prefix: decode straight through to the real instruction ---- */
    if (op == 0xCB) {
        u8       cb   = dis_peek(gb, (u32)addr + 1u);
        unsigned reg  = (unsigned)cb & 7u;
        unsigned kind = (unsigned)cb >> 6;      /* 0 = rot/shift, 1..3 = bit  */

        if (kind == 0u) {
            unsigned grp = ((unsigned)cb >> 3) & 7u;
            snprintf(tmp, sizeof tmp, "%s %s", cb_rot_ops[grp], r8_names[reg]);
        } else {
            unsigned bit = ((unsigned)cb >> 3) & 7u;
            snprintf(tmp, sizeof tmp, "%s %u,%s",
                     cb_bit_ops[kind & 3u], bit, r8_names[reg]);
        }
        if (out && out_sz > 0) snprintf(out, out_sz, "%s", tmp);
        return 2;
    }

    /* ---- illegal opcodes --------------------------------------------- */
    if (base_ops[op].fmt == NULL) {
        snprintf(tmp, sizeof tmp, "DB $%02X", (unsigned)op);
        if (out && out_sz > 0) snprintf(out, out_sz, "%s", tmp);
        return 1;
    }

    {
        int len = (int)base_ops[op].len;
        if (len < 1) len = 1;
        if (len > 3) len = 3;
        dis_expand(gb, addr, base_ops[op].fmt, len, tmp, sizeof tmp);
        if (out && out_sz > 0) snprintf(out, out_sz, "%s", tmp);
        return len;
    }
}

/* ========================================================================== */
/*  Trace line                                                                */
/* ========================================================================== */

void gb_trace_line(GB *gb, char *out, size_t out_sz)
{
    char dis[80];
    u8   m[4];
    u16  pc;
    int  i;

    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!gb) return;

    pc = gb->cpu.pc;
    for (i = 0; i < 4; i++)
        m[i] = mmu_peek(gb, (u16)(((u32)pc + (u32)i) & 0xFFFFu));

    gb_disasm(gb, pc, dis, sizeof dis);

    snprintf(out, out_sz,
             "A:%02X F:%02X B:%02X C:%02X D:%02X E:%02X H:%02X L:%02X "
             "SP:%04X PC:%04X PCMEM:%02X,%02X,%02X,%02X  %s ; cyc=%llu",
             (unsigned)gb->cpu.a, (unsigned)gb->cpu.f,
             (unsigned)gb->cpu.b, (unsigned)gb->cpu.c,
             (unsigned)gb->cpu.d, (unsigned)gb->cpu.e,
             (unsigned)gb->cpu.h, (unsigned)gb->cpu.l,
             (unsigned)gb->cpu.sp, (unsigned)pc,
             (unsigned)m[0], (unsigned)m[1], (unsigned)m[2], (unsigned)m[3],
             dis, (unsigned long long)gb->cycles);
}

/* ========================================================================== */
/*  Breakpoints                                                               */
/* ========================================================================== */

/* Real capacity of gb->breakpoints, derived from the array itself. */
static int bp_capacity(const GB *gb)
{
    return (int)(sizeof gb->breakpoints / sizeof gb->breakpoints[0]);
}

/* Clamp a possibly-corrupt n_breakpoints into [0, capacity]. */
static int bp_count(GB *gb)
{
    int cap = bp_capacity(gb);
    int n   = gb->n_breakpoints;
    if (n < 0)   n = 0;
    if (n > cap) n = cap;
    gb->n_breakpoints = n;
    return n;
}

static bool bp_is_set(GB *gb, u16 pc)
{
    int n = bp_count(gb);
    int i;
    for (i = 0; i < n; i++)
        if ((gb->breakpoints[i] & 0xFFFFu) == (u32)pc) return true;
    return false;
}

static bool bp_add(GB *gb, u16 pc)
{
    int n   = bp_count(gb);
    int cap = bp_capacity(gb);
    if (bp_is_set(gb, pc)) return true;      /* already present: no-op */
    if (n >= cap) return false;
    gb->breakpoints[n] = (u32)pc;
    gb->n_breakpoints  = n + 1;
    return true;
}

static bool bp_remove(GB *gb, u16 pc)
{
    int n = bp_count(gb);
    int i;
    for (i = 0; i < n; i++) {
        if ((gb->breakpoints[i] & 0xFFFFu) != (u32)pc) continue;
        for (; i < n - 1; i++)
            gb->breakpoints[i] = gb->breakpoints[i + 1];
        gb->breakpoints[n - 1] = 0;
        gb->n_breakpoints      = n - 1;
        return true;
    }
    return false;
}

void debugger_break_here(GB *gb)
{
    if (!gb) return;
    gb->paused         = true;
    gb->hit_breakpoint = true;
}

/* ========================================================================== */
/*  Argument parsing                                                          */
/* ========================================================================== */

/* Hexadecimal value, tolerating a leading "0x", "0X", "$" or "#". */
static bool parse_hex(const char *s, u32 *value)
{
    unsigned long v;
    char         *end = NULL;

    if (!s || !value) return false;
    while (*s != '\0' && is_space_ch((unsigned char)*s)) s++;
    if (*s == '$' || *s == '#') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (*s == '\0' || *s == '-' || *s == '+') return false;

    v = strtoul(s, &end, 16);
    if (end == s) return false;
    while (*end != '\0' && is_space_ch((unsigned char)*end)) end++;
    if (*end != '\0') return false;

    *value = (u32)v;
    return true;
}

/* Count/repeat argument: decimal by default, hex when prefixed. */
static bool parse_count(const char *s, long *value)
{
    unsigned long v;
    char         *end  = NULL;
    int           base = 10;

    if (!s || !value) return false;
    while (*s != '\0' && is_space_ch((unsigned char)*s)) s++;
    if (*s == '$' || *s == '#') { s++; base = 16; }
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    if (*s == '\0' || *s == '-' || *s == '+') return false;

    v = strtoul(s, &end, base);
    if (end == s) return false;
    while (*end != '\0' && is_space_ch((unsigned char)*end)) end++;
    if (*end != '\0') return false;
    if (v > 0x7FFFFFFFuL) v = 0x7FFFFFFFuL;

    *value = (long)v;
    return true;
}

/* Split `s` in place into at most `max_tok` whitespace-separated tokens. */
static int tokenize(char *s, char **argv, int max_tok)
{
    int   n = 0;
    char *p = s;

    if (!s || !argv || max_tok <= 0) return 0;
    while (*p != '\0' && n < max_tok) {
        while (*p != '\0' && is_space_ch((unsigned char)*p)) p++;
        if (*p == '\0') break;
        argv[n++] = p;
        while (*p != '\0' && !is_space_ch((unsigned char)*p)) p++;
        if (*p != '\0') *p++ = '\0';
    }
    return n;
}

/* Everything after the first token of `src`, trailing whitespace trimmed,
 * copied into dst. Returns false when the remainder is empty. */
static bool rest_after_cmd(const char *src, char *dst, size_t dst_sz)
{
    size_t n;

    if (!src || !dst || dst_sz == 0) return false;
    dst[0] = '\0';
    while (*src != '\0' && is_space_ch((unsigned char)*src)) src++;
    while (*src != '\0' && !is_space_ch((unsigned char)*src)) src++;
    while (*src != '\0' && is_space_ch((unsigned char)*src)) src++;
    if (*src == '\0') return false;

    n = 0;
    while (src[n] != '\0' && n + 1 < dst_sz) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
    while (n > 0 && is_space_ch((unsigned char)dst[n - 1])) dst[--n] = '\0';
    return n > 0;
}

/* ========================================================================== */
/*  Commands                                                                  */
/* ========================================================================== */

static void print_help(void)
{
    static const char *const lines[] = {
        "commands (short / long):",
        "  s,  step [n]           execute n instructions (default 1)",
        "  n,  next               step over a CALL (otherwise same as step)",
        "  c,  continue           resume execution and leave the debugger",
        "  b,  break <hex>        add a breakpoint at an address",
        "  d,  delete [hex]       remove one breakpoint, or all when bare",
        "  bl, breaks             list the breakpoints",
        "  r,  regs               dump registers, flags, IRQs, LCD, banks",
        "  m,  mem <hex> [n]      hexdump n bytes (default 128)",
        "  w,  write <hex> <hex>  poke one byte",
        "      dis [hex] [n]      disassemble n instructions (default 16)",
        "  t,  trace [on|off]     toggle the CPU trace flag",
        "      save <path>        write a save state",
        "      load <path>        read a save state",
        "      reset              reset the machine",
        "  q,  quit               stop the emulator and exit",
        "  h,  help, ?            this list",
        NULL
    };
    int i;
    for (i = 0; lines[i] != NULL; i++)
        printf("%s\n", lines[i]);
}

static void print_one_line(GB *gb)
{
    char tl[192];
    gb_trace_line(gb, tl, sizeof tl);
    printf("%s\n", tl);
}

/* Execute `n` instructions, echoing the trace line of each one first. */
static void cmd_step(GB *gb, long n)
{
    long i;
    if (n < 1) n = 1;
    for (i = 0; i < n; i++) {
        print_one_line(gb);
        gb_step(gb);
        if (gb->cpu.illegal_op) {
            printf("!! illegal opcode $%02X — CPU stopped\n",
                   (unsigned)gb->cpu.illegal_opcode);
            break;
        }
    }
}

/* Step over a CALL by running until it returns to the following address with
 * the stack pointer restored. Bounded so a runaway ROM cannot hang the REPL. */
static void cmd_next(GB *gb)
{
    const u64 max_insn = 200000000ull;
    u8   op;
    u16  ret_addr, sp_before;
    u64  executed = 0;

    op = mmu_peek(gb, gb->cpu.pc);
    if (op != 0xCD && op != 0xC4 && op != 0xCC && op != 0xD4 && op != 0xDC) {
        cmd_step(gb, 1);
        return;
    }

    ret_addr  = (u16)(((u32)gb->cpu.pc + 3u) & 0xFFFFu);
    sp_before = gb->cpu.sp;
    print_one_line(gb);

    for (;;) {
        gb_step(gb);
        executed++;

        if (gb->cpu.illegal_op) {
            printf("!! illegal opcode $%02X — CPU stopped\n",
                   (unsigned)gb->cpu.illegal_opcode);
            break;
        }
        if (gb->cpu.pc == ret_addr && gb->cpu.sp == sp_before) break;
        if (bp_is_set(gb, gb->cpu.pc)) {
            printf("breakpoint hit at $%04X during step-over\n",
                   (unsigned)gb->cpu.pc);
            gb->hit_breakpoint = true;
            break;
        }
        if (executed >= max_insn) {
            printf("step-over gave up after %llu instructions\n",
                   (unsigned long long)executed);
            break;
        }
    }
    print_one_line(gb);
}

static void cmd_regs(GB *gb)
{
    const CPU *c   = &gb->cpu;
    u8  ie         = gb->mmu.ie;
    u8  ifr        = (u8)(gb->mmu.ifr | 0xE0u);
    int n          = bp_count(gb);

    printf("AF:%04X  BC:%04X  DE:%04X  HL:%04X  SP:%04X  PC:%04X\n",
           (unsigned)((u32)c->a << 8 | c->f), (unsigned)((u32)c->b << 8 | c->c),
           (unsigned)((u32)c->d << 8 | c->e), (unsigned)((u32)c->h << 8 | c->l),
           (unsigned)c->sp, (unsigned)c->pc);
    printf("flags  Z=%d N=%d H=%d C=%d   (F=$%02X)\n",
           (c->f & FLAG_Z) ? 1 : 0, (c->f & FLAG_N) ? 1 : 0,
           (c->f & FLAG_H) ? 1 : 0, (c->f & FLAG_C) ? 1 : 0,
           (unsigned)c->f);
    printf("IME:%d  ime_pending:%d  halted:%d  stopped:%d  halt_bug:%d\n",
           c->ime ? 1 : 0, c->ime_pending ? 1 : 0, c->halted ? 1 : 0,
           c->stopped ? 1 : 0, c->halt_bug ? 1 : 0);
    printf("IE:$%02X [%c%c%c%c%c]  IF:$%02X [%c%c%c%c%c]\n",
           (unsigned)ie,
           (ie & INT_JOYPAD) ? 'J' : '-', (ie & INT_SERIAL) ? 'S' : '-',
           (ie & INT_TIMER)  ? 'T' : '-', (ie & INT_STAT)   ? 'L' : '-',
           (ie & INT_VBLANK) ? 'V' : '-',
           (unsigned)ifr,
           (ifr & INT_JOYPAD) ? 'J' : '-', (ifr & INT_SERIAL) ? 'S' : '-',
           (ifr & INT_TIMER)  ? 'T' : '-', (ifr & INT_STAT)   ? 'L' : '-',
           (ifr & INT_VBLANK) ? 'V' : '-');
    printf("LCDC:$%02X STAT:$%02X LY:%3u LYC:%3u mode:%u  SCX:%3u SCY:%3u "
           "WX:%3u WY:%3u\n",
           (unsigned)gb->ppu.lcdc, (unsigned)gb->ppu.stat,
           (unsigned)gb->ppu.ly, (unsigned)gb->ppu.lyc,
           (unsigned)(gb->ppu.mode & STAT_MODE_MASK),
           (unsigned)gb->ppu.scx, (unsigned)gb->ppu.scy,
           (unsigned)gb->ppu.wx, (unsigned)gb->ppu.wy);
    printf("ROM bank:%u (low:%u)  RAM bank:%u  RAM %s  WRAM bank:%u  "
           "VRAM bank:%u\n",
           (unsigned)gb->cart.rom_bank, (unsigned)gb->cart.rom_bank_0,
           (unsigned)gb->cart.ram_bank,
           gb->cart.ram_enabled ? "enabled" : "disabled",
           (unsigned)gb->mmu.wram_bank, (unsigned)gb->ppu.vram_bank);
    printf("model:%s  cgb_mode:%d  speed:%dx  boot_rom:%s  breakpoints:%d\n",
           gb->cgb_hw ? "CGB" : "DMG", gb->cgb_mode ? 1 : 0,
           (gb->speed == 2) ? 2 : 1,
           gb->mmu.boot_active ? "active" : "off", n);
    printf("cycles:%llu  frames:%u\n",
           (unsigned long long)gb->cycles, (unsigned)gb->ppu.frames);
    print_one_line(gb);
}

static void cmd_mem(GB *gb, u32 start, long count)
{
    char asc[17];
    long i;

    if (count < 1)        count = 128;
    if (count > 0x10000L) count = 0x10000L;

    for (i = 0; i < count; ) {
        long n = count - i;
        int  j;
        if (n > 16) n = 16;

        printf("%04X:", (unsigned)((start + (u32)i) & 0xFFFFu));
        for (j = 0; j < 16; j++) {
            if (j < (int)n) {
                u8 v = mmu_peek(gb, (u16)((start + (u32)i + (u32)j) & 0xFFFFu));
                printf(" %02X", (unsigned)v);
                asc[j] = (v >= 0x20u && v < 0x7Fu) ? (char)v : '.';
            } else {
                printf("   ");
                asc[j] = ' ';
            }
        }
        asc[16] = '\0';
        printf("  |%s|\n", asc);
        i += n;
    }
}

static void cmd_dis(GB *gb, u32 start, long count)
{
    u32  addr = start & 0xFFFFu;
    long i;

    if (count < 1)   count = 16;
    if (count > 4096) count = 4096;

    for (i = 0; i < count; i++) {
        char text[80];
        char bytes[16];
        int  len = gb_disasm(gb, (u16)addr, text, sizeof text);
        int  k;
        size_t pos = 0;

        if (len < 1) len = 1;
        if (len > 3) len = 3;

        bytes[0] = '\0';
        for (k = 0; k < 3; k++) {
            char cell[8];
            if (k < len)
                snprintf(cell, sizeof cell, "%02X ",
                         (unsigned)mmu_peek(gb,
                             (u16)((addr + (u32)k) & 0xFFFFu)));
            else
                snprintf(cell, sizeof cell, "   ");
            sb_add(bytes, sizeof bytes, &pos, cell);
        }
        printf("%c %04X: %s %s\n",
               ((u16)addr == gb->cpu.pc) ? '>' : ' ',
               (unsigned)addr, bytes, text);
        addr = (addr + (u32)len) & 0xFFFFu;
    }
}

static void cmd_breaks(GB *gb)
{
    int n = bp_count(gb);
    int i;
    if (n == 0) { printf("no breakpoints\n"); return; }
    for (i = 0; i < n; i++)
        printf("  [%d] $%04X\n", i, (unsigned)(gb->breakpoints[i] & 0xFFFFu));
}

/* ========================================================================== */
/*  REPL                                                                      */
/* ========================================================================== */

void debugger_repl(GB *gb)
{
    char  line[512];
    char  work[512];
    char  path[512];
    char *argv[8];

    if (!gb) return;

    gb->paused = true;

    printf("\n==================== gameboy debugger ====================\n");
    printf(" type 'h' for help, 'c' to continue, 'q' to quit\n");
    printf("----------------------------------------------------------\n");
    print_one_line(gb);

    for (;;) {
        int   argc;
        char *cmd;

        printf("(gb) ");
        fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) {
            /* EOF (Ctrl-D) behaves exactly like "continue". */
            printf("\n");
            gb->paused         = false;
            gb->hit_breakpoint = false;
            return;
        }

        /* Keep an untouched copy: tokenize() writes NULs into its buffer and
         * "save"/"load" need the raw remainder (paths may contain spaces). */
        memcpy(work, line, sizeof work > sizeof line ? sizeof line : sizeof work);
        work[sizeof work - 1] = '\0';

        argc = tokenize(line, argv, (int)(sizeof argv / sizeof argv[0]));
        if (argc <= 0) continue;
        cmd = argv[0];

        /* ---- execution ------------------------------------------------ */
        if (str_ieq(cmd, "s") || str_ieq(cmd, "step")) {
            long n = 1;
            if (argc >= 2 && !parse_count(argv[1], &n)) {
                printf("bad count: %s\n", argv[1]);
                continue;
            }
            cmd_step(gb, n);
            continue;
        }
        if (str_ieq(cmd, "n") || str_ieq(cmd, "next")) {
            cmd_next(gb);
            continue;
        }
        if (str_ieq(cmd, "c") || str_ieq(cmd, "cont") ||
            str_ieq(cmd, "continue")) {
            gb->paused         = false;
            gb->hit_breakpoint = false;
            printf("continuing\n");
            return;
        }
        if (str_ieq(cmd, "q") || str_ieq(cmd, "quit") || str_ieq(cmd, "exit")) {
            gb->running        = false;
            gb->paused         = false;
            gb->hit_breakpoint = false;
            printf("quitting\n");
            return;
        }

        /* ---- breakpoints ---------------------------------------------- */
        if (str_ieq(cmd, "b") || str_ieq(cmd, "break") || str_ieq(cmd, "bp")) {
            u32 v;
            if (argc < 2) { printf("usage: b <hex address>\n"); continue; }
            if (!parse_hex(argv[1], &v)) {
                printf("bad address: %s\n", argv[1]);
                continue;
            }
            if (bp_add(gb, (u16)(v & 0xFFFFu)))
                printf("breakpoint at $%04X\n", (unsigned)(v & 0xFFFFu));
            else
                printf("breakpoint table full (%d entries)\n", bp_capacity(gb));
            continue;
        }
        if (str_ieq(cmd, "d") || str_ieq(cmd, "delete") ||
            str_ieq(cmd, "del")) {
            u32 v;
            if (argc < 2) {
                gb->n_breakpoints = 0;
                memset(gb->breakpoints, 0, sizeof gb->breakpoints);
                printf("all breakpoints cleared\n");
                continue;
            }
            if (!parse_hex(argv[1], &v)) {
                printf("bad address: %s\n", argv[1]);
                continue;
            }
            if (bp_remove(gb, (u16)(v & 0xFFFFu)))
                printf("removed breakpoint at $%04X\n",
                       (unsigned)(v & 0xFFFFu));
            else
                printf("no breakpoint at $%04X\n", (unsigned)(v & 0xFFFFu));
            continue;
        }
        if (str_ieq(cmd, "bl") || str_ieq(cmd, "breaks") ||
            str_ieq(cmd, "blist")) {
            cmd_breaks(gb);
            continue;
        }

        /* ---- inspection ------------------------------------------------ */
        if (str_ieq(cmd, "r") || str_ieq(cmd, "regs") ||
            str_ieq(cmd, "reg")) {
            cmd_regs(gb);
            continue;
        }
        if (str_ieq(cmd, "m") || str_ieq(cmd, "mem")) {
            u32  addr = 0;
            long n    = 128;
            if (argc < 2) { printf("usage: m <hex address> [count]\n"); continue; }
            if (!parse_hex(argv[1], &addr)) {
                printf("bad address: %s\n", argv[1]);
                continue;
            }
            if (argc >= 3 && !parse_count(argv[2], &n)) {
                printf("bad count: %s\n", argv[2]);
                continue;
            }
            cmd_mem(gb, addr & 0xFFFFu, n);
            continue;
        }
        if (str_ieq(cmd, "dis") || str_ieq(cmd, "u") ||
            str_ieq(cmd, "disasm")) {
            u32  addr = gb->cpu.pc;
            long n    = 16;
            if (argc >= 2 && !parse_hex(argv[1], &addr)) {
                printf("bad address: %s\n", argv[1]);
                continue;
            }
            if (argc >= 3 && !parse_count(argv[2], &n)) {
                printf("bad count: %s\n", argv[2]);
                continue;
            }
            cmd_dis(gb, addr & 0xFFFFu, n);
            continue;
        }

        /* ---- mutation --------------------------------------------------- */
        if (str_ieq(cmd, "w") || str_ieq(cmd, "write") ||
            str_ieq(cmd, "poke")) {
            u32 addr = 0, val = 0;
            if (argc < 3) {
                printf("usage: w <hex address> <hex value>\n");
                continue;
            }
            if (!parse_hex(argv[1], &addr)) {
                printf("bad address: %s\n", argv[1]);
                continue;
            }
            if (!parse_hex(argv[2], &val)) {
                printf("bad value: %s\n", argv[2]);
                continue;
            }
            mmu_poke(gb, (u16)(addr & 0xFFFFu), (u8)(val & 0xFFu));
            printf("[$%04X] <- $%02X\n",
                   (unsigned)(addr & 0xFFFFu), (unsigned)(val & 0xFFu));
            continue;
        }
        if (str_ieq(cmd, "t") || str_ieq(cmd, "trace")) {
            if (argc >= 2) {
                if (str_ieq(argv[1], "on") || str_ieq(argv[1], "1"))
                    gb->trace = true;
                else if (str_ieq(argv[1], "off") || str_ieq(argv[1], "0"))
                    gb->trace = false;
                else {
                    printf("usage: t [on|off]\n");
                    continue;
                }
            } else {
                gb->trace = !gb->trace;
            }
            printf("trace %s\n", gb->trace ? "on" : "off");
            continue;
        }
        if (str_ieq(cmd, "reset")) {
            gb_reset(gb);
            printf("machine reset\n");
            print_one_line(gb);
            continue;
        }

        /* ---- save states ------------------------------------------------ */
        if (str_ieq(cmd, "save")) {
            if (!rest_after_cmd(work, path, sizeof path)) {
                printf("usage: save <path>\n");
                continue;
            }
            if (savestate_save(gb, path))
                printf("saved state to %s\n", path);
            else
                printf("save failed: %.255s\n", gb->last_error);
            continue;
        }
        if (str_ieq(cmd, "load")) {
            if (!rest_after_cmd(work, path, sizeof path)) {
                printf("usage: load <path>\n");
                continue;
            }
            if (savestate_load(gb, path)) {
                printf("loaded state from %s\n", path);
                print_one_line(gb);
            } else {
                printf("load failed: %.255s\n", gb->last_error);
            }
            continue;
        }

        /* ---- help ------------------------------------------------------- */
        if (str_ieq(cmd, "h") || str_ieq(cmd, "help") || str_ieq(cmd, "?")) {
            print_help();
            continue;
        }

        printf("unknown command '%s' — type 'h' for help\n", cmd);
    }
}

/* CONTRACT-NOTE:
 *  1. gb.h declares no accessor for "the current ROM bank", so the `r` command
 *     reports gb->cart.rom_bank (the 0x4000-0x7FFF bank) together with
 *     gb->cart.rom_bank_0 (the 0x0000-0x3FFF bank, which MBC1 mode 1 can move).
 *  2. gb.h gives PPU::mode its own field rather than deriving it from STAT, so
 *     the register dump prints `gb->ppu.mode & STAT_MODE_MASK`; if a module
 *     ever lets the two disagree, STAT is the authoritative value for a game.
 *  3. The `next` command detects only the five CALL opcodes (0xCD, 0xC4, 0xCC,
 *     0xD4, 0xDC). RST is intentionally *not* stepped over: an RST vector is
 *     usually the thing a user is trying to inspect. Step-over terminates on
 *     "PC == return address AND SP == the SP seen before the CALL", which is
 *     recursion-safe, and is additionally bounded to 200M instructions so a
 *     runaway ROM can never wedge the REPL.
 */
