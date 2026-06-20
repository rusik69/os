#include "doom.h"
#include "string.h"

/*
 * Level layout: start chamber at (5,10) facing north toward y=7 wall.
 * Sector 0 = halls, 1 = side room, 2 = raised platform, 3 = east wing.
 */
static void set_wall(doom_state_t *st, int x, int y, uint8_t tex) {
    st->cells[y][x].wall_tex = tex;
}

static void set_open(doom_state_t *st, int x, int y, uint8_t sector) {
    st->cells[y][x].wall_tex = 0;
    st->cells[y][x].sector = sector;
}

static void build_map(doom_state_t *st) {
    for (int y = 0; y < DOOM_MAP_SIZE; y++) {
        for (int x = 0; x < DOOM_MAP_SIZE; x++) {
            st->cells[y][x].wall_tex = 1;
            st->cells[y][x].sector = 0;
        }
    }

    st->sector_count = 4;
    st->sectors[0] = (doom_sector_t){ 0, 56, 0, 0, 255 };
    st->sectors[1] = (doom_sector_t){ 0, 72, 1, 1, 255 };
    st->sectors[2] = (doom_sector_t){ 20, 72, 2, 1, 240 };
    st->sectors[3] = (doom_sector_t){ 0, 64, 1, 0, 255 };

    /* Starting chamber (7x5): x=3..9, y=8..12 */
    for (int x = 3; x <= 9; x++) {
        for (int y = 8; y <= 12; y++)
            set_open(st, x, y, 0);
    }

    /* Main hall (5 cells wide): x=3..9, y=14..18 */
    for (int x = 3; x <= 22; x++) {
        for (int y = 14; y <= 18; y++)
            set_open(st, x, y, 0);
    }

    /* West side room */
    for (int x = 11; x <= 16; x++) {
        for (int y = 8; y <= 12; y++)
            set_open(st, x, y, 1);
    }

    /* Raised platform in side room */
    for (int x = 12; x <= 15; x++) {
        for (int y = 9; y <= 11; y++)
            st->cells[y][x].sector = 2;
    }

    /* East wing (large room) */
    for (int x = 18; x <= 26; x++) {
        for (int y = 14; y <= 24; y++)
            set_open(st, x, y, 3);
    }
    for (int x = 19; x <= 25; x++) {
        for (int y = 15; y <= 23; y++)
            set_open(st, x, y, 3);
    }

    /* Connect hall to east wing */
    for (int y = 16; y <= 18; y++)
        set_open(st, 17, y, 0);

    /* Pillars */
    set_wall(st, 6, 16, 2);
    set_wall(st, 19, 18, 2);
    set_wall(st, 23, 21, 2);

    /* Switch wall */
    set_wall(st, 14, 10, 3);

    /* Chamber north wall (perpendicular to spawn view) */
    for (int x = 2; x <= 10; x++)
        set_wall(st, x, 7, 1);

    /* Chamber side walls */
    for (int y = 7; y <= 12; y++) {
        set_wall(st, 2, y, 1);
        set_wall(st, 10, y, 1);
    }

    /* Hall side walls */
    for (int x = 2; x <= 27; x++) {
        set_wall(st, x, 13, 1);
        set_wall(st, x, 19, 1);
    }
    set_wall(st, 2, 14, 1);
    set_wall(st, 2, 15, 1);
    set_wall(st, 2, 16, 1);
    set_wall(st, 2, 17, 1);
    set_wall(st, 2, 18, 1);

    /* Side room walls */
    for (int x = 10; x <= 17; x++) {
        set_wall(st, x, 7, 1);
        set_wall(st, x, 13, 1);
    }
    for (int y = 7; y <= 13; y++) {
        set_wall(st, 10, y, 1);
        set_wall(st, 17, y, 1);
    }

    /* East wing outer walls */
    for (int x = 17; x <= 27; x++) {
        set_wall(st, x, 13, 1);
        set_wall(st, x, 25, 1);
    }
    for (int y = 13; y <= 25; y++) {
        set_wall(st, 17, y, 1);
        set_wall(st, 27, y, 1);
    }

    /* Door cells stay open */
    set_open(st, 14, 16, 0);
    set_open(st, 17, 16, 0);

    /* Chamber south exit into main hall */
    for (int x = 3; x <= 9; x++)
        set_open(st, x, 13, 0);

    /* Chamber east exit into side room */
    for (int y = 9; y <= 11; y++)
        set_open(st, 10, y, 0);
}

