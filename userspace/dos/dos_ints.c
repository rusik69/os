/*
 * dos_ints.c - DOS interrupt dispatch and handlers
 *
 * This file implements dos_handle_int() which dispatches hardware/software
 * interrupts in the emulated 16-bit real-mode environment.
 *
 * Handled interrupts:
 *   INT 00h  -- divide error (stub: stops execution)
 *   INT 10h  -- video services (AH=0x0E: teletype output)
 *   INT 16h  -- keyboard services (AH=0x00: read key, AH=0x01: check key)
 *   INT 20h  -- exit program
 *   INT 21h  -- DOS services; forwarded to dos_handle_int21() (dos_int21.c)
 *
 * External declarations:
 *   dos_handle_int21(struct dos_cpu_state *)  -- defined in dos_int21.c
 */

#include "dos.h"

/* dos_handle_int21 is defined in dos_int21.c */
extern void dos_handle_int21(struct dos_cpu_state *state);

/*
 * dos_handle_int -- main interrupt dispatcher
 *
 * Called from the emulator loop when it encounters an INT imm8 instruction.
 * state->ax, state->bx, etc. hold the register state at the time of the INT.
 */
void dos_handle_int(struct dos_cpu_state *state, uint8_t int_num)
{
    switch (int_num) {

    /* INT 00h -- divide error */
    case 0x00:
        state->running = 0;
        break;

    /* INT 10h -- video services */
    case 0x10: {
        uint8_t ah = (uint8_t)(state->ax >> 8);
        switch (ah) {
        case 0x0E: /* Teletype output */
            kprintf("%c", (unsigned char)(state->ax & 0xFF));
            break;
        default:
            break;
        }
        break;
    }

    /* INT 16h -- keyboard services */
    case 0x16: {
        uint8_t ah = (uint8_t)(state->ax >> 8);
        switch (ah) {
        case 0x00: { /* Read key */
            char c = keyboard_getchar();
            state->ax = (uint16_t)(unsigned char)c;
            break;
        }
        case 0x01: { /* Check key */
            if (keyboard_has_input()) {
                state->flags &= ~DOS_FLAG_ZF; /* ZF = 0 => key available */
            } else {
                state->flags |= DOS_FLAG_ZF;  /* ZF = 1 => no key */
            }
            break;
        }
        default:
            break;
        }
        break;
    }

    /* INT 20h -- exit program */
    case 0x20:
        state->running = 0;
        break;

    /* INT 21h -- DOS services */
    case 0x21:
        dos_handle_int21(state);
        break;

    default:
        break;
    }
}
