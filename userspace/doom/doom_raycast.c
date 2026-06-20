#include "doom.h"

extern uint32_t doom_tex_wall_color(int tex_id, int u, int v);

#define DOOM_FOG_COLOR DOOM_COLOR(60, 45, 35)
#define DOOM_FOG_START 18
#define DOOM_FOG_MAX   180

uint32_t doom_apply_fog(uint32_t color, int32_t dist) {
    if (dist <= DOOM_FOG_START) return color;
    int fog = (int)(dist - DOOM_FOG_START) * 12;
    if (fog > DOOM_FOG_MAX) fog = DOOM_FOG_MAX;
    return doom_lerp_rgb(color, DOOM_FOG_COLOR, fog, 256);
}

static int door_at(const doom_state_t *st, int mx, int my) {
    for (int i = 0; i < st->door_count; i++) {
        const doom_door_t *d = &st->doors[i];
        if (d->active && d->mx == mx && d->my == my)
            return i;
    }
    return -1;
}

void doom_raycast(const doom_state_t *st, int32_t angle, doom_ray_hit_t *out) {
    out->hit = 0;
    int32_t sin_a = doom_sin(angle);
    int32_t cos_a = doom_cos(angle);

    int32_t px = st->player.x;
    int32_t py = st->player.y;

    int32_t map_x = (int)(px / DOOM_FRAC);
    int32_t map_y = (int)(py / DOOM_FRAC);

    int32_t delta_dist_x, delta_dist_y;
    if (cos_a == 0) delta_dist_x = 0x7FFFFFFF;
    else delta_dist_x = (DOOM_FRAC * 65536) / (cos_a < 0 ? -cos_a : cos_a);
    if (sin_a == 0) delta_dist_y = 0x7FFFFFFF;
    else delta_dist_y = (DOOM_FRAC * 65536) / (sin_a < 0 ? -sin_a : sin_a);

    int32_t step_x, step_y;
    int32_t side_dist_x, side_dist_y;

    if (cos_a < 0) { step_x = -1; side_dist_x = ((px % DOOM_FRAC) * delta_dist_x) / DOOM_FRAC; }
    else { step_x = 1; side_dist_x = ((DOOM_FRAC - px % DOOM_FRAC) * delta_dist_x) / DOOM_FRAC; }
    if (sin_a < 0) { step_y = -1; side_dist_y = ((py % DOOM_FRAC) * delta_dist_y) / DOOM_FRAC; }
    else { step_y = 1; side_dist_y = ((DOOM_FRAC - py % DOOM_FRAC) * delta_dist_y) / DOOM_FRAC; }

    int side = 0;
    int hit = 0;
    int32_t perp_dist = 0;
    for (int i = 0; i < 64; i++) {
        if (side_dist_x < side_dist_y) {
            side_dist_x += delta_dist_x;
            map_x += step_x;
            side = 0;
        } else {
            side_dist_y += delta_dist_y;
            map_y += step_y;
            side = 1;
        }
        int di = door_at(st, map_x, map_y);
        if (di >= 0 && st->doors[di].open_frac < 220) {
            hit = 1;
            if (side == 0)
                perp_dist = (side_dist_x - delta_dist_x);
            else
                perp_dist = (side_dist_y - delta_dist_y);
            break;
        }
        if (doom_map_is_wall(st, map_x, map_y)) {
            hit = 1;
            if (side == 0)
                perp_dist = (side_dist_x - delta_dist_x);
            else
                perp_dist = (side_dist_y - delta_dist_y);
            break;
        }
    }

    out->hit = hit;
    out->perp_dist = perp_dist;
    if (perp_dist < DOOM_FRAC) perp_dist = DOOM_FRAC;
    out->dist = perp_dist / DOOM_FRAC;
    out->side = side;
    out->map_x = map_x;
    out->map_y = map_y;
    out->tex = doom_map_tex(st, map_x, map_y);

    const doom_sector_t *sec = doom_map_sector(st, map_x, map_y);
    if (sec) {
        out->floor_h = sec->floor_h;
        out->ceil_h = sec->ceil_h;
    } else {
        out->floor_h = 0;
        out->ceil_h = 56;
    }

    if (side == 0) {
        int32_t hit_y = py + (perp_dist * sin_a) / DOOM_FRAC;
        out->wall_x = hit_y & (DOOM_FRAC - 1);
    } else {
        int32_t hit_x = px + (perp_dist * cos_a) / DOOM_FRAC;
        out->wall_x = hit_x & (DOOM_FRAC - 1);
    }
}

