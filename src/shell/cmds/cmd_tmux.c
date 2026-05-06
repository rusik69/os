/* tmux.c — Terminal multiplexer (tmux-like)
 *
 * Provides multiple virtual terminal panes within the 80x25 VGA screen.
 * Prefix key: Ctrl-B, then:
 *   c   = create new window
 *   n   = next window
 *   p   = previous window
 *   0-3 = switch to window N
 *   x   = close current window
 *   d   = detach (exit tmux)
 *   "   = horizontal split
 *   o   = switch pane in split
 *   ?   = show help
 *
 * Status bar at row 24 shows: [0:sh] [1:sh]* ...
 */

#include "libc.h"
#include "printf.h"
#include "string.h"
#include "shell_cmds.h"

#define TMUX_MAX_PANES  4
#define TMUX_COLS       80
#define TMUX_ROWS       24   /* 25 - 1 for status bar */
#define TMUX_PREFIX     0x02 /* Ctrl-B */
#define HISTORY_SIZE    16

/* Per-pane screen buffer */
typedef struct {
    uint16_t screen[TMUX_ROWS][TMUX_COLS]; /* VGA character + attribute */
    int cur_row;
    int cur_col;
    uint8_t color;
    int active;           /* 1 = in use */
    char title[16];       /* window title */
    /* Command input state */
    char cmd_buf[256];
    int cmd_len;
    int hist_pos;
    /* Split info: if split, this pane shows rows [view_top..view_bot) */
    int view_top;
    int view_bot;
} Pane;

static Pane panes[TMUX_MAX_PANES];
static int active_pane = 0;
static int num_panes = 0;
static int tmux_running = 0;
static int prefix_mode = 0;
static int split_mode = 0;      /* 0=no split, 1=horizontal split (2 panes visible) */
static int split_pane_a = 0;    /* top pane index */
static int split_pane_b = -1;   /* bottom pane index */
static int split_focus = 0;     /* 0=top(a), 1=bottom(b) */

/* ------------------------------------------------------------------ */
/*  Status bar                                                         */
/* ------------------------------------------------------------------ */

static void draw_status_bar(void) {
    uint8_t scolor = VGA_BLACK | (VGA_GREEN << 4);
    /* Fill row 24 with status bar */
    for (int c = 0; c < TMUX_COLS; c++)
        vga_put_entry_at(' ', scolor, 24, c);

    int col = 1;
    for (int i = 0; i < TMUX_MAX_PANES && col < 75; i++) {
        if (!panes[i].active) continue;
        char buf[20];
        int len = 0;
        buf[len++] = '[';
        buf[len++] = '0' + i;
        buf[len++] = ':';
        const char *t = panes[i].title;
        while (*t && len < 14) buf[len++] = *t++;
        buf[len++] = ']';
        if (i == active_pane) buf[len++] = '*';
        buf[len] = '\0';
        for (int j = 0; j < len && col < 78; j++)
            vga_put_entry_at(buf[j], scolor, 24, col++);
        vga_put_entry_at(' ', scolor, 24, col++);
    }

    /* Right side: show prefix indicator */
    if (prefix_mode) {
        const char *pm = "^B-";
        int rc = 74;
        for (int j = 0; pm[j]; j++)
            vga_put_entry_at(pm[j], VGA_WHITE | (VGA_RED << 4), 24, rc++);
    }
}

/* ------------------------------------------------------------------ */
/*  Pane screen buffer management                                     */
/* ------------------------------------------------------------------ */

static void pane_clear(Pane *p) {
    p->color = VGA_LIGHT_GREY | (VGA_BLACK << 4);
    for (int r = 0; r < TMUX_ROWS; r++)
        for (int c = 0; c < TMUX_COLS; c++)
            p->screen[r][c] = (uint16_t)' ' | ((uint16_t)p->color << 8);
    p->cur_row = 0;
    p->cur_col = 0;
}

static void pane_scroll(Pane *p) {
    if (p->cur_row < TMUX_ROWS) return;
    for (int r = 0; r < TMUX_ROWS - 1; r++)
        for (int c = 0; c < TMUX_COLS; c++)
            p->screen[r][c] = p->screen[r + 1][c];
    for (int c = 0; c < TMUX_COLS; c++)
        p->screen[TMUX_ROWS - 1][c] = (uint16_t)' ' | ((uint16_t)p->color << 8);
    p->cur_row = TMUX_ROWS - 1;
}

