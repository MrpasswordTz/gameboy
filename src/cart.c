/* =============================================================================
 *  cart.c — Cartridge image loading, header parsing, MBC emulation,
 *           battery-backed SRAM and the MBC3 real-time clock.
 *
 *  Owns two heap allocations: Cart.rom and Cart.ram. Everything else lives in
 *  `struct GB`. All guest-derived indices are masked and then bounds-checked
 *  before they touch memory — a malicious ROM must not be able to make the
 *  emulator read or write outside those two blocks.
 * ========================================================================== */
#include "gb.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --------------------------------------------------------------- limits --- */
#define CART_MIN_SIZE   0x150u                 /* must contain a full header  */
#define CART_MAX_SIZE   (8u * 1024u * 1024u)   /* 8 MiB, biggest MBC5 image   */
#define ROM_BANK_SIZE   0x4000u
#define RAM_BANK_SIZE   0x2000u
#define MBC2_RAM_SIZE   0x200u                 /* 512 x 4 bits, built in      */
#define RTC_BLOB_SIZE   18u                    /* 5 live + 5 latched + 8 time */

/* ======================================================================== */
/*  small helpers                                                           */
/* ======================================================================== */

/* snprintf-style error reporting into gb->last_error (never strcpy). */
static void cart_set_error(GB *gb, const char *fmt, ...)
{
    va_list ap;
    if (!gb) return;
    va_start(ap, fmt);
    vsnprintf(gb->last_error, sizeof gb->last_error, fmt, ap);
    va_end(ap);
    gb->last_error[sizeof gb->last_error - 1] = '\0';
}

