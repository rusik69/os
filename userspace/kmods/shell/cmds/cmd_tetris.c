/* cmd_tetris.c — terminal Tetris game */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"
#include "keyboard.h"

#define TET_W       10
#define TET_H       20
#define TET_EMPTY   0

/* Tetromino shapes (4 rotations each, stored as 4x4 grids) */
static const uint8_t tet_shapes[7][4][16] = {
    /* I */ { {0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0},
              {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
              {0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0},
              {0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0} },
    /* O */ { {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
              {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
              {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
              {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0} },
    /* T */ { {0,0,0,0,0,1,0,0,1,1,1,0,0,0,0,0},
              {0,0,0,0,0,1,0,0,0,1,1,0,0,1,0,0},
              {0,0,0,0,0,0,0,0,1,1,1,0,0,1,0,0},
              {0,0,0,0,0,1,0,0,1,1,0,0,0,1,0,0} },
    /* S */ { {0,0,0,0,0,1,1,0,1,1,0,0,0,0,0,0},
              {0,0,0,0,1,0,0,0,1,1,0,0,0,1,0,0},
              {0,0,0,0,0,0,0,0,0,1,1,0,1,1,0,0},
              {0,0,0,0,0,1,0,0,0,1,1,0,0,0,1,0} },
    /* Z */ { {0,0,0,0,1,1,0,0,0,1,1,0,0,0,0,0},
              {0,0,0,0,0,0,1,0,0,1,1,0,0,1,0,0},
              {0,0,0,0,0,0,0,0,0,1,1,0,0,1,1,0},
              {0,0,0,0,0,1,0,0,1,1,0,0,1,0,0,0} },
    /* L */ { {0,0,0,0,1,0,0,0,1,1,1,0,0,0,0,0},
              {0,0,0,0,0,0,1,0,0,0,1,0,0,1,1,0},
              {0,0,0,0,0,0,0,0,1,1,1,0,0,0,1,0},
              {0,0,0,0,0,1,1,0,0,1,0,0,0,1,0,0} },
    /* J */ { {0,0,0,0,0,0,1,0,1,1,1,0,0,0,0,0},
              {0,0,0,0,0,1,0,0,0,1,0,0,1,1,0,0},
              {0,0,0,0,0,0,0,0,1,1,1,0,1,0,0,0},
              {0,0,0,0,0,1,1,0,0,0,1,0,0,0,1,0} },
};

/* Game state */
static uint8_t tet_board[TET_H][TET_W];
static int tet_piece_type;
static int tet_rotation;
static int tet_px, tet_py;      /* piece position (top-left of 4x4) */
static int tet_score;
static int tet_lines;
static int tet_game_over;
static int tet_fall_counter;

/* Simple LCG PRNG */
static uint32_t tet_seed = 54321;
static int tet_rand(void) {
    tet_seed = tet_seed * 1103515245 + 12345;
    return (int)((tet_seed >> 16) & 0x7FFF);
}

static void tet_new_piece(void) {
    tet_piece_type = tet_rand() % 7;
    tet_rotation = 0;
    tet_px = (TET_W - 4) / 2;
    tet_py = 0;
    tet_fall_counter = 0;

    /* Check if new piece collides immediately (game over) */
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (tet_shapes[tet_piece_type][tet_rotation][r * 4 + c]) {
                int br = tet_py + r;
                int bc = tet_px + c;
                if (bc < 0 || bc >= TET_W || br >= TET_H ||
                    (br >= 0 && tet_board[br][bc]))
                    tet_game_over = 1;
            }
}

static void tet_init(void) {
    memset(tet_board, 0, sizeof(tet_board));
    tet_score = 0;
    tet_lines = 0;
    tet_game_over = 0;
    tet_new_piece();
}

static int tet_collides(int type, int rot, int px, int py) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (tet_shapes[type][rot][r * 4 + c]) {
                int br = py + r;
                int bc = px + c;
                if (bc < 0 || bc >= TET_W || br >= TET_H) return 1;
                if (br >= 0 && tet_board[br][bc]) return 1;
            }
    return 0;
}

static void tet_lock_piece(void) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (tet_shapes[tet_piece_type][tet_rotation][r * 4 + c]) {
                int br = tet_py + r;
                int bc = tet_px + c;
                if (br >= 0 && br < TET_H && bc >= 0 && bc < TET_W)
                    tet_board[br][bc] = (uint8_t)(tet_piece_type + 1);
            }
}

static void tet_clear_lines(void) {
    int cleared = 0;
    for (int r = TET_H - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < TET_W; c++)
            if (tet_board[r][c] == TET_EMPTY) { full = 0; break; }
        if (full) {
            /* Shift everything down */
            memmove(tet_board[1], tet_board[0], sizeof(uint8_t) * TET_W * r);
            memset(tet_board[0], 0, sizeof(uint8_t) * TET_W);
            cleared++;
            r++; /* re-check this row */
        }
    }
    if (cleared > 0) {
        tet_lines += cleared;
        tet_score += cleared * 100 * cleared; /* 100, 400, 900, 1600 */
    }
}

static void tet_tick(void) {
    if (tet_game_over) return;

    tet_fall_counter++;
    if (tet_fall_counter < 15) return;  /* fall speed */
    tet_fall_counter = 0;

    /* Move piece down */
    if (!tet_collides(tet_piece_type, tet_rotation, tet_px, tet_py + 1)) {
        tet_py++;
    } else {
        tet_lock_piece();
        tet_clear_lines();
        tet_new_piece();
    }
}

static void tet_rotate(void) {
    int new_rot = (tet_rotation + 1) % 4;
    if (!tet_collides(tet_piece_type, new_rot, tet_px, tet_py))
        tet_rotation = new_rot;
}

static void tet_move(int dx) {
    if (!tet_collides(tet_piece_type, tet_rotation, tet_px + dx, tet_py))
        tet_px += dx;
}

static void tet_drop(void) {
    while (!tet_collides(tet_piece_type, tet_rotation, tet_px, tet_py + 1))
        tet_py++;
    tet_lock_piece();
    tet_clear_lines();
    tet_new_piece();
}

static void tet_draw(void) {
    kprintf("\033[H\033[J");  /* home + clear */

    /* Build a display buffer with the current piece placed */
    uint8_t disp[TET_H][TET_W];
    memcpy(disp, tet_board, sizeof(tet_board));
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (tet_shapes[tet_piece_type][tet_rotation][r * 4 + c]) {
                int br = tet_py + r;
                int bc = tet_px + c;
                if (br >= 0 && br < TET_H && bc >= 0 && bc < TET_W)
                    disp[br][bc] = (uint8_t)(tet_piece_type + 8); /* offset to distinguish */
            }

    kprintf("\033[1;35m  Tetris   Score: %d   Lines: %d\033[0m\n\n", tet_score, tet_lines);

    /* Top border */
    kprintf("  \033[1;37m+");
    for (int c = 0; c < TET_W; c++) kprintf("-");
    kprintf("+\033[0m\n");

    /* Board */
    for (int r = 0; r < TET_H; r++) {
        kprintf("  \033[1;37m|\033[0m");
        for (int c = 0; c < TET_W; c++) {
            uint8_t v = disp[r][c];
            if (v == TET_EMPTY)
                kprintf(" ");
            else if (v >= 8) {
                /* Active piece — color by piece type */
                static const char *colors[] = {"\033[36;1m",  /* I: cyan */
                                               "\033[33;1m",  /* O: yellow */
                                               "\033[35;1m",  /* T: magenta */
                                               "\033[32;1m",  /* S: green */
                                               "\033[31;1m",  /* Z: red */
                                               "\033[34;1m",  /* L: blue */
                                               "\033[37;1m"}; /* J: white */
                kprintf("%s@\033[0m", colors[tet_piece_type]);
            } else {
                /* Locked piece */
                static const char *lcolors[] = {"\033[36m", "\033[33m", "\033[35m",
                                                "\033[32m", "\033[31m", "\033[34m", "\033[37m"};
                kprintf("%s#\033[0m", lcolors[(v - 1) % 7]);
            }
        }
        kprintf("\033[1;37m|\033[0m\n");
    }

    /* Bottom border */
    kprintf("  \033[1;37m+");
    for (int c = 0; c < TET_W; c++) kprintf("-");
    kprintf("+\033[0m\n");

    if (tet_game_over)
        kprintf("\n\033[31;1;5m*** GAME OVER (score: %d) ***\033[0m\n", tet_score);
    else
        kprintf("\n\033[33m<- -> move   Up: rotate   Space: drop   Q: quit\033[0m\n");
}

void cmd_tetris(const char *args) {
    (void)args;
    tet_init();

    while (!tet_game_over) {
        /* Process all buffered input */
        while (keyboard_has_input()) {
            char ch = keyboard_getchar();
            if (ch == 'q' || ch == 'Q') { kprintf("Quit.\n"); return; }
            if (ch == KEY_LEFT)  tet_move(-1);
            if (ch == KEY_RIGHT) tet_move(1);
            if (ch == KEY_UP)    tet_rotate();
            if (ch == ' ')       tet_drop();
        }

        tet_tick();
        tet_draw();
        libc_sleep_ticks(5); /* ~50ms per frame */
    }

    tet_draw();
    kprintf("Press any key...\n");
    keyboard_getchar();
}