static void pane_putchar(Pane *p, char ch) {
    if (ch == '\n') {
        p->cur_col = 0;
        p->cur_row++;
    } else if (ch == '\r') {
        p->cur_col = 0;
    } else if (ch == '\t') {
        p->cur_col = (p->cur_col + 8) & ~7;
        if (p->cur_col >= TMUX_COLS) { p->cur_col = 0; p->cur_row++; }
    } else if (ch == '\b') {
        if (p->cur_col > 0) {
            p->cur_col--;
            p->screen[p->cur_row][p->cur_col] = (uint16_t)' ' | ((uint16_t)p->color << 8);
        }
    } else {
        if (p->cur_row >= TMUX_ROWS) pane_scroll(p);
        p->screen[p->cur_row][p->cur_col] = (uint16_t)(uint8_t)ch | ((uint16_t)p->color << 8);
        p->cur_col++;
        if (p->cur_col >= TMUX_COLS) { p->cur_col = 0; p->cur_row++; }
    }
    pane_scroll(p);
}

static void pane_write(Pane *p, const char *s) {
    while (*s) pane_putchar(p, *s++);
}

/* ------------------------------------------------------------------ */
/*  Display a pane to VGA (optionally to a sub-region)                */
/* ------------------------------------------------------------------ */

static void display_pane_region(Pane *p, int scr_top, int scr_bot) {
    int rows = scr_bot - scr_top;
    /* Show last 'rows' lines from pane buffer */
    int start_row = p->cur_row - rows + 1;
    if (start_row < 0) start_row = 0;
    for (int r = 0; r < rows; r++) {
        int pr = start_row + r;
        for (int c = 0; c < TMUX_COLS; c++) {
            uint16_t cell = (pr >= 0 && pr < TMUX_ROWS)
                                ? p->screen[pr][c]
                                : ((uint16_t)' ' | ((uint16_t)(VGA_LIGHT_GREY | (VGA_BLACK << 4)) << 8));
            char ch = (char)(cell & 0xFF);
            uint8_t color = (uint8_t)((cell >> 8) & 0xFF);
            vga_put_entry_at(ch, color, (uint16_t)(scr_top + r), (uint16_t)c);
        }
    }
}

static void display_full(void) {
    Pane *p = &panes[active_pane];
    if (!split_mode) {
        display_pane_region(p, 0, TMUX_ROWS);
        /* Set hardware cursor */
        int crow = p->cur_row;
        int ccol = p->cur_col;
        if (crow >= TMUX_ROWS) crow = TMUX_ROWS - 1;
        vga_set_cursor((uint16_t)crow, (uint16_t)ccol);
    } else {
        /* Split: top half = pane_a, bottom half = pane_b */
        int mid = TMUX_ROWS / 2;
        display_pane_region(&panes[split_pane_a], 0, mid);
        /* Separator line */
        uint8_t sep_color = VGA_WHITE | (VGA_BLUE << 4);
        for (int c = 0; c < TMUX_COLS; c++)
            vga_put_entry_at('-', sep_color, mid, c);
        if (split_pane_b >= 0)
            display_pane_region(&panes[split_pane_b], mid + 1, TMUX_ROWS);
        /* Cursor in focused pane */
        Pane *fp = (split_focus == 0) ? &panes[split_pane_a] : &panes[split_pane_b];
        int offset = (split_focus == 0) ? 0 : mid + 1;
        int crow = fp->cur_row;
        if (crow >= (TMUX_ROWS / 2)) crow = (TMUX_ROWS / 2) - 1;
        vga_set_cursor((uint16_t)(offset + crow), (uint16_t)fp->cur_col);
    }
    draw_status_bar();
}

/* ------------------------------------------------------------------ */
/*  Pane management                                                    */
/* ------------------------------------------------------------------ */

static int create_pane(void) {
    for (int i = 0; i < TMUX_MAX_PANES; i++) {
        if (!panes[i].active) {
            panes[i].active = 1;
            pane_clear(&panes[i]);
            panes[i].cmd_len = 0;
            panes[i].hist_pos = 0;
            strncpy(panes[i].title, "sh", 15);
            panes[i].view_top = 0;
            panes[i].view_bot = TMUX_ROWS;
            num_panes++;
            return i;
        }
    }
    return -1;
}

static void close_pane(int idx) {
    if (idx < 0 || idx >= TMUX_MAX_PANES) return;
    if (!panes[idx].active) return;
    panes[idx].active = 0;
    num_panes--;
    if (split_pane_b == idx) { split_mode = 0; split_pane_b = -1; }
    if (num_panes <= 0) { tmux_running = 0; return; }
    /* Switch to another pane */
    if (active_pane == idx) {
        for (int i = 0; i < TMUX_MAX_PANES; i++)
            if (panes[i].active) { active_pane = i; break; }
    }
}