static char lower_ascii(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

/* Case-insensitive "does `s` (length slen) end with `suffix`?" */
static bool ends_with_ci(const char *s, size_t slen, const char *suffix)
{
    size_t n = strlen(suffix);
    size_t i;
    if (n > slen) return false;
    for (i = 0; i < n; i++)
        if (lower_ascii(s[slen - n + i]) != lower_ascii(suffix[i])) return false;
    return true;
}

/* Wrap `bank` into [0, count). Uses a mask for the (usual) power-of-two case
 * and a modulo otherwise, so no index can ever escape the allocation.       */
static u32 bank_mask(u32 bank, u32 count)
{
    if (count == 0u) count = 1u;
    if ((count & (count - 1u)) == 0u) return bank & (count - 1u);
    return bank % count;
}

static u32 cart_rom_banks(const Cart *c)
{
    u32 n = (u32)(c->rom_size / ROM_BANK_SIZE);
    return n ? n : 1u;
}

static u32 cart_ram_banks(const Cart *c)
{
    u32 n = (u32)(c->ram_size / RAM_BANK_SIZE);
    return n ? n : 1u;
}

/* ======================================================================== */
/*  real-time clock                                                         */
/* ======================================================================== */

/* Advance the clock by exactly one second, reproducing the MBC3 wrap quirks:
 * each field is a narrow counter, so an out-of-range value written by the game
 * counts up to its own width and wraps without carrying.                    */
static void rtc_tick_second(RTC *r)
{
    u16 day;
    u8  v;

    if (r->dh & 0x40u) return;                 /* halted */

    v = (u8)(r->s + 1u);
    if (v == 60u) { r->s = 0u; } else { r->s = (u8)(v & 0x3Fu); return; }

    v = (u8)(r->m + 1u);
    if (v == 60u) { r->m = 0u; } else { r->m = (u8)(v & 0x3Fu); return; }

    v = (u8)(r->h + 1u);
    if (v == 24u) { r->h = 0u; } else { r->h = (u8)(v & 0x1Fu); return; }

    day = (u16)((((u16)(r->dh & 0x01u)) << 8) | r->dl);
    day = (u16)(day + 1u);
    if (day > 0x1FFu) { day = 0u; r->dh = (u8)(r->dh | 0x80u); }   /* day carry */
    r->dl = (u8)(day & 0xFFu);
    r->dh = (u8)((r->dh & 0xFEu) | (u8)((day >> 8) & 0x01u));
}

/* Bulk catch-up used when a battery save is restored after real time passed.
 * Done arithmetically so a years-old save does not spin for 10^8 iterations. */
static void rtc_advance(RTC *r, u64 secs)
{
    u64 total, days, rem, day;

    if (secs == 0u || (r->dh & 0x40u)) return;

    /* Normalise any out-of-range field first; games never leave them so. */
    if (r->s >= 60u) r->s = (u8)(r->s % 60u);
    if (r->m >= 60u) r->m = (u8)(r->m % 60u);
    if (r->h >= 24u) r->h = (u8)(r->h % 24u);

    total = (u64)r->s + 60ull * (u64)r->m + 3600ull * (u64)r->h + secs;
    days  = total / 86400ull;
    rem   = total % 86400ull;

    r->s = (u8)(rem % 60ull); rem /= 60ull;
    r->m = (u8)(rem % 60ull); rem /= 60ull;
    r->h = (u8)(rem % 24ull);

    day = (u64)((((u16)(r->dh & 0x01u)) << 8) | r->dl) + days;
    if (day > 0x1FFull) r->dh = (u8)(r->dh | 0x80u);
    day &= 0x1FFull;
    r->dl = (u8)(day & 0xFFull);
    r->dh = (u8)((r->dh & 0xFEu) | (u8)((day >> 8) & 0x01ull));
}

static void rtc_latch(Cart *c)
{
    c->rtc.latched[0] = c->rtc.s;
    c->rtc.latched[1] = c->rtc.m;
    c->rtc.latched[2] = c->rtc.h;
    c->rtc.latched[3] = c->rtc.dl;
    c->rtc.latched[4] = c->rtc.dh;
}

void cart_tick_rtc(GB *gb, int t)
{
    Cart *c;
    RTC  *r;
    u64   secs;

    if (!gb || t <= 0) return;
    c = &gb->cart;
    if (!c->has_rtc) return;

    r = &c->rtc;
    r->sub_cycles += (u64)t;
    if (r->sub_cycles < (u64)GB_CPU_HZ) return;

    secs = r->sub_cycles / (u64)GB_CPU_HZ;
    r->sub_cycles -= secs * (u64)GB_CPU_HZ;

    if (r->dh & 0x40u) return;                 /* halted: the seconds are lost */

    while (secs-- > 0u) rtc_tick_second(r);
    c->ram_dirty = true;                       /* clock state wants flushing   */
}

/* ======================================================================== */
/*  bank resolution                                                         */
/* ======================================================================== */

/* Recompute rom_bank / rom_bank_0 (and, for the MBC1 family, ram_bank) from
 * the raw register latches. Called after every mapper register write.       */
static void cart_refresh_banks(Cart *c)
{
    u32 nrom = cart_rom_banks(c);
    u32 nram = cart_ram_banks(c);
    u32 lo, hi;

    switch (c->mbc) {
    case MBC_NONE:
        c->rom_bank   = bank_mask(1u, nrom);
        c->rom_bank_0 = 0u;
        c->ram_bank   = 0u;
        break;

    case MBC_2:
        lo = (u32)(c->bank_lo & 0x0Fu);
        if (lo == 0u) lo = 1u;
        c->rom_bank   = bank_mask(lo, nrom);
        c->rom_bank_0 = 0u;
        c->ram_bank   = 0u;
        break;

    case MBC_3:
        lo = (u32)(c->bank_lo & 0x7Fu);
        if (lo == 0u) lo = 1u;
        c->rom_bank   = bank_mask(lo, nrom);
        c->rom_bank_0 = 0u;
        c->ram_bank   = bank_mask(c->ram_bank, nram);
        break;

    case MBC_5:
        lo = ((u32)(c->bank_hi & 0x01u) << 8) | (u32)c->bank_lo;  /* bank 0 ok */
        c->rom_bank   = bank_mask(lo, nrom);
        c->rom_bank_0 = 0u;
        c->ram_bank   = bank_mask(c->ram_bank, nram);
        break;

    case MBC_1:
    case MBC_HUC1:
    case MBC_UNKNOWN:
    default:
        lo = (u32)(c->bank_lo & 0x1Fu);
        if (lo == 0u) lo = 1u;                 /* bank 0 always reads as 1     */
        hi = (u32)(c->bank_hi & 0x03u);
        c->rom_bank = bank_mask((hi << 5) | lo, nrom);
        if (c->mode) {
            c->rom_bank_0 = bank_mask(hi << 5, nrom);
            c->ram_bank   = bank_mask(hi, nram);
        } else {
            c->rom_bank_0 = 0u;
            c->ram_bank   = 0u;
        }
        break;
    }
}

/* ======================================================================== */
/*  bus access                                                              */
/* ======================================================================== */

u8 cart_read_rom(GB *gb, u16 addr)
{
    const Cart *c;
    size_t off;

    if (!gb) return 0xFF;
    c = &gb->cart;
    if (!c->rom || c->rom_size == 0u) return 0xFF;

    if (addr < 0x4000u)
        off = (size_t)c->rom_bank_0 * (size_t)ROM_BANK_SIZE + (size_t)addr;
    else
        off = (size_t)c->rom_bank * (size_t)ROM_BANK_SIZE + (size_t)(addr & 0x3FFFu);

    if (off >= c->rom_size) return 0xFF;       /* hard bounds check            */
    return c->rom[off];
}

u8 cart_read_ram(GB *gb, u16 addr)
{
    const Cart *c;
    size_t off;

    if (!gb) return 0xFF;
    c = &gb->cart;
    if (!c->ram_enabled) return 0xFF;

    /* MBC3 maps the latched RTC registers over the RAM window. */
    if ((c->mbc == MBC_3) && c->has_rtc && c->rtc_select >= 0x08u && c->rtc_select <= 0x0Cu)
        return c->rtc.latched[c->rtc_select - 0x08u];

    if (!c->ram || c->ram_size == 0u) return 0xFF;

    if (c->mbc == MBC_2) {
        off = (size_t)(addr & 0x01FFu);        /* 512 nibbles, echoed to BFFF  */
        if (off >= c->ram_size) return 0xFF;
        return (u8)(c->ram[off] | 0xF0u);      /* upper nibble reads open-bus  */
    }

    off = (size_t)bank_mask(c->ram_bank, cart_ram_banks(c)) * (size_t)RAM_BANK_SIZE
        + (size_t)(addr & 0x1FFFu);
    if (off >= c->ram_size) return 0xFF;
    return c->ram[off];
}

void cart_write_ram(GB *gb, u16 addr, u8 val)
{
    Cart *c;
    size_t off;

    if (!gb) return;
    c = &gb->cart;
    if (!c->ram_enabled) return;

    if ((c->mbc == MBC_3) && c->has_rtc && c->rtc_select >= 0x08u && c->rtc_select <= 0x0Cu) {
        switch (c->rtc_select) {
        case 0x08u: c->rtc.s  = (u8)(val & 0x3Fu); c->rtc.sub_cycles = 0u; break;
        case 0x09u: c->rtc.m  = (u8)(val & 0x3Fu); break;
        case 0x0Au: c->rtc.h  = (u8)(val & 0x1Fu); break;
        case 0x0Bu: c->rtc.dl = val;               break;
        case 0x0Cu: c->rtc.dh = (u8)(val & 0xC1u); break;
        default: break;
        }
        c->ram_dirty = true;
        return;
    }

    if (!c->ram || c->ram_size == 0u) return;

    if (c->mbc == MBC_2) {
        off = (size_t)(addr & 0x01FFu);
        if (off >= c->ram_size) return;
        c->ram[off] = (u8)(val & 0x0Fu);       /* 4-bit cells                  */
        c->ram_dirty = true;
        return;
    }

    off = (size_t)bank_mask(c->ram_bank, cart_ram_banks(c)) * (size_t)RAM_BANK_SIZE
        + (size_t)(addr & 0x1FFFu);
    if (off >= c->ram_size) return;
    c->ram[off] = val;
    c->ram_dirty = true;
}

void cart_write_rom(GB *gb, u16 addr, u8 val)
{
    Cart *c;

    if (!gb) return;
    c = &gb->cart;

    switch (c->mbc) {
    /* ------------------------------------------------------------ none --- */
    case MBC_NONE:
        /* No mapper: writes to the ROM area are simply dropped. */
        break;

    /* ------------------------------------------------------------ MBC2 --- */
    case MBC_2:
        /* Only 0x0000-0x3FFF decodes, and address bit 8 picks the register. */
        if (addr < 0x4000u) {
            if (addr & 0x0100u) {
                c->bank_lo = (u8)(val & 0x0Fu);
                if (c->bank_lo == 0u) c->bank_lo = 1u;
            } else {
                c->ram_enabled = ((val & 0x0Fu) == 0x0Au);
            }
        }
        break;

    /* ------------------------------------------------------------ MBC3 --- */
    case MBC_3:
        if (addr < 0x2000u) {
            c->ram_enabled = ((val & 0x0Fu) == 0x0Au);
        } else if (addr < 0x4000u) {
            c->bank_lo = (u8)(val & 0x7Fu);
            if (c->bank_lo == 0u) c->bank_lo = 1u;
        } else if (addr < 0x6000u) {
            if (val <= 0x07u) {
                c->ram_bank   = (u32)val;
                c->rtc_select = 0u;
            } else if (val >= 0x08u && val <= 0x0Cu) {
                c->rtc_select = val;
            }
            /* values above 0x0C are not decoded by the mapper */
        } else {
            /* 0x6000-0x7FFF: 0 -> 1 latches the live clock. */
            if (val == 0x00u) {
                c->rtc.latch_armed = true;
            } else if (val == 0x01u) {
                if (c->rtc.latch_armed) rtc_latch(c);
                c->rtc.latch_armed = false;
            } else {
                c->rtc.latch_armed = false;
            }
        }
        break;

    /* ------------------------------------------------------------ MBC5 --- */
    case MBC_5:
        if (addr < 0x2000u) {
            c->ram_enabled = ((val & 0x0Fu) == 0x0Au);
        } else if (addr < 0x3000u) {
            c->bank_lo = val;                          /* ROM bank bits 0-7    */
        } else if (addr < 0x4000u) {
            c->bank_hi = (u8)(val & 0x01u);            /* ROM bank bit 8       */
        } else if (addr < 0x6000u) {
            /* Bit 3 drives the rumble motor on rumble carts, so it is not part
             * of the bank number there. */
            c->ram_bank = c->has_rumble ? (u32)(val & 0x07u) : (u32)(val & 0x0Fu);
        }
        /* 0x6000-0x7FFF is unmapped on MBC5 */
        break;

    /* --------------------------------------------- MBC1 / HuC1 / unknown -- */
    case MBC_1:
    case MBC_HUC1:
    case MBC_UNKNOWN:
    default:
        if (addr < 0x2000u) {
            c->ram_enabled = ((val & 0x0Fu) == 0x0Au);
        } else if (addr < 0x4000u) {
            c->bank_lo = (u8)(val & 0x1Fu);
            if (c->bank_lo == 0u) c->bank_lo = 1u;
        } else if (addr < 0x6000u) {
            c->bank_hi = (u8)(val & 0x03u);
        } else {
            c->mode = (u8)(val & 0x01u);
        }
        break;
    }

    cart_refresh_banks(c);
}

/* ======================================================================== */
/*  battery-backed saves                                                    */
/* ======================================================================== */

static void put_u64_le(u8 *p, u64 v)
{
    int i;
    for (i = 0; i < 8; i++) p[i] = (u8)((v >> (8 * i)) & 0xFFu);
}

static u64 get_u64_le(const u8 *p)
{
    u64 v = 0u;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | (u64)p[i];
    return v;
}

bool cart_save_ram(GB *gb)
{
    Cart  *c;
    FILE  *f;
    char   tmp[sizeof c->save_path + 8];   /* not a VLA: constant expression  */
    int    n;

    if (!gb) return false;
    c = &gb->cart;

    if (!c->has_battery || c->ram_size == 0u) return true;   /* nothing to do */
    if (!c->ram) return true;
    if (c->save_path[0] == '\0') {
        cart_set_error(gb, "cart: no save path available for this ROM");
        return false;
    }

    n = snprintf(tmp, sizeof tmp, "%s.tmp", c->save_path);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        cart_set_error(gb, "cart: save path too long");
        return false;
    }

    f = fopen(tmp, "wb");
    if (!f) {
        cart_set_error(gb, "cart: cannot create '%s'", tmp);
        return false;
    }

    if (fwrite(c->ram, 1u, c->ram_size, f) != c->ram_size) {
        cart_set_error(gb, "cart: short write to '%s'", tmp);
        fclose(f);
        remove(tmp);
        return false;
    }

    if (c->has_rtc) {
        u8 blob[RTC_BLOB_SIZE];
        memset(blob, 0, sizeof blob);
        c->rtc.base_unix = (s64)time(NULL);    /* anchor for the next restore */
        blob[0] = c->rtc.s;
        blob[1] = c->rtc.m;
        blob[2] = c->rtc.h;
        blob[3] = c->rtc.dl;
        blob[4] = c->rtc.dh;
        memcpy(blob + 5, c->rtc.latched, 5);
        put_u64_le(blob + 10, (u64)c->rtc.base_unix);
        if (fwrite(blob, 1u, sizeof blob, f) != sizeof blob) {
            cart_set_error(gb, "cart: short RTC write to '%s'", tmp);
            fclose(f);
            remove(tmp);
            return false;
        }
    }

    if (fflush(f) != 0) {
        cart_set_error(gb, "cart: flush failed for '%s'", tmp);
        fclose(f);
        remove(tmp);
        return false;
    }
    if (fclose(f) != 0) {
        cart_set_error(gb, "cart: close failed for '%s'", tmp);
        remove(tmp);
        return false;
    }

    /* Atomic replace: a crash mid-write can never corrupt the old save. */
    if (rename(tmp, c->save_path) != 0) {
        cart_set_error(gb, "cart: cannot rename '%s' -> '%s'", tmp, c->save_path);
        remove(tmp);
        return false;
    }

    c->ram_dirty = false;
    return true;
}