void doom_map_init(doom_state_t *st) {
    memset(st, 0, sizeof(*st));
    build_map(st);

    st->player.x = 5 * DOOM_FRAC + DOOM_FRAC / 2;
    st->player.y = 10 * DOOM_FRAC + DOOM_FRAC / 2;
    st->player.angle = (DOOM_ANGLE_UNITS * 3) / 4;
    st->player.health = 100;
    st->player.ammo = 50;
    st->player.shells = 8;
    st->player.weapon = 0;
    st->player.keys = 0;
    st->spawn_grace = 180;

    doom_doors_init(st);

    static const int espawns[][2] = {
        {8, 16}, {12, 16}, {20, 18}, {22, 20}, {24, 22}
    };
    st->enemy_count = 5;
    for (int i = 0; i < st->enemy_count; i++) {
        st->enemies[i].active = 1;
        st->enemies[i].x = espawns[i][0] * DOOM_FRAC + DOOM_FRAC / 2;
        st->enemies[i].y = espawns[i][1] * DOOM_FRAC + DOOM_FRAC / 2;
        st->enemies[i].health = 30;
        st->enemies[i].state = DOOM_ENEMY_IDLE;
        st->enemies[i].attack_cd = 0;
    }

    static const struct { int x, y; doom_pickup_type_t type; } pspawns[] = {
        {4, 10, DOOM_PICKUP_AMMO},
        {13, 11, DOOM_PICKUP_HEALTH},
        {14, 9, DOOM_PICKUP_KEY_RED},
        {10, 16, DOOM_PICKUP_SHELLS},
        {21, 20, DOOM_PICKUP_KEY_BLUE},
    };
    st->pickup_count = 5;
    for (int i = 0; i < st->pickup_count; i++) {
        st->pickups[i].active = 1;
        st->pickups[i].x = pspawns[i].x * DOOM_FRAC + DOOM_FRAC / 2;
        st->pickups[i].y = pspawns[i].y * DOOM_FRAC + DOOM_FRAC / 2;
        st->pickups[i].type = pspawns[i].type;
    }
}

const doom_sector_t *doom_map_sector(const doom_state_t *st, int mx, int my) {
    if (mx < 0 || my < 0 || mx >= DOOM_MAP_SIZE || my >= DOOM_MAP_SIZE)
        return &st->sectors[0];
    uint8_t sid = st->cells[my][mx].sector;
    if (sid >= st->sector_count)
        sid = 0;
    return &st->sectors[sid];
}

int doom_map_is_wall(const doom_state_t *st, int mx, int my) {
    if (mx < 0 || my < 0 || mx >= DOOM_MAP_SIZE || my >= DOOM_MAP_SIZE)
        return 1;
    if (doom_doors_is_wall(st, mx, my))
        return 1;
    return st->cells[my][mx].wall_tex != 0;
}

uint8_t doom_map_tex(const doom_state_t *st, int mx, int my) {
    if (mx < 0 || my < 0 || mx >= DOOM_MAP_SIZE || my >= DOOM_MAP_SIZE)
        return 0;
    uint8_t t = st->cells[my][mx].wall_tex;
    if (t == 0) return 0;
    return (uint8_t)(t - 1);
}

int doom_map_collide(const doom_state_t *st, int32_t x, int32_t y) {
    int mx = (int)(x / DOOM_FRAC);
    int my = (int)(y / DOOM_FRAC);
    int r = DOOM_FRAC / 4;
    if (doom_map_is_wall(st, mx, my)) return 1;
    if (doom_map_is_wall(st, (x + r) / DOOM_FRAC, my)) return 1;
    if (doom_map_is_wall(st, (x - r) / DOOM_FRAC, my)) return 1;
    if (doom_map_is_wall(st, mx, (y + r) / DOOM_FRAC)) return 1;
    if (doom_map_is_wall(st, mx, (y - r) / DOOM_FRAC)) return 1;
    return 0;
}

static void collect_pickups(doom_state_t *st) {
    for (int i = 0; i < st->pickup_count; i++) {
        doom_pickup_t *p = &st->pickups[i];
        if (!p->active) continue;
        int32_t dx = p->x - st->player.x;
        int32_t dy = p->y - st->player.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > DOOM_FRAC / 2 || dy > DOOM_FRAC / 2) continue;
        switch (p->type) {
        case DOOM_PICKUP_AMMO:   st->player.ammo += 20; break;
        case DOOM_PICKUP_SHELLS: st->player.shells += 4; break;
        case DOOM_PICKUP_HEALTH:
            st->player.health += 25;
            if (st->player.health > 100) st->player.health = 100;
            break;
        case DOOM_PICKUP_KEY_RED:  st->player.keys |= DOOM_KEY_RED; break;
        case DOOM_PICKUP_KEY_BLUE: st->player.keys |= DOOM_KEY_BLUE; break;
        }
        p->active = 0;
    }
}

void doom_map_update(doom_state_t *st) {
    collect_pickups(st);
    doom_doors_update(st);
}

int doom_test_collision(void) {
    doom_state_t st;
    doom_map_init(&st);
    if (doom_map_collide(&st, st.player.x, st.player.y)) return 0;
    int32_t wx = 0 * DOOM_FRAC + DOOM_FRAC / 2;
    int32_t wy = 0 * DOOM_FRAC + DOOM_FRAC / 2;
    if (!doom_map_collide(&st, wx, wy)) return 0;
    return 1;
}