/* ------------------------------------------------------------------ */
/*  kprintf hook — routes output to active pane buffer                */
/* ------------------------------------------------------------------ */

static void tmux_output_hook(char c, void *ctx) {
    (void)ctx;
    Pane *p = &panes[active_pane];
    pane_putchar(p, c);
}

/* ------------------------------------------------------------------ */
/*  Prompt for a pane                                                  */
/* ------------------------------------------------------------------ */

static void pane_prompt(Pane *p) {
    p->color = VGA_LIGHT_GREEN | (VGA_BLACK << 4);
    pane_write(p, "os>");
    p->color = VGA_LIGHT_GREY | (VGA_BLACK << 4);
    pane_putchar(p, ' ');
}

/* ------------------------------------------------------------------ */
/*  Execute command in a pane                                          */
/* ------------------------------------------------------------------ */

static void pane_exec(int pane_idx) {
    Pane *p = &panes[pane_idx];
    p->cmd_buf[p->cmd_len] = '\0';

    /* Skip empty */
    char *cmd = p->cmd_buf;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') { pane_prompt(p); return; }

    /* Add to shell history */
    shell_history_add(p->cmd_buf);
    p->hist_pos = shell_history_count();

    /* Exit tmux from within */
    if (strcmp(cmd, "exit") == 0) {
        close_pane(pane_idx);
        return;
    }

    /* Parse cmd + args */
    char *args = cmd;
    while (*args && *args != ' ') args++;
    if (*args) { *args = '\0'; args++; while (*args == ' ') args++; }
    else args = 0;

    /* Set kprintf hook to capture into this pane */
    int saved_pane = active_pane;
    active_pane = pane_idx;
    kprintf_set_hook(tmux_output_hook, 0);
    libc_shell_exec_cmd(cmd, args);
    kprintf_set_hook(0, 0);
    active_pane = saved_pane;

    pane_putchar(p, '\n');
    pane_prompt(p);
}

/* ------------------------------------------------------------------ */
/*  Handle prefix commands                                             */
/* ------------------------------------------------------------------ */

