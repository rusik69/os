#include "doom.h"

#define DOOR_SPEED 8

void doom_doors_init(doom_state_t *st) {
    st->door_count = 2;
    st->doors[0] = (doom_door_t){ 1, 14, 16, 0, DOOM_KEY_RED, 0 };
    st->doors[1] = (doom_door_t){ 1, 17, 16, 0, DOOM_KEY_BLUE, 0 };
    st->switch_state = 0;
}

int doom_doors_is_wall(const doom_state_t *st, int mx, int my) {
    for (int i = 0; i < st->door_count; i++) {
        const doom_door_t *d = &st->doors[i];
        if (!d->active || d->mx != mx || d->my != my) continue;
        return d->open_frac < 220;
    }
    return 0;
}

void doom_doors_update(doom_state_t *st) {
    for (int i = 0; i < st->door_count; i++) {
        doom_door_t *d = &st->doors[i];
        if (!d->active) continue;
        if (d->open_frac > 0 && d->open_frac < 256) {
            /* animating */
        } else if (d->open_frac >= 256) {
            d->open_frac = 256;
        }
    }
}

static void try_toggle_switch(doom_state_t *st) {
    int32_t cos_a = doom_cos(st->player.angle);
    int32_t sin_a = doom_sin(st->player.angle);
    int32_t fx = st->player.x + (cos_a * DOOM_FRAC) / 65536;
    int32_t fy = st->player.y + (sin_a * DOOM_FRAC) / 65536;
    int mx = (int)(fx / DOOM_FRAC);
    int my = (int)(fy / DOOM_FRAC);
    if (mx < 0 || my < 0 || mx >= DOOM_MAP_SIZE || my >= DOOM_MAP_SIZE) return;
    if (st->cells[my][mx].wall_tex != 3) return;
    st->switch_state ^= 1;
    for (int i = 0; i < st->door_count; i++) {
        doom_door_t *d = &st->doors[i];
        if (!d->active || d->switch_id != 0) continue;
        if (d->open_frac == 0)
            d->open_frac = 1;
    }
}

void doom_doors_try_open(doom_state_t *st) {
    try_toggle_switch(st);

    int32_t cos_a = doom_cos(st->player.angle);
    int32_t sin_a = doom_sin(st->player.angle);
    int32_t fx = st->player.x + (cos_a * DOOM_FRAC * 2) / 65536;
    int32_t fy = st->player.y + (sin_a * DOOM_FRAC * 2) / 65536;
    int mx = (int)(fx / DOOM_FRAC);
    int my = (int)(fy / DOOM_FRAC);

    for (int i = 0; i < st->door_count; i++) {
        doom_door_t *d = &st->doors[i];
        if (!d->active) continue;
        int dx = d->mx - mx;
        int dy = d->my - my;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 2 || dy > 2) continue;
        if (d->need_key == DOOM_KEY_RED && !(st->player.keys & DOOM_KEY_RED)) continue;
        if (d->need_key == DOOM_KEY_BLUE && !(st->player.keys & DOOM_KEY_BLUE)) continue;
        if (d->open_frac < 256)
            d->open_frac += DOOR_SPEED;
        if (d->open_frac > 256) d->open_frac = 256;
    }
}

int doom_test_door_opens(void) {
    doom_state_t st;
    doom_math_init();
    doom_map_init(&st);
    st.player.keys = DOOM_KEY_RED;
    st.player.x = 13 * DOOM_FRAC + DOOM_FRAC / 2;
    st.player.y = 16 * DOOM_FRAC + DOOM_FRAC / 2;
    st.player.angle = 0;
    if (!doom_doors_is_wall(&st, 14, 16)) return 0;
    doom_doors_try_open(&st);
    if (st.doors[0].open_frac <= 0) return 0;
    doom_doors_update(&st);
    for (int i = 0; i < 40; i++)
        doom_doors_try_open(&st);
    return st.doors[0].open_frac >= 220;
}
