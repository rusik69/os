#include "doom.h"
#include "speaker.h"

#define ENEMY_SPEED   (DOOM_FRAC / 16)
#define ENEMY_ATTACK  5
#define ENEMY_ATTACK_CD 25
#define PISTOL_COOLDOWN 15
#define SHOTGUN_COOLDOWN 30
#define HITSCAN_RANGE (DOOM_FRAC * 12)

static int enemies_alive(const doom_state_t *st) {
    int n = 0;
    for (int i = 0; i < DOOM_MAX_ENEMIES; i++) {
        if (st->enemies[i].active && st->enemies[i].state != DOOM_ENEMY_DEAD)
            n++;
    }
    return n;
}

static void damage_enemy(doom_state_t *st, int idx, int dmg) {
    doom_enemy_t *e = &st->enemies[idx];
    e->health -= dmg;
    if (e->health <= 0) {
        e->state = DOOM_ENEMY_DEAD;
        e->active = 0;
    }
}

static void fire_hitscan(doom_state_t *st, int32_t angle, int dmg) {
    int32_t cos_a = doom_cos(angle);
    int32_t sin_a = doom_sin(angle);

    int best = -1;
    int32_t best_dist = HITSCAN_RANGE;
    for (int i = 0; i < DOOM_MAX_ENEMIES; i++) {
        const doom_enemy_t *e = &st->enemies[i];
        if (!e->active || e->state == DOOM_ENEMY_DEAD) continue;
        int32_t dx = e->x - st->player.x;
        int32_t dy = e->y - st->player.y;
        int32_t dist = (dx / 16) * (dx / 16) + (dy / 16) * (dy / 16);
        if (dist > best_dist) continue;
        int32_t rel = doom_atan2(dy, dx) - angle;
        rel &= (DOOM_ANGLE_UNITS - 1);
        if (rel > DOOM_ANGLE_UNITS / 2) rel -= DOOM_ANGLE_UNITS;
        if (rel < -DOOM_ANGLE_UNITS / 12 || rel > DOOM_ANGLE_UNITS / 12) continue;
        best = i;
        best_dist = dist;
    }
    if (best >= 0)
        damage_enemy(st, best, dmg);
    (void)cos_a;
    (void)sin_a;
}

void doom_combat_switch_weapon(doom_state_t *st, int weapon) {
    if (weapon == 0) {
        st->player.weapon = 0;
    } else if (weapon == 1 && st->player.shells > 0) {
        st->player.weapon = 1;
    }
}

void doom_combat_fire(doom_state_t *st) {
    if (st->fire_cooldown > 0) return;

    if (st->player.weapon == 0) {
        if (st->player.ammo <= 0) return;
        st->player.ammo--;
        st->fire_cooldown = PISTOL_COOLDOWN;
        st->pistol_frame = 1;
        st->muzzle_flash = 4;
        speaker_beep(800, 30);
        fire_hitscan(st, st->player.angle, 15);
    } else {
        if (st->player.shells <= 0) {
            st->player.weapon = 0;
            return;
        }
        st->player.shells--;
        st->fire_cooldown = SHOTGUN_COOLDOWN;
        st->pistol_frame = 1;
        st->muzzle_flash = 6;
        speaker_beep(400, 50);
        for (int i = -2; i <= 2; i++) {
            int32_t spread = (i * DOOM_ANGLE_UNITS) / 64;
            int32_t ang = (st->player.angle + spread) & (DOOM_ANGLE_UNITS - 1);
            fire_hitscan(st, ang, 12);
        }
    }
}

void doom_combat_update(doom_state_t *st) {
    if (st->fire_cooldown > 0) st->fire_cooldown--;
    if (st->pistol_frame > 0 && st->fire_cooldown < PISTOL_COOLDOWN - 3)
        st->pistol_frame = 0;
    if (st->muzzle_flash > 0) st->muzzle_flash--;
    if (st->spawn_grace > 0) st->spawn_grace--;
    if (st->damage_flash > 0) st->damage_flash--;

    for (int i = 0; i < DOOM_MAX_ENEMIES; i++) {
        doom_enemy_t *e = &st->enemies[i];
        if (!e->active || e->state == DOOM_ENEMY_DEAD) continue;

        if (e->attack_cd > 0) e->attack_cd--;

        int32_t dx = st->player.x - e->x;
        int32_t dy = st->player.y - e->y;
        int32_t dist2 = (dx / 16) * (dx / 16) + (dy / 16) * (dy / 16);

        if (dist2 > (DOOM_FRAC / 2) * (DOOM_FRAC / 2))
            e->state = DOOM_ENEMY_CHASE;
        else
            e->state = DOOM_ENEMY_IDLE;

        if (e->state == DOOM_ENEMY_CHASE) {
            int32_t angle = doom_atan2(dy, dx);
            int32_t nx = e->x + (doom_cos(angle) * ENEMY_SPEED) / 65536;
            int32_t ny = e->y + (doom_sin(angle) * ENEMY_SPEED) / 65536;
            if (!doom_map_collide(st, nx, e->y)) e->x = nx;
            if (!doom_map_collide(st, e->x, ny)) e->y = ny;
            e->frame = (e->frame + 1) % DOOM_ENEMY_FRAMES;
        }

        if (st->spawn_grace == 0 &&
            dist2 < (DOOM_FRAC * 2) * (DOOM_FRAC * 2) &&
            e->attack_cd == 0) {
            st->player.health -= ENEMY_ATTACK;
            st->damage_flash = 8;
            e->attack_cd = ENEMY_ATTACK_CD;
            speaker_beep(200, 50);
        }
    }

    if (st->player.health <= 0)
        st->quit = 1;
    if (enemies_alive(st) == 0)
        st->won = 1;
}