bool cart_load_ram(GB *gb)
{
    Cart   *c;
    FILE   *f;
    size_t  got;

    if (!gb) return false;
    c = &gb->cart;

    if (!c->has_battery || c->ram_size == 0u || !c->ram) return false;
    if (c->save_path[0] == '\0') return false;

    f = fopen(c->save_path, "rb");
    if (!f) return false;                      /* no save yet — not an error   */

    got = fread(c->ram, 1u, c->ram_size, f);
    if (got < c->ram_size) {
        /* Tolerate a short/truncated save: the remainder stays zeroed. */
        memset(c->ram + got, 0, c->ram_size - got);
    }

    if (c->has_rtc) {
        u8 blob[RTC_BLOB_SIZE];
        if (fread(blob, 1u, sizeof blob, f) == sizeof blob) {
            s64 saved, now;
            c->rtc.s  = (u8)(blob[0] & 0x3Fu);
            c->rtc.m  = (u8)(blob[1] & 0x3Fu);
            c->rtc.h  = (u8)(blob[2] & 0x1Fu);
            c->rtc.dl = blob[3];
            c->rtc.dh = (u8)(blob[4] & 0xC1u);
            memcpy(c->rtc.latched, blob + 5, 5);
            saved = (s64)get_u64_le(blob + 10);
            now   = (s64)time(NULL);
            if (saved > 0 && now > saved) rtc_advance(&c->rtc, (u64)(now - saved));
            c->rtc.base_unix  = now;
            c->rtc.sub_cycles = 0u;
            c->rtc.latch_armed = false;
        }
        /* A file that is exactly ram_size bytes simply has no RTC part. */
    }

    fclose(f);
    c->ram_dirty = false;
    return true;
}

