#include "doom.h"
#include "vga.h"

static uint32_t doom_fb[DOOM_SCREEN_W * DOOM_SCREEN_H];
static int32_t doom_depth[DOOM_SCREEN_W];

extern void doom_draw_weapon(const doom_state_t *st, uint32_t *fb);

static void draw_char(uint32_t *fb, int x, int y, char c, uint32_t fg) {
    static const uint8_t font[10][7] = {
        {0x3E,0x51,0x49,0x45,0x3E,0,0},
        {0x00,0x42,0x7F,0x40,0x00,0,0},
        {0x62,0x51,0x49,0x49,0x46,0,0},
        {0x22,0x41,0x49,0x49,0x36,0,0},
        {0x18,0x14,0x12,0x7F,0x10,0,0},
        {0x27,0x45,0x45,0x45,0x39,0,0},
        {0x3C,0x4A,0x49,0x49,0x30,0,0},
        {0x01,0x71,0x09,0x05,0x03,0,0},
        {0x36,0x49,0x49,0x49,0x36,0,0},
        {0x06,0x49,0x49,0x29,0x1E,0,0},
    };
    int idx = -1;
    if (c >= '0' && c <= '9') idx = c - '0';
    if (idx < 0) return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = font[idx][row];
        for (int col = 0; col < 6; col++) {
            if (bits & (1 << (5 - col)))
                fb[(y + row) * DOOM_SCREEN_W + (x + col)] = fg;
        }
    }
}

static void draw_number(uint32_t *fb, int x, int y, int val, uint32_t fg) {
    if (val < 0) val = 0;
    if (val > 999) val = 999;
    draw_char(fb, x, y, (char)('0' + (val / 100) % 10), fg);
    draw_char(fb, x + 8, y, (char)('0' + (val / 10) % 10), fg);
    draw_char(fb, x + 16, y, (char)('0' + val % 10), fg);
}

static void draw_status_bar(doom_state_t *st, uint32_t *fb) {
    uint32_t bar_bg = DOOM_COLOR(88, 64, 40);
    uint32_t bar_edge = DOOM_COLOR(48, 32, 20);
    if (st->damage_flash > 0)
        bar_bg = DOOM_COLOR(120, 32, 32);
    int y0 = DOOM_VIEW_H;

    for (int y = y0; y < DOOM_SCREEN_H; y++) {
        for (int x = 0; x < DOOM_SCREEN_W; x++)
            fb[y * DOOM_SCREEN_W + x] = bar_bg;
    }
    for (int x = 0; x < DOOM_SCREEN_W; x++) {
        fb[y0 * DOOM_SCREEN_W + x] = bar_edge;
        fb[(DOOM_SCREEN_H - 1) * DOOM_SCREEN_W + x] = bar_edge;
    }

    /* Health (left) */
    draw_number(fb, 16, y0 + 12, st->player.health, DOOM_COLOR(180, 0, 0));

    /* Ammo / shells (right) */
    if (st->player.weapon == 0)
        draw_number(fb, DOOM_SCREEN_W - 56, y0 + 12, st->player.ammo,
                    DOOM_COLOR(200, 180, 60));
    else
        draw_number(fb, DOOM_SCREEN_W - 56, y0 + 12, st->player.shells,
                    DOOM_COLOR(200, 140, 40));

    /* Face tile (center) */
    uint32_t face_col;
    if (st->player.health > 60)
        face_col = DOOM_COLOR(60, 140, 50);
    else if (st->player.health > 25)
        face_col = DOOM_COLOR(180, 160, 40);
    else
        face_col = DOOM_COLOR(160, 40, 40);

    int fx = DOOM_SCREEN_W / 2 - 12;
    int fy = y0 + 4;
    for (int y = fy; y < fy + 24; y++) {
        for (int x = fx; x < fx + 24; x++) {
            uint32_t c = face_col;
            if (x == fx || x == fx + 23 || y == fy || y == fy + 23)
                c = bar_edge;
            /* eyes */
            if ((y == fy + 8 || y == fy + 9) && (x == fx + 7 || x == fx + 16))
                c = DOOM_COLOR(20, 20, 20);
            fb[y * DOOM_SCREEN_W + x] = c;
        }
    }

    if (st->won) {
        for (int y = DOOM_VIEW_H / 2 - 12; y < DOOM_VIEW_H / 2 + 12; y++)
            for (int x = DOOM_SCREEN_W / 2 - 48; x < DOOM_SCREEN_W / 2 + 48; x++)
                if (y >= 0 && y < DOOM_VIEW_H)
                    fb[y * DOOM_SCREEN_W + x] = DOOM_COLOR(0, 100, 0);
    }
}

