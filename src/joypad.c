/* =============================================================================
 *  joypad.c — P1 / JOYP (0xFF00)
 *
 *  The Game Boy matrixes eight buttons onto four input lines.  Everything on
 *  this port is ACTIVE LOW:
 *
 *      bit 7-6  unused, always read back as 1
 *      bit 5    P15 — 0 selects the ACTION button row   (A, B, Select, Start)
 *      bit 4    P14 — 0 selects the DIRECTION pad row   (Right, Left, Up, Down)
 *      bit 3-0  P13-P10, the four column lines: 0 = pressed
 *
 *  Selecting both rows at once pulls a column low if *either* row has that
 *  column pressed, which is equivalent to ANDing the two read-back nibbles.
 *  Selecting neither row leaves all four columns pulled high (0x0F).
 *
 *  A joypad interrupt is raised on a high-to-low transition of any column
 *  line, i.e. when a newly pressed button belongs to a currently selected row.
 *  Any new press also releases the CPU from STOP mode.
 * ========================================================================== */
#include "gb.h"

/* Bits 7-6 are open bus and read back as 1 on real hardware. */
#define P1_UNUSED  0xC0u
/* Only the two row-select bits are writable. */
#define P1_SELECT  0x30u
#define P1_SEL_DIR 0x10u   /* P14: 0 = direction row selected                  */
#define P1_SEL_ACT 0x20u   /* P15: 0 = action row selected                     */

/* --------------------------------------------------------------------------
 *  Column nibbles, active low (0 = pressed), derived from a BTN_* mask.
 *
 *  Directions occupy the low nibble of gb->joypad.buttons
 *      (BTN_RIGHT 0x01, BTN_LEFT 0x02, BTN_UP 0x04, BTN_DOWN 0x08)
 *  Actions occupy the high nibble
 *      (BTN_A 0x10, BTN_B 0x20, BTN_SELECT 0x40, BTN_START 0x80)
 * ------------------------------------------------------------------------ */
static u8 joypad_dir_nibble(u8 pressed)
{
    return (u8)(~(unsigned)pressed & 0x0Fu);
}

static u8 joypad_act_nibble(u8 pressed)
{
    return (u8)(~((unsigned)pressed >> 4) & 0x0Fu);
}

/* Resulting level of the four column lines for a given selection + state. */
static u8 joypad_lines(u8 sel, u8 pressed)
{
    u8 lines = 0x0Fu;

    if (!(sel & P1_SEL_DIR))
        lines = (u8)(lines & joypad_dir_nibble(pressed));
    if (!(sel & P1_SEL_ACT))
        lines = (u8)(lines & joypad_act_nibble(pressed));

    return (u8)(lines & 0x0Fu);
}

/* --------------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------------ */
void joypad_reset(GB *gb)
{
    if (!gb)
        return;

    gb->joypad.buttons = 0x00;      /* nothing held                           */
    gb->joypad.p1      = 0x30;      /* neither row selected                   */
}

void joypad_set_buttons(GB *gb, u8 mask)
{
    u8 old_lines, new_lines, newly_pressed;

    if (!gb)
        return;

    newly_pressed = (u8)(mask & (u8)~(unsigned)gb->joypad.buttons);

    old_lines = joypad_lines(gb->joypad.p1, gb->joypad.buttons);
    gb->joypad.buttons = mask;
    new_lines = joypad_lines(gb->joypad.p1, mask);

    /* STOP is broken by any button press, selected row or not. */
    if (newly_pressed)
        gb->cpu.stopped = false;

    /* A column line that was high and is now low is a falling edge => IRQ. */
    if ((u8)(old_lines & (u8)~(unsigned)new_lines) != 0u)
        gb_request_interrupt(gb, INT_JOYPAD);
}

u8 joypad_read(GB *gb)
{
    if (!gb)
        return 0xFF;

    return (u8)(P1_UNUSED
                | (gb->joypad.p1 & P1_SELECT)
                | joypad_lines(gb->joypad.p1, gb->joypad.buttons));
}

void joypad_write(GB *gb, u8 val)
{
    if (!gb)
        return;

    /* Only P14/P15 are writable; the column bits are inputs. */
    gb->joypad.p1 = (u8)(val & P1_SELECT);
}