/* ======================================================================== */
/*  header parsing / loading                                                */
/* ======================================================================== */

const char *gb_mbc_name(MbcType m)
{
    switch (m) {
    case MBC_NONE:    return "ROM ONLY";
    case MBC_1:       return "MBC1";
    case MBC_2:       return "MBC2";
    case MBC_3:       return "MBC3";
    case MBC_5:       return "MBC5";
    case MBC_HUC1:    return "HuC1";
    case MBC_UNKNOWN: return "UNKNOWN";
    default:          return "UNKNOWN";
    }
}

/* Returns false when the cartridge type byte is not one we recognise; the
 * caller then runs it as an MBC1, which is the most useful fallback.        */
static bool cart_decode_type(Cart *c, u8 type)
{
    c->mbc         = MBC_NONE;
    c->has_battery = false;
    c->has_rtc     = false;
    c->has_rumble  = false;

    switch (type) {
    case 0x00: c->mbc = MBC_NONE; return true;                       /* ROM   */
    case 0x08: c->mbc = MBC_NONE; return true;                       /* +RAM  */
    case 0x09: c->mbc = MBC_NONE; c->has_battery = true; return true;

    case 0x01: c->mbc = MBC_1; return true;
    case 0x02: c->mbc = MBC_1; return true;
    case 0x03: c->mbc = MBC_1; c->has_battery = true; return true;

    case 0x05: c->mbc = MBC_2; return true;
    case 0x06: c->mbc = MBC_2; c->has_battery = true; return true;

    case 0x0B: c->mbc = MBC_NONE; return true;                       /* MMM01 */
    case 0x0C: c->mbc = MBC_NONE; return true;
    case 0x0D: c->mbc = MBC_NONE; c->has_battery = true; return true;

    case 0x0F: c->mbc = MBC_3; c->has_rtc = true; c->has_battery = true; return true;
    case 0x10: c->mbc = MBC_3; c->has_rtc = true; c->has_battery = true; return true;
    case 0x11: c->mbc = MBC_3; return true;
    case 0x12: c->mbc = MBC_3; return true;
    case 0x13: c->mbc = MBC_3; c->has_battery = true; return true;

    case 0x19: c->mbc = MBC_5; return true;
    case 0x1A: c->mbc = MBC_5; return true;
    case 0x1B: c->mbc = MBC_5; c->has_battery = true; return true;
    case 0x1C: c->mbc = MBC_5; c->has_rumble = true; return true;
    case 0x1D: c->mbc = MBC_5; c->has_rumble = true; return true;
    case 0x1E: c->mbc = MBC_5; c->has_rumble = true; c->has_battery = true; return true;

    case 0xFF: c->mbc = MBC_HUC1; c->has_battery = true; return true; /* HuC1 */
    default:   break;
    }

    c->mbc = MBC_UNKNOWN;
    c->has_battery = true;     /* be generous: never silently lose a save file */
    return false;
}

