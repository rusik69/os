#include "doom.h"

extern uint32_t doom_tex_enemy_color(int frame, int u, int v);
extern uint32_t doom_tex_pistol_color(int frame, int u, int v);
extern uint32_t doom_tex_shotgun_color(int frame, int u, int v);
extern uint32_t doom_tex_pickup_color(doom_pickup_type_t type, int u, int v);

static int32_t sprite_linear_dist(int32_t dx, int32_t dy) {
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int32_t d = dx > dy ? dx : dy;
    if (d < DOOM_FRAC) return 1;
    return d / DOOM_FRAC;
}

static int32_t sprite_dist(const doom_state_t *st, int32_t sx, int32_t sy) {
    int32_t dx = sx - st->player.x;
    int32_t dy = sy - st->player.y;
    return sprite_linear_dist(dx, dy);
}

void doom_sprites_collect(const doom_state_t *st, doom_sprite_t *out, int *count) {
    int n = 0;
    for (int i = 0; i < DOOM_MAX_ENEMIES; i++) {
        const doom_enemy_t *e = &st->enemies[i];
        if (!e->active || e->state == DOOM_ENEMY_DEAD) continue;
        if (n >= DOOM_MAX_SPRITES) break;
        out[n].type = DOOM_SPRITE_ENEMY;
        out[n].x = e->x;
        out[n].y = e->y;
        out[n].dist = sprite_dist(st, e->x, e->y);
        out[n].frame = e->frame;
        n++;
    }
    for (int i = 0; i < st->pickup_count; i++) {
        const doom_pickup_t *p = &st->pickups[i];
        if (!p->active) continue;
        if (n >= DOOM_MAX_SPRITES) break;
        out[n].type = DOOM_SPRITE_PICKUP;
        out[n].x = p->x;
        out[n].y = p->y;
        out[n].dist = sprite_dist(st, p->x, p->y);
        out[n].frame = 0;
        out[n].pickup_type = p->type;
        n++;
    }
    *count = n;
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (out[j].dist > out[i].dist) {
                doom_sprite_t tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
}

static void draw_sprite_column(const doom_state_t *st, const doom_sprite_t *sp,
                               int screen_x, uint32_t *fb, const int32_t *depth) {
    int32_t dx = sp->x - st->player.x;
    int32_t dy = sp->y - st->player.y;

    int32_t rel = doom_atan2(dy, dx) - st->player.angle;
    rel &= (DOOM_ANGLE_UNITS - 1);
    if (rel > DOOM_ANGLE_UNITS / 2)
        rel -= DOOM_ANGLE_UNITS;

    int32_t fov = DOOM_ANGLE_UNITS / 3;
    int32_t screen_rel = (rel * DOOM_SCREEN_W) / fov;
    int cx = DOOM_SCREEN_W / 2 + (int)screen_rel;
    if (screen_x != cx) return;
    if (cx < 0 || cx >= DOOM_SCREEN_W) return;

    int32_t dist = sprite_dist(st, sp->x, sp->y);
    if (dist < 1) dist = 1;

    int sprite_h;
    if (sp->type == DOOM_SPRITE_PICKUP)
        sprite_h = DOOM_VIEW_H / (dist * 2);
    else
        sprite_h = (DOOM_VIEW_H * 2) / dist;
    if (sprite_h < 4) return;

    int draw_start = DOOM_VIEW_H / 2 - sprite_h / 2;
    int draw_end = draw_start + sprite_h;
    if (draw_start < 0) draw_start = 0;
    if (draw_end > DOOM_VIEW_H) draw_end = DOOM_VIEW_H;

    int sprite_w = sprite_h;
    if (sp->type == DOOM_SPRITE_PICKUP)
        sprite_w = sprite_h / 2;
    int left = cx - sprite_w / 2;

    for (int x = left; x < left + sprite_w; x++) {
        if (x < 0 || x >= DOOM_SCREEN_W) continue;
        if (depth[x] < dist) continue;
        for (int y = draw_start; y < draw_end; y++) {
            int u = ((x - left) * DOOM_TEX_SIZE) / sprite_w;
            int v = ((y - draw_start) * DOOM_TEX_SIZE) / sprite_h;
            uint32_t color;
            if (sp->type == DOOM_SPRITE_PICKUP)
                color = doom_tex_pickup_color(sp->pickup_type, u, v);
            else
                color = doom_tex_enemy_color(sp->frame, u, v);
            if ((color & 0xFFFFFF) == 0) continue;
            fb[y * DOOM_SCREEN_W + x] = color;
        }
    }
}

void doom_sprites_draw(const doom_state_t *st, const doom_sprite_t *sprites, int count,
                       uint32_t *fb, const int32_t *depth) {
    for (int i = 0; i < count; i++) {
        int32_t dx = sprites[i].x - st->player.x;
        int32_t dy = sprites[i].y - st->player.y;
        int32_t rel = doom_atan2(dy, dx) - st->player.angle;
        rel &= (DOOM_ANGLE_UNITS - 1);
        if (rel > DOOM_ANGLE_UNITS / 2) rel -= DOOM_ANGLE_UNITS;
        int32_t fov = DOOM_ANGLE_UNITS / 3;
        int cx = DOOM_SCREEN_W / 2 + (int)((rel * DOOM_SCREEN_W) / fov);
        draw_sprite_column(st, &sprites[i], cx, fb, depth);
    }
}

void doom_draw_weapon(const doom_state_t *st, uint32_t *fb) {
    int frame = st->pistol_frame;
    if (st->muzzle_flash) frame = 1;

    int bob = 0;
    if (st->player.moving)
        bob = (st->player.walk_phase < 16)
              ? st->player.walk_phase / 2
              : (31 - st->player.walk_phase) / 2;

    int base_x = DOOM_SCREEN_W / 2 - 24;
    int base_y = DOOM_VIEW_H - 56 + bob;

    for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 48; x++) {
            uint32_t color;
            if (st->player.weapon == 0)
                color = doom_tex_pistol_color(frame, x / 6, y / 6);
            else
                color = doom_tex_shotgun_color(frame, x / 6, y / 6);
            if ((color & 0xFFFFFF) == 0) continue;
            int sx = base_x + x;
            int sy = base_y + y;
            if (sx >= 0 && sx < DOOM_SCREEN_W && sy >= 0 && sy < DOOM_VIEW_H)
                fb[sy * DOOM_SCREEN_W + sx] = color;
        }
    }
}