void doom_raycast_column(const doom_state_t *st, int col, int32_t fov,
                         uint32_t *fb, int32_t *depth) {
    int32_t rel = ((col - DOOM_SCREEN_W / 2) * fov) / DOOM_SCREEN_W;
    int32_t angle = (st->player.angle + rel) & (DOOM_ANGLE_UNITS - 1);

    doom_ray_hit_t hit;
    doom_raycast(st, angle, &hit);
    depth[col] = hit.dist;

    int draw_start = 0;
    int draw_end = DOOM_VIEW_H;

    if (hit.hit) {
        int32_t pd = hit.perp_dist;
        if (pd < DOOM_FRAC) pd = DOOM_FRAC;
        int line_h = (int)((DOOM_VIEW_H * DOOM_FRAC) / pd);
        if (line_h < 1) line_h = 1;
        if (line_h > DOOM_VIEW_H) line_h = DOOM_VIEW_H;

        draw_start = DOOM_VIEW_H / 2 - line_h / 2;
        draw_end = draw_start + line_h;
        if (draw_start < 0) draw_start = 0;
        if (draw_end > DOOM_VIEW_H) draw_end = DOOM_VIEW_H;
    }

    /* Floor/ceiling casting replaces sky gradient and floor placeholder */
    /* Cast floor and ceiling via doom_floor_column, which handles both */
    if (!hit.hit) {
        /* No wall hit: cast full floor and ceiling */
        doom_floor_column(st, col, fov, 0, DOOM_VIEW_H, fb);
        return;
    }

    /* Wall was hit: draw wall texture */
    int tex_u = (hit.wall_x * DOOM_TEX_SIZE) / DOOM_FRAC;
    if (hit.side == 0 && doom_sin(angle) > 0) tex_u = DOOM_TEX_SIZE - tex_u - 1;
    if (hit.side == 1 && doom_cos(angle) < 0) tex_u = DOOM_TEX_SIZE - tex_u - 1;

    int di = door_at(st, hit.map_x, hit.map_y);
    if (di >= 0)
        hit.tex = 1;

    for (int y = draw_start; y < draw_end; y++) {
        int d = y * DOOM_TEX_SIZE - draw_start * DOOM_TEX_SIZE + (draw_end - draw_start) / 2;
        int span = draw_end - draw_start;
        if (span < 1) span = 1;
        int tex_v = (d * DOOM_TEX_SIZE) / span;
        uint32_t color = doom_tex_wall_color(hit.tex, tex_u, tex_v);
        if (hit.side == 1)
            color = doom_scale_rgb(color, 3, 4);
        int shade = 256 - (int)(hit.dist * 6);
        if (shade < 80) shade = 80;
        color = doom_scale_rgb(color, shade, 256);
        fb[y * DOOM_SCREEN_W + col] = doom_apply_fog(color, hit.dist);
    }

    /* Cast floor and ceiling using proper texture mapping */
    doom_floor_column(st, col, fov, draw_start, draw_end, fb);
}

int doom_test_ray_hit(void) {
    doom_state_t st;
    doom_math_init();
    doom_map_init(&st);
    doom_ray_hit_t hit;
    doom_raycast(&st, st.player.angle, &hit);
    if (!hit.hit) return 0;
    if (hit.dist < 2 || hit.dist > 8) return 0;
    return 1;
}

int doom_test_column_has_sky(void) {
    doom_state_t st;
    uint32_t fb[DOOM_SCREEN_W * DOOM_VIEW_H];
    int32_t depth[DOOM_SCREEN_W];
    doom_ray_hit_t hit;
    doom_math_init();
    doom_map_init(&st);
    doom_raycast(&st, st.player.angle, &hit);
    if (!hit.hit || hit.dist < 2) return 0;
    int col = DOOM_SCREEN_W / 2;
    doom_raycast_column(&st, col, DOOM_ANGLE_UNITS / 3, fb, depth);
    uint32_t top = fb[col];
    return DOOM_B(top) > DOOM_R(top) + 10;
}

int doom_test_column_has_wall(void) {
    doom_state_t st;
    uint32_t fb[DOOM_SCREEN_W * DOOM_VIEW_H];
    int32_t depth[DOOM_SCREEN_W];
    doom_math_init();
    doom_map_init(&st);
    int col = DOOM_SCREEN_W / 2;
    doom_raycast_column(&st, col, DOOM_ANGLE_UNITS / 3, fb, depth);
    uint32_t mid = fb[(DOOM_VIEW_H / 2) * DOOM_SCREEN_W + col];
    uint32_t bot = fb[(DOOM_VIEW_H - 1) * DOOM_SCREEN_W + col];
    return mid != bot && depth[col] > 0 && depth[col] < 20;
}