static size_t cart_ram_size_for_code(u8 code)
{
    switch (code) {
    case 0x00: return 0u;
    case 0x01: return 8u  * 1024u;   /* nominally 2 KiB; a full bank is fine  */
    case 0x02: return 8u  * 1024u;
    case 0x03: return 32u * 1024u;
    case 0x04: return 128u * 1024u;
    case 0x05: return 64u * 1024u;
    default:   return 0u;
    }
}

/* ROM path -> save path: swap a trailing .gb/.gbc for .sav, else append it. */
static void cart_derive_save_path(Cart *c, const char *path)
{
    size_t len = strlen(path);
    size_t cut = len;
    int    n;

    c->save_path[0] = '\0';

    if (ends_with_ci(path, len, ".gbc"))      cut = len - 4u;
    else if (ends_with_ci(path, len, ".cgb")) cut = len - 4u;
    else if (ends_with_ci(path, len, ".gb"))  cut = len - 3u;

    if (cut > (size_t)2147483647) return;      /* absurd; leave it unset       */

    n = snprintf(c->save_path, sizeof c->save_path, "%.*s.sav", (int)cut, path);
    if (n < 0 || (size_t)n >= sizeof c->save_path) c->save_path[0] = '\0';
}

static void cart_parse_title(Cart *c, const u8 *rom)
{
    int i, last;

    memset(c->title, 0, sizeof c->title);
    for (i = 0; i < 16; i++) {
        u8 ch = rom[0x134 + i];
        if (ch < 0x20u || ch >= 0x7Fu) break;  /* stops at the CGB flag too    */
        c->title[i] = (char)ch;
    }
    c->title[16] = '\0';

    last = (int)strlen(c->title);
    while (last > 0 && c->title[last - 1] == ' ') c->title[--last] = '\0';
}

