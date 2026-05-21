#include "doom.h"

static const uint32_t wall_palette[4] = {
    DOOM_COLOR(72, 56, 40),
    DOOM_COLOR(108, 88, 64),
    DOOM_COLOR(140, 100, 72),
    DOOM_COLOR(48, 48, 56),
};

static const uint32_t flat_palette[4] = {
    DOOM_COLOR(88, 68, 52),
    DOOM_COLOR(64, 64, 72),
    DOOM_COLOR(96, 80, 56),
    DOOM_COLOR(52, 40, 32),
};

static const uint8_t wall_stone[64] = {
    1,1,1,2,2,1,1,1,
    1,2,2,2,2,2,1,1,
    1,2,3,2,3,2,2,1,
    2,2,3,2,3,2,2,2,
    2,2,2,2,2,2,2,2,
    1,2,2,3,2,2,2,1,
    1,1,2,2,2,2,1,1,
    1,1,1,2,2,1,1,1,
};

static const uint8_t wall_metal[64] = {
    3,3,3,3,3,3,3,3,
    3,1,1,1,1,1,1,3,
    3,1,2,2,2,2,1,3,
    3,1,2,0,0,2,1,3,
    3,1,2,0,0,2,1,3,
    3,1,2,2,2,2,1,3,
    3,1,1,1,1,1,1,3,
    3,3,3,3,3,3,3,3,
};

static const uint8_t wall_blood[64] = {
    1,1,2,1,1,2,1,1,
    1,0,0,2,0,0,1,1,
    2,0,0,0,0,2,2,1,
    1,2,0,0,0,0,2,1,
    1,2,0,0,0,2,2,1,
    1,1,2,0,2,2,1,1,
    1,1,1,2,2,1,1,1,
    1,1,1,1,1,1,1,1,
};

static const uint8_t flat_stone[64] = {
    0,0,1,1,0,0,1,1,
    0,1,1,1,1,1,1,0,
    1,1,2,1,1,2,1,1,
    1,1,1,1,1,1,1,1,
    0,1,1,2,2,1,1,0,
    0,1,1,1,1,1,1,0,
    0,0,1,1,1,1,0,0,
    0,0,0,1,1,0,0,0,
};

static const uint8_t flat_tile[64] = {
    1,1,1,1,1,1,1,1,
    1,0,0,1,1,0,0,1,
    1,0,0,1,1,0,0,1,
    1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,
    1,0,0,1,1,0,0,1,
    1,0,0,1,1,0,0,1,
    1,1,1,1,1,1,1,1,
};

static const uint8_t flat_plate[64] = {
    2,2,2,2,2,2,2,2,
    2,1,1,1,1,1,1,2,
    2,1,0,0,0,0,1,2,
    2,1,0,0,0,0,1,2,
    2,1,0,0,0,0,1,2,
    2,1,0,0,0,0,1,2,
    2,1,1,1,1,1,1,2,
    2,2,2,2,2,2,2,2,
};

static const uint8_t enemy_frames[DOOM_ENEMY_FRAMES][64] = {
    {
        0,0,1,1,1,1,0,0,
        0,1,2,2,2,2,1,0,
        1,2,3,2,3,2,2,1,
        1,2,2,2,2,2,2,1,
        1,2,2,2,2,2,2,1,
        0,1,1,0,0,1,1,0,
        0,1,0,0,0,0,1,0,
        0,0,0,0,0,0,0,0,
    },
    {
        0,0,1,1,1,1,0,0,
        0,1,3,3,3,3,1,0,
        1,3,2,3,3,2,3,1,
        1,3,3,3,3,3,3,1,
        1,2,3,3,3,3,2,1,
        0,1,1,0,0,1,1,0,
        0,1,0,0,0,0,1,0,
        0,0,0,0,0,0,0,0,
    },
    {
        0,0,1,1,1,1,0,0,
        0,1,2,2,2,2,1,0,
        1,2,3,2,3,2,2,1,
        1,2,2,2,2,2,2,1,
        0,1,2,2,2,2,1,0,
        0,0,1,1,1,1,0,0,
        0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,
    },
    {
        0,0,0,0,0,0,0,0,
        0,0,1,1,1,1,0,0,
        0,1,2,2,2,2,1,0,
        0,1,2,3,3,2,1,0,
        0,0,1,2,2,1,0,0,
        0,0,0,1,1,0,0,0,
        0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,
    },
};

