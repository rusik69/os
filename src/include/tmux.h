#ifndef TMUX_H
#define TMUX_H

#include "types.h"

#define TMUX_MAX_PANES  4
#define TMUX_COLS       80
#define TMUX_ROWS       24   /* 25 - 1 for status bar */
#define TMUX_PREFIX     0x02 /* Ctrl-B */

void cmd_tmux(const char *args);
void tmux_run(void);

#endif
