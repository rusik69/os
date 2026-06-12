/* cmd_snake.c — terminal snake game */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

/* IMPORTANT: libc.h must come before keyboard.h because libc.h provides
 * static inline keyboard_getchar() and keyboard.h has the extern declaration.
 * The static inline must be seen first to avoid a C17 conflict. */
#include "libc.h"
#include "keyboard.h"

#define SNK_W       40
#define SNK_H       20
#define SNK_MAX_LEN (SNK_W * SNK_H)

/* Direction constants */
#define SNK_RIGHT   0
#define SNK_UP      1
#define SNK_LEFT    2
#define SNK_DOWN    3

static int snk_body[SNK_MAX_LEN][2]; /* [segment][0]=y, [1]=x */
static int snk_len;
static int snk_dir;
static int snk_next_dir;
static int snk_food_y, snk_food_x;
static int snk_score;
static int snk_game_over;
static int snk_won;

/* Simple LCG PRNG */
static uint32_t snk_seed = 67890;
static int snk_rand(void) {
    snk_seed = snk_seed * 1103515245 + 12345;
    return (int)((snk_seed >> 16) & 0x7FFF);
}

/* Spawn food at a random unoccupied cell */
static void snk_spawn_food(void) {
    int occupied;
    do {
        occupied = 0;
        snk_food_y = snk_rand() % SNK_H;
        snk_food_x = snk_rand() % SNK_W;
        for (int i = 0; i < snk_len; i++) {
            if (snk_body[i][0] == snk_food_y && snk_body[i][1] == snk_food_x) {
                occupied = 1;
                break;
            }
        }
    } while (occupied);
}

static void snk_init(void) {
    /* Start with a length-3 snake in the middle, facing right */
    int sy = SNK_H / 2;
    int sx = SNK_W / 2;
    snk_len = 3;
    snk_body[0][0] = sy;     snk_body[0][1] = sx;       /* head */
    snk_body[1][0] = sy;     snk_body[1][1] = sx - 1;   /* body */
    snk_body[2][0] = sy;     snk_body[2][1] = sx - 2;   /* tail */
    snk_dir = SNK_RIGHT;
    snk_next_dir = SNK_RIGHT;
    snk_score = 0;
    snk_game_over = 0;
    snk_won = 0;
    snk_spawn_food();
}

static void snk_read_input(void) {
    while (keyboard_has_input()) {
        char ch = keyboard_getchar();
        int new_dir = snk_dir;
        if (ch == KEY_UP    || ch == 'w' || ch == 'W') new_dir = SNK_UP;
        if (ch == KEY_DOWN  || ch == 's' || ch == 'S') new_dir = SNK_DOWN;
        if (ch == KEY_LEFT  || ch == 'a' || ch == 'A') new_dir = SNK_LEFT;
        if (ch == KEY_RIGHT || ch == 'd' || ch == 'D') new_dir = SNK_RIGHT;
        if (ch == 'q' || ch == 'Q') { snk_game_over = 1; return; }

        /* Prevent 180-degree reversal */
        if ((new_dir == SNK_UP    && snk_dir != SNK_DOWN) ||
            (new_dir == SNK_DOWN  && snk_dir != SNK_UP)    ||
            (new_dir == SNK_LEFT  && snk_dir != SNK_RIGHT) ||
            (new_dir == SNK_RIGHT && snk_dir != SNK_LEFT)) {
            snk_next_dir = new_dir;
        }
    }
}