static const uint32_t enemy_palette[4] = {
    0,
    DOOM_COLOR(120, 72, 48),
    DOOM_COLOR(160, 100, 64),
    DOOM_COLOR(200, 60, 40),
};

static const uint8_t pistol_frames[DOOM_PISTOL_FRAMES][64] = {
    {
        0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,
        0,0,0,1,1,0,0,0,
        0,0,1,2,2,1,0,0,
        0,1,2,2,2,2,1,0,
        0,1,2,2,2,2,1,0,
        0,0,1,1,1,1,0,0,
        0,0,0,0,0,0,0,0,
    },
    {
        0,0,0,0,0,0,0,0,
        0,0,0,0,1,1,0,0,
        0,0,0,1,3,3,1,0,
        0,0,1,2,2,2,2,1,
        0,1,2,2,2,2,2,1,
        0,1,2,2,2,2,1,0,
        0,0,1,1,1,1,0,0,
        0,0,0,0,0,0,0,0,
    },
};

static const uint8_t shotgun_frames[DOOM_SHOTGUN_FRAMES][64] = {
    {
        0,0,0,0,0,0,0,0,
        0,0,0,1,1,1,0,0,
        0,0,1,2,2,2,1,0,
        0,1,2,2,2,2,2,1,
        0,1,2,2,2,2,2,1,
        0,0,1,2,2,2,1,0,
        0,0,0,1,1,1,0,0,
        0,0,0,0,0,0,0,0,
    },
    {
        0,0,0,0,1,1,0,0,
        0,0,0,1,3,3,1,0,
        0,0,1,2,2,2,2,1,
        0,1,2,2,2,2,2,2,
        0,1,2,2,2,2,2,1,
        0,0,1,2,2,2,1,0,
        0,0,0,1,1,1,0,0,
        0,0,0,0,0,0,0,0,
    },
};

static const uint32_t pistol_palette[4] = {
    0,
    DOOM_COLOR(48, 48, 52),
    DOOM_COLOR(80, 80, 88),
    DOOM_COLOR(255, 220, 100),
};

const uint8_t *doom_tex_wall(int tex_id) {
    switch (tex_id) {
    case 1: return wall_metal;
    case 2: return wall_blood;
    default: return wall_stone;
    }
}

const uint8_t *doom_tex_flat(int tex_id) {
    switch (tex_id) {
    case 1: return flat_tile;
    case 2: return flat_plate;
    default: return flat_stone;
    }
}

uint32_t doom_tex_wall_color(int tex_id, int u, int v) {
    const uint8_t *t = doom_tex_wall(tex_id);
    uint8_t idx = t[(v & 7) * 8 + (u & 7)];
    return wall_palette[idx & 3];
}

uint32_t doom_tex_flat_color(int tex_id, int u, int v) {
    const uint8_t *t = doom_tex_flat(tex_id);
    uint8_t idx = t[(v & 7) * 8 + (u & 7)];
    return flat_palette[idx & 3];
}

uint32_t doom_tex_enemy_color(int frame, int u, int v) {
    uint8_t idx = enemy_frames[frame & 3][(v & 7) * 8 + (u & 7)];
    return enemy_palette[idx & 3];
}

uint32_t doom_tex_pistol_color(int frame, int u, int v) {
    uint8_t idx = pistol_frames[frame & 1][(v & 7) * 8 + (u & 7)];
    return pistol_palette[idx & 3];
}

uint32_t doom_tex_shotgun_color(int frame, int u, int v) {
    uint8_t idx = shotgun_frames[frame & 1][(v & 7) * 8 + (u & 7)];
    return pistol_palette[idx & 3];
}

uint32_t doom_tex_pickup_color(doom_pickup_type_t type, int u, int v) {
    int cx = 3, cy = 3;
    int dx = (u & 7) - cx;
    int dy = (v & 7) - cy;
    if (dx * dx + dy * dy > 10) return 0;
    switch (type) {
    case DOOM_PICKUP_AMMO:   return DOOM_COLOR(200, 180, 40);
    case DOOM_PICKUP_SHELLS: return DOOM_COLOR(180, 140, 60);
    case DOOM_PICKUP_HEALTH: return DOOM_COLOR(40, 180, 40);
    case DOOM_PICKUP_KEY_RED:  return DOOM_COLOR(200, 40, 40);
    case DOOM_PICKUP_KEY_BLUE: return DOOM_COLOR(40, 80, 200);
    }
    return 0;
}
