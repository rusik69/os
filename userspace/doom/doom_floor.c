#include "doom.h"

extern uint32_t doom_tex_flat_color(int tex_id, int u, int v);

void doom_floor_column(const doom_state_t *st, int col, int32_t fov,
                       int wall_top, int wall_bot, uint32_t *fb) {
    (void)wall_top;
    int32_t rel = ((col - DOOM_SCREEN_W / 2) * fov) / DOOM_SCREEN_W;
    int32_t angle = (st->player.angle + rel) & (DOOM_ANGLE_UNITS - 1);
    int32_t sin_a = doom_sin(angle);
    int32_t cos_a = doom_cos(angle);

    int mx = (int)(st->player.x / DOOM_FRAC);
    int my = (int)(st->player.y / DOOM_FRAC);
    const doom_sector_t *sec = doom_map_sector(st, mx, my);
    uint8_t floor_tex = sec ? sec->floor_tex : 0;

    int half_h = DOOM_VIEW_H / 2;

    /* Floor texture below wall bottom (sky/walls already drawn above) */
    for (int y = wall_bot; y < DOOM_VIEW_H; y++) {
        int p = y - half_h;
        if (p < 1) p = 1;
        int32_t row_dist = (DOOM_FRAC * half_h) / p;
        int32_t rx = st->player.x + (row_dist * cos_a) / DOOM_FRAC;
        int32_t ry = st->player.y + (row_dist * sin_a) / DOOM_FRAC;
        int u = (rx / 8) & 7;
        int v = (ry / 8) & 7;
        uint32_t c = doom_tex_flat_color(floor_tex, u, v);
        int shade = 256 - (int)(row_dist / DOOM_FRAC) * 6;
        if (shade < 80) shade = 80;
        c = doom_scale_rgb(c, shade, 256);
        fb[y * DOOM_SCREEN_W + col] = doom_apply_fog(c, row_dist / DOOM_FRAC);
    }
}