bool cart_load(GB *gb, const char *path)
{
    Cart  *c;
    FILE  *f;
    u8    *rom = NULL;
    long   fsz;
    size_t size, got, i;
    u8     hsum = 0;
    u32    gsum = 0;
    bool   known;

    if (!gb) return false;
    if (!path || path[0] == '\0') {
        cart_set_error(gb, "cart: no ROM path given");
        return false;
    }

    cart_unload(gb);                           /* flush + free any old image   */
    memset(&gb->cart, 0, sizeof gb->cart);
    c = &gb->cart;

    f = fopen(path, "rb");
    if (!f) {
        cart_set_error(gb, "cart: cannot open '%s'", path);
        return false;
    }
    if (fseek(f, 0L, SEEK_END) != 0) {
        cart_set_error(gb, "cart: cannot seek '%s'", path);
        fclose(f);
        return false;
    }
    fsz = ftell(f);
    if (fsz < 0) {
        cart_set_error(gb, "cart: cannot size '%s'", path);
        fclose(f);
        return false;
    }
    rewind(f);

    size = (size_t)fsz;
    if (size < (size_t)CART_MIN_SIZE) {
        cart_set_error(gb, "cart: '%s' is only %lu bytes; a ROM header needs %u",
                       path, (unsigned long)size, (unsigned)CART_MIN_SIZE);
        fclose(f);
        return false;
    }
    if (size > (size_t)CART_MAX_SIZE) {
        cart_set_error(gb, "cart: '%s' is %lu bytes; the limit is %u (8 MiB)",
                       path, (unsigned long)size, (unsigned)CART_MAX_SIZE);
        fclose(f);
        return false;
    }

    rom = (u8 *)calloc(1u, size);
    if (!rom) {
        cart_set_error(gb, "cart: out of memory allocating %lu bytes of ROM",
                       (unsigned long)size);
        fclose(f);
        return false;
    }

    got = fread(rom, 1u, size, f);
    fclose(f);
    if (got != size) {
        cart_set_error(gb, "cart: short read on '%s' (%lu of %lu bytes)",
                       path, (unsigned long)got, (unsigned long)size);
        free(rom);
        return false;
    }

    c->rom      = rom;
    c->rom_size = size;

    /* ---- header ---------------------------------------------------------- */
    cart_parse_title(c, rom);
    c->cgb_flag        = rom[0x143];
    c->sgb_flag        = rom[0x146];
    c->cart_type       = rom[0x147];
    c->rom_size_code   = rom[0x148];
    c->ram_size_code   = rom[0x149];
    c->header_checksum = rom[0x14D];
    c->global_checksum = (u16)(((u16)rom[0x14E] << 8) | rom[0x14F]);

    for (i = 0x134u; i <= 0x14Cu; i++) hsum = (u8)(hsum - rom[i] - 1u);
    c->computed_header_checksum = hsum;

    for (i = 0; i < size; i++) {
        if (i == 0x14Eu || i == 0x14Fu) continue;
        gsum += rom[i];
    }
    c->computed_global_checksum = (u16)(gsum & 0xFFFFu);

    /* ---- mapper ---------------------------------------------------------- */
    known = cart_decode_type(c, c->cart_type);

    /* ---- cartridge RAM --------------------------------------------------- */
    if (c->mbc == MBC_2) {
        c->ram_size = (size_t)MBC2_RAM_SIZE;   /* built in, ignores the code   */
    } else {
        c->ram_size = cart_ram_size_for_code(c->ram_size_code);
    }
    if (c->ram_size > 0u) {
        c->ram = (u8 *)calloc(1u, c->ram_size);
        if (!c->ram) {
            cart_set_error(gb, "cart: out of memory allocating %lu bytes of SRAM",
                           (unsigned long)c->ram_size);
            free(c->rom);
            memset(&gb->cart, 0, sizeof gb->cart);
            return false;
        }
    } else {
        c->ram = NULL;
    }

    /* ---- banking state --------------------------------------------------- */
    c->rom_bank    = 1u;
    c->rom_bank_0  = 0u;
    c->ram_bank    = 0u;
    c->ram_enabled = false;
    c->mode        = 0u;
    c->bank_lo     = 1u;
    c->bank_hi     = 0u;
    c->rtc_select  = 0u;
    c->ram_dirty   = false;
    cart_refresh_banks(c);

    /* ---- clock and battery ----------------------------------------------- */
    if (c->has_rtc) {
        memset(&c->rtc, 0, sizeof c->rtc);
        c->rtc.base_unix = (s64)time(NULL);
    }

    cart_derive_save_path(c, path);
    if (c->has_battery) (void)cart_load_ram(gb);

    if (!known) {
        cart_set_error(gb,
            "cart: unrecognised cartridge type 0x%02X in '%s'; running it as MBC1",
            c->cart_type, path);
    }
    return true;
}

