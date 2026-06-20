#include "doom.h"
#include "keyboard.h"

#define MOVE_SPEED  (DOOM_FRAC / 8)
#define TURN_SPEED  (DOOM_ANGLE_UNITS / 48)
#define MOUSE_SENS  4

void doom_player_init(doom_player_t *p) {
    p->health = 100;
    p->ammo = 50;
    p->shells = 8;
    p->weapon = 0;
    p->keys = 0;
}

static void try_move(doom_state_t *st, int32_t nx, int32_t ny) {
    if (!doom_map_collide(st, nx, st->player.y))
        st->player.x = nx;
    if (!doom_map_collide(st, st->player.x, ny))
        st->player.y = ny;
}

void doom_player_use(doom_state_t *st) {
    doom_doors_try_open(st);
}

void doom_player_update(doom_state_t *st, int mouse_dx) {
    int32_t angle = st->player.angle;
    int32_t cos_a = doom_cos(angle);
    int32_t sin_a = doom_sin(angle);
    int moved = 0;

    if (keyboard_is_down('w') || keyboard_is_down(KEY_UP)) {
        try_move(st,
                 st->player.x + (cos_a * MOVE_SPEED) / 65536,
                 st->player.y + (sin_a * MOVE_SPEED) / 65536);
        moved = 1;
    }
    if (keyboard_is_down('s') || keyboard_is_down(KEY_DOWN)) {
        try_move(st,
                 st->player.x - (cos_a * MOVE_SPEED) / 65536,
                 st->player.y - (sin_a * MOVE_SPEED) / 65536);
        moved = 1;
    }
    if (keyboard_is_down('a') || keyboard_is_down(KEY_LEFT)) {
        st->player.angle = (st->player.angle + TURN_SPEED) & (DOOM_ANGLE_UNITS - 1);
    }
    if (keyboard_is_down('d') || keyboard_is_down(KEY_RIGHT)) {
        st->player.angle = (st->player.angle - TURN_SPEED) & (DOOM_ANGLE_UNITS - 1);
    }

    if (mouse_dx != 0) {
        st->player.angle = (st->player.angle - mouse_dx * MOUSE_SENS) & (DOOM_ANGLE_UNITS - 1);
    }

    if (keyboard_is_down(' ') || keyboard_is_down('e') || keyboard_is_down('E'))
        doom_player_use(st);

    if (keyboard_is_down('1'))
        doom_combat_switch_weapon(st, 0);
    if (keyboard_is_down('2'))
        doom_combat_switch_weapon(st, 1);

    st->player.moving = moved;
    if (moved)
        st->player.walk_phase = (st->player.walk_phase + 1) & 31;
}
