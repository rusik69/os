#ifndef DOOM_H
#define DOOM_H

#include "types.h"

#define DOOM_SCREEN_W   320
#define DOOM_SCREEN_H   200
#define DOOM_VIEW_H     168
#define DOOM_STATUS_H   32
#define DOOM_MAP_SIZE   32
#define DOOM_FRAC       256
#define DOOM_ANGLE_UNITS 4096
#define DOOM_MAX_ENEMIES 8
#define DOOM_MAX_PICKUPS 8
#define DOOM_MAX_SPRITES (DOOM_MAX_ENEMIES + DOOM_MAX_PICKUPS)
#define DOOM_MAX_DOORS  8
#define DOOM_MAX_SECTORS 8
#define DOOM_TEX_SIZE   8
#define DOOM_WALL_TEX   3
#define DOOM_ENEMY_FRAMES 4
#define DOOM_PISTOL_FRAMES 2
#define DOOM_SHOTGUN_FRAMES 2

#define DOOM_KEY_RED  1
#define DOOM_KEY_BLUE 2

#define DOOM_COLOR(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define DOOM_R(c) (((c) >> 16) & 0xFF)
#define DOOM_G(c) (((c) >> 8) & 0xFF)
#define DOOM_B(c) ((c) & 0xFF)

static inline uint32_t doom_scale_rgb(uint32_t c, int num, int den) {
    if (den <= 0) den = 1;
    return DOOM_COLOR((DOOM_R(c) * num) / den,
                      (DOOM_G(c) * num) / den,
                      (DOOM_B(c) * num) / den);
}

static inline uint32_t doom_lerp_rgb(uint32_t a, uint32_t b, int num, int den) {
    if (den <= 0) den = 1;
    int inv = den - num;
    return DOOM_COLOR((DOOM_R(a) * inv + DOOM_R(b) * num) / den,
                      (DOOM_G(a) * inv + DOOM_G(b) * num) / den,
                      (DOOM_B(a) * inv + DOOM_B(b) * num) / den);
}

typedef struct {
    int8_t  floor_h;
    int8_t  ceil_h;
    uint8_t floor_tex;
    uint8_t ceil_tex;
    uint8_t light;
} doom_sector_t;

typedef struct {
    uint8_t wall_tex;
    uint8_t sector;
} doom_cell_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t angle;
    int health;
    int ammo;
    int shells;
    int keys;
    int weapon;
    int moving;
    int walk_phase;
} doom_player_t;

typedef enum {
    DOOM_ENEMY_IDLE = 0,
    DOOM_ENEMY_CHASE,
    DOOM_ENEMY_DEAD
} doom_enemy_state_t;

typedef struct {
    int active;
    int32_t x;
    int32_t y;
    int health;
    int frame;
    int attack_cd;
    doom_enemy_state_t state;
} doom_enemy_t;

typedef enum {
    DOOM_PICKUP_AMMO = 0,
    DOOM_PICKUP_SHELLS,
    DOOM_PICKUP_HEALTH,
    DOOM_PICKUP_KEY_RED,
    DOOM_PICKUP_KEY_BLUE
} doom_pickup_type_t;

typedef struct {
    int active;
    int32_t x;
    int32_t y;
    doom_pickup_type_t type;
} doom_pickup_t;

typedef struct {
    int active;
    int mx;
    int my;
    int open_frac;
    int need_key;
    int switch_id;
} doom_door_t;

typedef struct {
    doom_cell_t cells[DOOM_MAP_SIZE][DOOM_MAP_SIZE];
    doom_sector_t sectors[DOOM_MAX_SECTORS];
    int sector_count;
    doom_door_t doors[DOOM_MAX_DOORS];
    int door_count;
    doom_player_t player;
    doom_enemy_t enemies[DOOM_MAX_ENEMIES];
    int enemy_count;
    doom_pickup_t pickups[DOOM_MAX_PICKUPS];
    int pickup_count;
    int quit;
    int fire_cooldown;
    int pistol_frame;
    int muzzle_flash;
    int won;
    int spawn_grace;
    int damage_flash;
    int switch_state;
} doom_state_t;