void doom_render_frame(doom_state_t *st) {
    int32_t fov = DOOM_ANGLE_UNITS / 3;
    for (int col = 0; col < DOOM_SCREEN_W; col++)
        doom_raycast_column(st, col, fov, doom_fb, doom_depth);

    doom_sprite_t sprites[DOOM_MAX_SPRITES];
    int sc = 0;
    doom_sprites_collect(st, sprites, &sc);
    doom_sprites_draw(st, sprites, sc, doom_fb, doom_depth);

    doom_draw_weapon(st, doom_fb);
    draw_status_bar(st, doom_fb);
}

#define DOOM_BLIT_STATUS_PX 48

static void blit_row(volatile uint32_t *row, uint32_t fw, uint32_t dy, uint32_t fh) {
    uint32_t status_px = DOOM_BLIT_STATUS_PX;
    if (status_px > fh) status_px = fh;
    uint32_t view_h = fh - status_px;

    if (dy < view_h) {
        int sy = view_h > 0 ? (int)(dy * DOOM_VIEW_H / view_h) : 0;
        for (uint32_t dx = 0; dx < fw; dx++) {
            int sx = (int)(dx * DOOM_SCREEN_W / fw);
            row[dx] = doom_fb[sy * DOOM_SCREEN_W + sx];
        }
        return;
    }

    int rel = (int)(dy - view_h);
    int pad = (int)status_px > DOOM_STATUS_H ? ((int)status_px - DOOM_STATUS_H) / 2 : 0;
    uint32_t pad_col = DOOM_COLOR(48, 32, 20);
    for (uint32_t dx = 0; dx < fw; dx++) {
        int sx = (int)(dx * DOOM_SCREEN_W / fw);
        if (rel < pad || rel >= pad + DOOM_STATUS_H) {
            row[dx] = pad_col;
            continue;
        }
        int sy = DOOM_VIEW_H + (rel - pad);
        row[dx] = doom_fb[sy * DOOM_SCREEN_W + sx];
    }
}

void doom_render_blit(void) {
    uint8_t *fb_ptr;
    uint32_t fw, fh, pitch;
    vga_get_framebuffer_ptr(&fb_ptr, &fw, &fh, &pitch);
    if (fw == 0 || fh == 0)
        vga_get_framebuffer_info(&fw, &fh, &pitch, NULL);

    if (!fb_ptr || fw == 0 || fh == 0) {
        if (fw == 0) fw = 1024;
        if (fh == 0) fh = 768;
        for (uint32_t dy = 0; dy < fh; dy++) {
            for (uint32_t dx = 0; dx < fw; dx++) {
                uint32_t status_px = DOOM_BLIT_STATUS_PX;
                if (status_px > fh) status_px = fh;
                uint32_t view_h = fh - status_px;
                uint32_t color;
                if (dy < view_h) {
                    int sy = view_h > 0 ? (int)(dy * DOOM_VIEW_H / view_h) : 0;
                    int sx = (int)(dx * DOOM_SCREEN_W / fw);
                    color = doom_fb[sy * DOOM_SCREEN_W + sx];
                } else {
                    int rel = (int)(dy - view_h);
                    int pad = (int)status_px > DOOM_STATUS_H
                              ? ((int)status_px - DOOM_STATUS_H) / 2 : 0;
                    int sx = (int)(dx * DOOM_SCREEN_W / fw);
                    if (rel < pad || rel >= pad + DOOM_STATUS_H)
                        color = DOOM_COLOR(48, 32, 20);
                    else {
                        int sy = DOOM_VIEW_H + (rel - pad);
                        color = doom_fb[sy * DOOM_SCREEN_W + sx];
                    }
                }
                vga_put_pixel((int32_t)dx, (int32_t)dy, color);
            }
        }
        return;
    }

    for (uint32_t dy = 0; dy < fh; dy++) {
        volatile uint32_t *row = (volatile uint32_t *)(fb_ptr + dy * pitch);
        blit_row(row, fw, dy, fh);
    }
    __asm__ volatile("mfence" ::: "memory");
}

int doom_test_frame_varies(void) {
    doom_state_t st;
    static const int cols[] = { 0, 80, 160, 240, 319 };
    doom_math_init();
    doom_map_init(&st);
    doom_render_frame(&st);

    int y = 40;
    uint32_t p0 = doom_fb[y * DOOM_SCREEN_W + cols[0]];
    for (int i = 1; i < 5; i++) {
        if (doom_fb[y * DOOM_SCREEN_W + cols[i]] != p0)
            goto check_sky;
    }
    return 0;

check_sky:
    for (int x = 0; x < DOOM_SCREEN_W; x++) {
        uint32_t c = doom_fb[x];
        if (DOOM_B(c) > DOOM_R(c) + 10)
            return 1;
    }
    return 0;
}
