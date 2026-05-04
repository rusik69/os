/* cmd_play.c — play command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "speaker.h"
#include "timer.h"

static uint32_t note_freq(const char *note) {
    if (strcmp(note, "C4") == 0) return NOTE_C4;
    if (strcmp(note, "D4") == 0) return NOTE_D4;
    if (strcmp(note, "E4") == 0) return NOTE_E4;
    if (strcmp(note, "F4") == 0) return NOTE_F4;
    if (strcmp(note, "G4") == 0) return NOTE_G4;
    if (strcmp(note, "A4") == 0) return NOTE_A4;
    if (strcmp(note, "B4") == 0) return NOTE_B4;
    if (strcmp(note, "C5") == 0) return NOTE_C5;
    return 440;
}

void cmd_play(const char *args) {
    if (!args || !*args) { kprintf("Usage: play <note> [note ...]\n"); return; }
    char note[8];
    while (*args) {
        while (*args == ' ') args++;
        if (!*args) break;
        int ni = 0;
        while (*args && *args != ' ' && ni < 7) note[ni++] = *args++;
        note[ni] = '\0';
        speaker_beep(note_freq(note), 200);
        uint64_t start = timer_get_ticks();
        while (timer_get_ticks() - start < (uint64_t)TIMER_FREQ / 20);
    }
}