extern doom_state_t g_doom;

/* doom_textures.c */
const uint8_t *doom_tex_wall(int tex_id);
const uint8_t *doom_tex_flat(int tex_id);
uint32_t doom_tex_wall_color(int tex_id, int u, int v);
uint32_t doom_tex_flat_color(int tex_id, int u, int v);
uint32_t doom_tex_enemy_color(int frame, int u, int v);
uint32_t doom_tex_pistol_color(int frame, int u, int v);
uint32_t doom_tex_shotgun_color(int frame, int u, int v);
uint32_t doom_tex_pickup_color(doom_pickup_type_t type, int u, int v);

/* doom_math.c */
void doom_math_init(void);
int32_t doom_sin(int32_t angle);
int32_t doom_cos(int32_t angle);
int32_t doom_atan2(int32_t dy, int32_t dx);

/* doom_map.c */
void doom_map_init(doom_state_t *st);
int doom_map_is_wall(const doom_state_t *st, int mx, int my);
uint8_t doom_map_tex(const doom_state_t *st, int mx, int my);
const doom_sector_t *doom_map_sector(const doom_state_t *st, int mx, int my);
int doom_map_collide(const doom_state_t *st, int32_t x, int32_t y);
void doom_map_update(doom_state_t *st);

/* doom_player.c */
void doom_player_init(doom_player_t *p);
void doom_player_update(doom_state_t *st, int mouse_dx);
void doom_player_use(doom_state_t *st);

/* doom_raycast.c */
typedef struct {
    int hit;
    int32_t dist;
    int side;
    int map_x;
    int map_y;
    uint8_t tex;
    int32_t wall_x;
    int32_t perp_dist;
    int8_t floor_h;
    int8_t ceil_h;
} doom_ray_hit_t;

uint32_t doom_apply_fog(uint32_t color, int32_t dist);
void doom_raycast(const doom_state_t *st, int32_t angle, doom_ray_hit_t *out);
void doom_raycast_column(const doom_state_t *st, int col, int32_t fov,
                         uint32_t *fb, int32_t *depth);

/* doom_floor.c */
void doom_floor_column(const doom_state_t *st, int col, int32_t fov,
                       int wall_top, int wall_bot, uint32_t *fb);

/* doom_doors.c */
void doom_doors_init(doom_state_t *st);
void doom_doors_update(doom_state_t *st);
void doom_doors_try_open(doom_state_t *st);
int doom_doors_is_wall(const doom_state_t *st, int mx, int my);
int doom_test_door_opens(void);

/* doom_sprites.c */
typedef struct {
    int type;
    int32_t x;
    int32_t y;
    int32_t dist;
    int frame;
    doom_pickup_type_t pickup_type;
} doom_sprite_t;

#define DOOM_SPRITE_ENEMY  0
#define DOOM_SPRITE_PICKUP 1

void doom_sprites_collect(const doom_state_t *st, doom_sprite_t *out, int *count);
void doom_sprites_draw(const doom_state_t *st, const doom_sprite_t *sprites, int count,
                       uint32_t *fb, const int32_t *depth);
void doom_draw_weapon(const doom_state_t *st, uint32_t *fb);

/* doom_combat.c */
void doom_combat_update(doom_state_t *st);
void doom_combat_fire(doom_state_t *st);
void doom_combat_switch_weapon(doom_state_t *st, int weapon);

/* doom_render.c */
void doom_render_frame(doom_state_t *st);
void doom_render_blit(void);

/* doom_task.c */
void doom_task(void);
void doom_init(void);
void doom_shutdown(void);
void doom_poll_input(doom_state_t *st);
void doom_update(doom_state_t *st);

/* Unit-test helpers */
int doom_test_ray_hit(void);
int doom_test_collision(void);
int doom_test_trig(void);
int doom_test_column_has_sky(void);
int doom_test_column_has_wall(void);
int doom_test_frame_varies(void);

#endif
