/* =============================================================================
 *  serial.c — Link port: SB (0xFF01) and SC (0xFF02)
 *
 *      SB  8-bit shift register.  During a transfer the outgoing bit leaves
 *          from bit 7 while the incoming bit enters at bit 0.
 *      SC  bit 7  transfer start / in progress (cleared by hardware when done)
 *          bit 1  clock speed, CGB mode only (0 = 8 KHz, 1 = 256 KHz)
 *          bit 0  clock source (1 = internal / this GB is the master)
 *
 *  No physical cable is attached, so the far end of the wire floats high and a
 *  master transfer shifts in 0xFF.  When the clock source is external (bit 0
 *  clear) no clock edge will ever arrive, so the transfer simply stays pending
 *  forever — which is exactly what real hardware does with a dangling port.
 *
 *  Timing: the internal clock runs at 8192 Hz, i.e. one bit every 512 T-cycles
 *  (16 T-cycles in CGB high-speed mode).  Eight bits complete the transfer,
 *  which then sets SB to the received byte and requests the serial interrupt.
 *
 *  Because Blargg's test ROMs report their results by writing each character
 *  to SB and kicking off a master transfer, every completed byte is also
 *  appended to gb->serial.log, a NUL-terminated capture buffer.
 * ========================================================================== */
#include "gb.h"

#define SC_START      0x80u
#define SC_HIGH_SPEED 0x02u
#define SC_INTERNAL   0x01u

/* Unused SC bits read back as 1.  CGB mode exposes bit 1, DMG does not. */
#define SC_UNUSED_CGB 0x7Cu
#define SC_UNUSED_DMG 0x7Eu

/* Open-bus value shifted in when nothing is connected to the link port. */
#define SERIAL_NO_PEER 0xFFu

/* --------------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------------ */
static bool serial_cgb(const GB *gb)
{
    /* The fast clock and the extra readable bit are CGB-mode features; a DMG
     * cartridge running on CGB hardware does not get them. */
    return gb->cgb_mode;
}

/* T-cycles per shifted bit for the current SC settings. */
static int serial_bit_period(const GB *gb)
{
    if (serial_cgb(gb) && (gb->serial.sc & SC_HIGH_SPEED))
        return 16;      /* 262144 Hz shift clock                              */
    return 512;         /*   8192 Hz shift clock                              */
}

/* Append one byte to the capture log, never overflowing and always leaving it
 * NUL-terminated.  Silently stops once the buffer is full. */
static void serial_log_put(GB *gb, u8 byte)
{
    const int cap = (int)sizeof gb->serial.log;   /* 4096                     */
    int len = gb->serial.log_len;

    /* Defensive clamp: a corrupt save state must not let us index outside. */
    if (len < 0 || len >= cap - 1) {
        if (len < 0 || len > cap - 1)
            gb->serial.log_len = (len < 0) ? 0 : (cap - 1);
        gb->serial.log[gb->serial.log_len] = '\0';
        return;
    }

    gb->serial.log[len]     = (char)byte;
    gb->serial.log[len + 1] = '\0';
    gb->serial.log_len      = len + 1;
}

/* Finish the byte currently in flight. */
static void serial_finish(GB *gb)
{
    u8 sent = gb->serial.sb;     /* what the game put in the shift register   */

    /* Nothing on the other end: the register ends up full of 1s. */
    gb->serial.sb           = SERIAL_NO_PEER;
    gb->serial.sc           = (u8)(gb->serial.sc & (u8)~SC_START);
    gb->serial.transferring = false;
    gb->serial.bit          = 0;
    gb->serial.timer        = 0;

    serial_log_put(gb, sent);
    gb_request_interrupt(gb, INT_SERIAL);
}

/* --------------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------------ */
void serial_reset(GB *gb)
{
    if (!gb)
        return;

    gb->serial.sb           = 0x00;
    gb->serial.sc           = 0x7E;
    gb->serial.bit          = 0;
    gb->serial.timer        = 0;
    gb->serial.transferring = false;
    gb->serial.log_len      = 0;
    gb->serial.log[0]       = '\0';
}

void serial_tick(GB *gb, int t)
{
    int period;

    if (!gb || t <= 0)
        return;
    if (!gb->serial.transferring)
        return;          /* idle, or waiting on an external clock that never comes */

    period = serial_bit_period(gb);          /* always >= 16, loop terminates */
    gb->serial.timer -= t;

    while (gb->serial.timer <= 0) {
        gb->serial.timer += period;
        gb->serial.bit++;
        if (gb->serial.bit >= 8) {
            serial_finish(gb);
            return;
        }
    }
}

u8 serial_read(GB *gb, u16 addr)
{
    if (!gb)
        return 0xFF;

    switch (addr) {
    case 0xFF01:
        return gb->serial.sb;
    case 0xFF02:
        return (u8)(gb->serial.sc |
                    (serial_cgb(gb) ? SC_UNUSED_CGB : SC_UNUSED_DMG));
    default:
        return 0xFF;
    }
}

void serial_write(GB *gb, u16 addr, u8 val)
{
    u8 writable;

    if (!gb)
        return;

    switch (addr) {
    case 0xFF01:
        gb->serial.sb = val;
        break;

    case 0xFF02:
        /* Bit 1 only exists in CGB mode. */
        writable = serial_cgb(gb) ? (u8)(SC_START | SC_HIGH_SPEED | SC_INTERNAL)
                                  : (u8)(SC_START | SC_INTERNAL);
        gb->serial.sc = (u8)(val & writable);

        if ((gb->serial.sc & SC_START) && (gb->serial.sc & SC_INTERNAL)) {
            /* Master: start clocking 8 bits out of SB. */
            gb->serial.transferring = true;
            gb->serial.bit          = 0;
            gb->serial.timer        = serial_bit_period(gb);
        } else {
            /* Either the transfer was cancelled, or an external clock was
             * requested — with no cable attached that clock never arrives, so
             * the request just sits there with SC bit 7 still set. */
            gb->serial.transferring = false;
            gb->serial.bit          = 0;
            gb->serial.timer        = 0;
        }
        break;

    default:
        break;
    }
}