static void handle_prefix_cmd(char c) {
    switch (c) {
    case 'c': { /* create window */
        int idx = create_pane();
        if (idx >= 0) {
            active_pane = idx;
            pane_prompt(&panes[idx]);
        }
        break;
    }
    case 'n': { /* next window */
        int start = active_pane;
        for (int i = 1; i <= TMUX_MAX_PANES; i++) {
            int ni = (start + i) % TMUX_MAX_PANES;
            if (panes[ni].active) { active_pane = ni; break; }
        }
        break;
    }
    case 'p': { /* previous window */
        int start = active_pane;
        for (int i = 1; i <= TMUX_MAX_PANES; i++) {
            int ni = (start - i + TMUX_MAX_PANES) % TMUX_MAX_PANES;
            if (panes[ni].active) { active_pane = ni; break; }
        }
        break;
    }
    case '0': case '1': case '2': case '3': {
        int idx = c - '0';
        if (idx < TMUX_MAX_PANES && panes[idx].active)
            active_pane = idx;
        break;
    }
    case 'x': /* close window */
        close_pane(active_pane);
        break;
    case 'd': /* detach */
        tmux_running = 0;
        break;
    case '"': { /* horizontal split */
        if (split_mode) { split_mode = 0; break; } /* toggle off */
        /* Find another pane, or create one */
        int other = -1;
        for (int i = 0; i < TMUX_MAX_PANES; i++) {
            if (i != active_pane && panes[i].active) { other = i; break; }
        }
        if (other < 0) other = create_pane();
        if (other >= 0) {
            split_mode = 1;
            split_pane_a = active_pane;
            split_pane_b = other;
            split_focus = 0;
            if (panes[other].cmd_len == 0 && panes[other].cur_row == 0)
                pane_prompt(&panes[other]);
        }
        break;
    }
    case 'o': /* switch focus in split */
        if (split_mode) {
            split_focus = 1 - split_focus;
            active_pane = (split_focus == 0) ? split_pane_a : split_pane_b;
        }
        break;
    case '?': { /* help */
        Pane *p = &panes[active_pane];
        pane_putchar(p, '\n');
        pane_write(p, "tmux keys (prefix Ctrl-B):\n");
        pane_write(p, "  c   new window\n");
        pane_write(p, "  n/p next/prev window\n");
        pane_write(p, "  0-3 switch to window\n");
        pane_write(p, "  \"   split horizontal\n");
        pane_write(p, "  o   switch split pane\n");
        pane_write(p, "  x   close window\n");
        pane_write(p, "  d   detach (exit tmux)\n");
        pane_write(p, "  ?   this help\n");
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Main tmux event loop                                               */
/* ------------------------------------------------------------------ */

void tmux_run(void) {
    /* Initialize first pane */
    memset(panes, 0, sizeof(panes));
    num_panes = 0;
    split_mode = 0;
    split_pane_b = -1;
    tmux_running = 1;
    prefix_mode = 0;

    int idx = create_pane();
    active_pane = idx;
    pane_prompt(&panes[idx]);

    /* Clear screen and draw */
    vga_clear();
    display_full();

    while (tmux_running) {
        char c = keyboard_getchar();
        Pane *p = &panes[active_pane];

        /* Prefix key handling */
        if (prefix_mode) {
            prefix_mode = 0;
            handle_prefix_cmd(c);
            if (!tmux_running) break;
            display_full();
            continue;
        }

        if (c == TMUX_PREFIX) {
            prefix_mode = 1;
            draw_status_bar(); /* show ^B indicator */
            continue;
        }

        /* Normal input to active pane */
        if (c == '\n') {
            pane_putchar(p, '\n');
            pane_exec(active_pane);
            if (!tmux_running) break;
        } else if (c == KEY_UP) {
            int cnt = shell_history_count();
            if (p->hist_pos > 0 && p->hist_pos > cnt - HISTORY_SIZE) {
                p->hist_pos--;
                /* Erase current input */
                for (int i = 0; i < p->cmd_len; i++) pane_putchar(p, '\b');
                const char *entry = shell_history_entry(p->hist_pos);
                if (entry) {
                    strncpy(p->cmd_buf, entry, 255);
                    p->cmd_len = strlen(p->cmd_buf);
                    pane_write(p, p->cmd_buf);
                }
            }
        } else if (c == KEY_DOWN) {
            for (int i = 0; i < p->cmd_len; i++) pane_putchar(p, '\b');
            int cnt = shell_history_count();
            if (p->hist_pos < cnt - 1) {
                p->hist_pos++;
                const char *entry = shell_history_entry(p->hist_pos);
                if (entry) {
                    strncpy(p->cmd_buf, entry, 255);
                    p->cmd_len = strlen(p->cmd_buf);
                    pane_write(p, p->cmd_buf);
                }
            } else {
                p->hist_pos = cnt;
                p->cmd_buf[0] = '\0';
                p->cmd_len = 0;
            }
        } else if (c == '\b') {
            if (p->cmd_len > 0) {
                p->cmd_len--;
                pane_putchar(p, '\b');
            }
        } else if (c == '\t') {
            /* Tab completion within pane */
            p->cmd_buf[p->cmd_len] = '\0';
            /* Simple: just complete command names */
            /* (Reuse shell logic via kprintf hook) */
            kprintf_set_hook(tmux_output_hook, 0);
            /* We reuse the telnet version which uses kprintf */
            int old_active = active_pane;
            shell_tab_complete_telnet(p->cmd_buf, &p->cmd_len, 0);
            active_pane = old_active;
            kprintf_set_hook(0, 0);
        } else if ((unsigned char)c >= 32 && (unsigned char)c < 128) {
            if (p->cmd_len < 255) {
                p->cmd_buf[p->cmd_len++] = c;
                pane_putchar(p, c);
            }
        }

        display_full();
    }

    /* Restore normal shell display */
    vga_clear();
    kprintf("tmux: detached\n");
}

/* ------------------------------------------------------------------ */
/*  Shell command entry point                                          */
/* ------------------------------------------------------------------ */

void cmd_tmux(const char *args) {
    if (args && strcmp(args, "--help") == 0) {
        kprintf("Usage: tmux\n");
        kprintf("  Terminal multiplexer with multiple windows.\n");
        kprintf("  Prefix key: Ctrl-B\n");
        kprintf("  Ctrl-B c   - new window\n");
        kprintf("  Ctrl-B n/p - next/prev window\n");
        kprintf("  Ctrl-B 0-3 - switch to window\n");
        kprintf("  Ctrl-B \"   - horizontal split\n");
        kprintf("  Ctrl-B o   - switch split focus\n");
        kprintf("  Ctrl-B x   - close window\n");
        kprintf("  Ctrl-B d   - detach (exit)\n");
        return;
    }
    tmux_run();
}