static int snk_tick(void) {
    if (snk_game_over) return 0;

    snk_dir = snk_next_dir;

    /* Compute new head position */
    int new_y = snk_body[0][0];
    int new_x = snk_body[0][1];
    switch (snk_dir) {
        case SNK_UP:    new_y--; break;
        case SNK_DOWN:  new_y++; break;
        case SNK_LEFT:  new_x--; break;
        case SNK_RIGHT: new_x++; break;
    }

    /* Wall collision */
    if (new_y < 0 || new_y >= SNK_H || new_x < 0 || new_x >= SNK_W) {
        snk_game_over = 1;
        return 0;
    }

    /* Self collision (check all but last segment — it moves away) */
    for (int i = 0; i < snk_len - 1; i++) {
        if (snk_body[i][0] == new_y && snk_body[i][1] == new_x) {
            snk_game_over = 1;
            return 0;
        }
    }

    /* Shift body: each segment takes the position of the one in front */
    for (int i = snk_len - 1; i > 0; i--) {
        snk_body[i][0] = snk_body[i - 1][0];
        snk_body[i][1] = snk_body[i - 1][1];
    }
    snk_body[0][0] = new_y;
    snk_body[0][1] = new_x;

    /* Check food */
    if (new_y == snk_food_y && new_x == snk_food_x) {
        snk_len++;
        snk_score += 10;
        if (snk_len >= SNK_MAX_LEN) {
            snk_won = 1;
            return 0;
        }
        snk_spawn_food();
    }

    return 1;
}

static void snk_draw(void) {
    kprintf("\033[H\033[J");  /* home + clear */

    /* Title */
    kprintf("\033[1;33m  Snake  Score: %d\033[0m\n", snk_score);

    /* Top wall */
    kprintf("\033[1;32m+");
    for (int x = 0; x < SNK_W; x++) kprintf("-");
    kprintf("+\033[0m\n");

    /* Game area */
    for (int y = 0; y < SNK_H; y++) {
        kprintf("\033[1;32m|\033[0m");  /* left wall */
        for (int x = 0; x < SNK_W; x++) {
            if (snk_game_over) {
                int is_body = 0;
                for (int i = 0; i < snk_len; i++) {
                    if (snk_body[i][0] == y && snk_body[i][1] == x) {
                        is_body = 1;
                        if (i == 0) kprintf("\033[41;1;37m@\033[0m");
                        else kprintf("\033[31mO\033[0m");
                        break;
                    }
                }
                if (!is_body) {
                    if (y == snk_food_y && x == snk_food_x)
                        kprintf("\033[33m*\033[0m");
                    else
                        kprintf(" ");
                }
            } else if (y == snk_food_y && x == snk_food_x) {
                kprintf("\033[33m*\033[0m");
            } else {
                int printed = 0;
                for (int i = 0; i < snk_len; i++) {
                    if (snk_body[i][0] == y && snk_body[i][1] == x) {
                        if (i == 0)
                            kprintf("\033[1;32m@\033[0m");
                        else
                            kprintf("\033[32mO\033[0m");
                        printed = 1;
                        break;
                    }
                }
                if (!printed) kprintf(" ");
            }
        }
        kprintf("\033[1;32m|\033[0m\n");  /* right wall */
    }

    /* Bottom wall */
    kprintf("\033[1;32m+");
    for (int x = 0; x < SNK_W; x++) kprintf("-");
    kprintf("+\033[0m\n");

    /* Status line */
    if (snk_won)
        kprintf("\n\033[1;32;5m*** YOU WIN! ***\033[0m\n");
    else if (snk_game_over)
        kprintf("\n\033[1;31;5m*** GAME OVER (score: %d) ***\033[0m\n", snk_score);
    else
        kprintf("\n\033[33mArrow/WASD: move  Q: quit\033[0m\n");
}

void cmd_snake(const char *args) {
    (void)args;
    int speed_ticks = 20;  /* default ~200ms per tick */

    /* Parse optional speed argument: snake fast / snake slow / snake N */
    if (args && *args) {
        if (strcmp(args, "fast") == 0)      speed_ticks = 8;
        else if (strcmp(args, "slow") == 0)  speed_ticks = 40;
        else {
            int n = 0;
            const char *p = args;
            while (*p >= '0' && *p <= '9') {
                n = n * 10 + (*p - '0');
                p++;
            }
            if (n >= 1 && n <= 100) speed_ticks = n;
        }
    }

    snk_init();

    while (!snk_game_over && !snk_won) {
        snk_read_input();
        if (snk_game_over) break;
        snk_tick();
        snk_draw();
        libc_sleep_ticks((uint64_t)speed_ticks);
    }

    /* Final frame */
    snk_draw();
    kprintf("Press any key...\n");
    keyboard_getchar();
}
