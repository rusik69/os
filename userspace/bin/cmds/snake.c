/* snake.c — Text Snake game using ANSI escapes */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define W 20
#define H 15

static char grid[H][W];
static int snake[W*H][2];
static int head, tail, len;
static int food[2];
static int dir; /* 0=up, 1=down, 2=left, 3=right */
static int gameover;

static void init_game(void) {
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            grid[r][c] = ' ';
    head = 0; tail = 0; len = 3;
    snake[0][0] = H/2; snake[0][1] = W/2;
    snake[1][0] = H/2; snake[1][1] = W/2 - 1;
    snake[2][0] = H/2; snake[2][1] = W/2 - 2;
    for (int i = 0; i < len; i++)
        grid[snake[i][0]][snake[i][1]] = '#';
    dir = 3;
    gameover = 0;
    /* Place food */
    food[0] = 1; food[1] = 1;
    grid[food[0]][food[1]] = '@';
}

static void spawn_food(void) {
    int empty = 0;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            if (grid[r][c] == ' ') empty++;
    if (empty == 0) return;
    int idx = rand() % empty;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            if (grid[r][c] == ' ' && idx-- == 0) {
                food[0] = r; food[1] = c;
                grid[r][c] = '@';
                return;
            }
}

static void draw(void) {
    printf("\033[H");
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++)
            printf("%c ", grid[r][c]);
        printf("\n");
    }
    printf("Use wasd. Score: %d\n", len - 3);
}

static void update(void) {
    int nr = snake[head][0], nc = snake[head][1];
    if (dir == 0) nr--;
    else if (dir == 1) nr++;
    else if (dir == 2) nc--;
    else nc++;
    if (nr < 0 || nr >= H || nc < 0 || nc >= W) { gameover = 1; return; }
    if (grid[nr][nc] == '#') { gameover = 1; return; }
    /* Move head */
    head = (head + 1) % (W*H);
    snake[head][0] = nr; snake[head][1] = nc;
    int ate = (nr == food[0] && nc == food[1]);
    if (ate) {
        len++;
        grid[nr][nc] = '#';
        spawn_food();
    } else {
        grid[nr][nc] = '#';
        /* Remove tail */
        int tr = snake[tail][0], tc = snake[tail][1];
        grid[tr][tc] = ' ';
        tail = (tail + 1) % (W*H);
    }
}

int main(void) {
    char buf[16];
    init_game();
    printf("\033[2J");
    while (!gameover) {
        draw();
        int n = read(0, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        for (int i = 0; buf[i]; i++) {
            if (buf[i] == 'w' && dir != 1) dir = 0;
            else if (buf[i] == 's' && dir != 0) dir = 1;
            else if (buf[i] == 'a' && dir != 3) dir = 2;
            else if (buf[i] == 'd' && dir != 2) dir = 3;
        }
        update();
    }
    printf("\033[2JGame over! Score: %d\n", len - 3);
    return 0;
}