void cart_unload(GB *gb)
{
    Cart *c;

    if (!gb) return;
    c = &gb->cart;

    if (c->has_battery && c->ram_dirty && c->ram && c->ram_size > 0u)
        (void)cart_save_ram(gb);

    free(c->rom);
    c->rom      = NULL;
    c->rom_size = 0u;

    free(c->ram);
    c->ram      = NULL;
    c->ram_size = 0u;

    c->ram_dirty   = false;
    c->ram_enabled = false;
    c->rom_bank    = 1u;
    c->rom_bank_0  = 0u;
    c->ram_bank    = 0u;
    c->rtc_select  = 0u;
    c->bank_lo     = 1u;
    c->bank_hi     = 0u;
    c->mode        = 0u;
}

/* CONTRACT-NOTE: three deliberate readings of the spec, none of which change
 * any declared signature or touch include/gb.h.
 *
 * 1. cart_read_ram() is specified to return 0xFF whenever ram == NULL, but an
 *    MBC3+TIMER+BATTERY cart (type 0x0F) has an RTC and no SRAM at all. The
 *    RTC-register check therefore runs before the ram == NULL guard, otherwise
 *    the clock would be unreadable on exactly the carts that only have a clock.
 *    The !ram_enabled guard still comes first, as specified.
 *
 * 2. cart_save_ram() obeys the letter of the spec and returns true without
 *    writing anything when ram_size == 0. A consequence is that a type-0x0F
 *    cart (RTC, no SRAM) does not persist its clock across runs; every real
 *    RTC cart in the wild is type 0x10 or 0x0F+SRAM, so this is harmless, but
 *    it is the one case where "do nothing when ram_size == 0" and "so the
 *    clock survives" pull in opposite directions.
 *
 * 3. Cart has no field for MBC3/MBC5's raw RAM-bank latch, only the resolved
 *    `ram_bank`. Those mappers therefore store the already-masked bank there
 *    and cart_refresh_banks() re-masks it; masking is idempotent for both the
 *    power-of-two and the modulo path, so re-entering refresh cannot drift.
 *    MBC1/HuC1 recompute ram_bank from bank_hi + mode as usual.
 *
 * Also worth recording: ram_size_code 1 (nominally 2 KiB) allocates a full
 * 8 KiB bank, as the assignment permits; MBC2 always gets 512 bytes and stores
 * one nibble per byte, reading back as (value | 0xF0). Every ROM/RAM index is
 * masked to the bank count and then bounds-checked against the real allocation
 * size, so no guest write can escape the two heap blocks this file owns.
 */
