/* cmd_minesweeper.c — terminal minesweeper game */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "keyboard.h"

#define MS_ROWS    9
#define MS_COLS    9
#define MS_MINES   10
#define MS_HIDDEN   0
#define MS_REVEALED 1
#define MS_FLAGGED  2

static int8_t ms_grid[MS_ROWS][MS_COLS];      /* -1=mine, 0-8=count */
static uint8_t ms_state[MS_ROWS][MS_COLS];     /* HIDDEN/REVEALED/FLAGGED */
static int ms_game_over;
static int ms_won;

/* Simple LCG PRNG */
static uint32_t ms_seed = 12345;
static int ms_rand(void) {
    ms_seed = ms_seed * 1103515245 + 12345;
    return (int)((ms_seed >> 16) & 0x7FFF);
}

/* Place mines avoiding first-click cell and its neighbours */
static void ms_place_mines(int avoid_r, int avoid_c) {
    int placed = 0;
    while (placed < MS_MINES) {
        int r = ms_rand() % MS_ROWS;
        int c = ms_rand() % MS_COLS;
        if (ms_grid[r][c] == -1) continue;
        if (r >= avoid_r - 1 && r <= avoid_r + 1 &&
            c >= avoid_c - 1 && c <= avoid_c + 1) continue;
        ms_grid[r][c] = -1;
        placed++;
    }
}

static void ms_fill_numbers(void) {
    for (int r = 0; r < MS_ROWS; r++) {
        for (int c = 0; c < MS_COLS; c++) {
            if (ms_grid[r][c] == -1) continue;
            int cnt = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    int nr = r + dr, nc = c + dc;
                    if (nr >= 0 && nr < MS_ROWS && nc >= 0 && nc < MS_COLS &&
                        ms_grid[nr][nc] == -1) cnt++;
                }
            ms_grid[r][c] = (int8_t)cnt;
        }
    }
}

static void ms_init(int avoid_r, int avoid_c) {
    memset(ms_grid, 0, sizeof(ms_grid));
    memset(ms_state, 0, sizeof(ms_state));
    ms_game_over = 0;
    ms_won = 0;
    ms_place_mines(avoid_r, avoid_c);
    ms_fill_numbers();
}

static void ms_reveal(int r, int c) {
    if (r < 0 || r >= MS_ROWS || c < 0 || c >= MS_COLS) return;
    if (ms_state[r][c] != MS_HIDDEN) return;
    ms_state[r][c] = MS_REVEALED;
    if (ms_grid[r][c] == -1) { ms_game_over = 1; return; }
    if (ms_grid[r][c] == 0) {
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++)
                if (dr || dc) ms_reveal(r + dr, c + dc);
    }
}

static int ms_check_win(void) {
    for (int r = 0; r < MS_ROWS; r++)
        for (int c = 0; c < MS_COLS; c++)
            if (ms_grid[r][c] != -1 && ms_state[r][c] != MS_REVEALED)
                return 0;
    return 1;
}

static void ms_draw(void) {
    kprintf("\033[H\033[J"); /* clear + home */

    kprintf("\033[1;36m  Minesweeper  %dx%d  %d mines\033[0m\n",
            MS_ROWS, MS_COLS, MS_MINES);

    /* Column header */
    kprintf("   ");
    for (int c = 0; c < MS_COLS; c++)
        kprintf(" \033[1;30m%d\033[0m", (c + 1) % 10);
    kprintf("\n");

    kprintf("  \033[1;30m+");
    for (int c = 0; c < MS_COLS; c++) kprintf("--");
    kprintf("\033[0m\n");

    for (int r = 0; r < MS_ROWS; r++) {
        kprintf("\033[1;30m%d\033[0m\033[1;30m|\033[0m", (r + 1) % 10);
        for (int c = 0; c < MS_COLS; c++) {
            uint8_t st = ms_state[r][c];
            if (st == MS_HIDDEN)
                kprintf("\033[47;30m%c\033[0m", 0xB1); /* ░ shaded block */
            else if (st == MS_FLAGGED)
                kprintf("\033[41;1;37m F\033[0m");
            else if (ms_grid[r][c] == -1)
                kprintf("\033[31;1m *\033[0m");
            else {
                int v = ms_grid[r][c];
                if (v == 0) kprintf("  ");
                else kprintf("\033[%dm %d\033[0m", 30 + v, v);
            }
        }
        kprintf("\n");
    }

    if (ms_game_over)
        kprintf("\n\033[31;1m*** GAME OVER ***\033[0m\n");
    else if (ms_won)
        kprintf("\n\033[32;1m*** YOU WIN! ***\033[0m\n");
    else
        kprintf("\n\033[33m[r3c5] reveal  [f3c5] flag  [q] quit\033[0m\n> ");
}

/* Read a single line of keyboard input (no echo buffer) */
static int ms_readline(char *buf, int max) {
    int pos = 0;
    buf[0] = '\0';
    while (pos < max - 1) {
        char ch = keyboard_getchar();
        if (ch == '\n' || ch == '\r') break;
        if (ch == '\b' || ch == 127) {
            if (pos > 0) { pos--; kprintf("\b \b"); }
            continue;
        }
        buf[pos++] = ch;
        kprintf("%c", ch);
    }
    buf[pos] = '\0';
    kprintf("\n");
    return pos;
}

void cmd_minesweeper(const char *args) {
    (void)args;

    ms_init(0, 0); /* safe first click at 0,0 */
    int first_click = 1;

    while (!ms_game_over && !ms_won) {
        ms_draw();

        char buf[16];
        if (ms_readline(buf, sizeof(buf)) <= 0) continue;

        if (buf[0] == 'q' || buf[0] == 'Q') {
            kprintf("Quit.\n");
            return;
        }

        int r = -1, c = -1;
        int is_flag = (buf[0] == 'f' || buf[0] == 'F');
        if ((buf[0] == 'r' || buf[0] == 'R') || is_flag) {
            for (int i = 1; buf[i]; i++) {
                if (buf[i] >= '1' && buf[i] <= '9') {
                    if (r < 0) r = buf[i] - '1';
                    else c = c * 10 + (buf[i] - '1');
                }
            }
        }

        if (r < 0 || r >= MS_ROWS || c < 0 || c >= MS_COLS) {
            kprintf("Bad input. Try r3c5 or f3c5.\n");
            continue;
        }

        if (is_flag) {
            if (ms_state[r][c] == MS_HIDDEN)
                ms_state[r][c] = MS_FLAGGED;
            else if (ms_state[r][c] == MS_FLAGGED)
                ms_state[r][c] = MS_HIDDEN;
            continue;
        }

        if (ms_state[r][c] != MS_HIDDEN) continue;

        if (first_click && ms_grid[r][c] == -1) {
            ms_init(r, c); /* re-seed avoiding first click */
        }
        first_click = 0;

        ms_reveal(r, c);
        if (!ms_game_over) ms_won = ms_check_win();
    }

    ms_draw();
    kprintf("Press any key...\n");
    keyboard_getchar();
}
