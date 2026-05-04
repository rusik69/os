/* cmd_beep.c — beep command */
#include "shell_cmds.h"
#include "printf.h"
#include "speaker.h"

void cmd_beep(const char *args) {
    uint32_t freq = 1000;
    uint32_t ms   = 200;
    if (args && *args >= '0' && *args <= '9') {
        freq = 0;
        while (*args >= '0' && *args <= '9') { freq = freq * 10 + (*args - '0'); args++; }
        while (*args == ' ') args++;
        if (*args >= '0' && *args <= '9') {
            ms = 0;
            while (*args >= '0' && *args <= '9') { ms = ms * 10 + (*args - '0'); args++; }
        }
    }
    speaker_beep(freq, ms);
}
