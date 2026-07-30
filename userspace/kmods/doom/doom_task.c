/**
 * doom_task.c — DOOM game module for Hermes OS (kernel-mode task version)
 *
 * Architecture overview:
 * ----------------------
 * This file implements the kernel-mode DOOM game module, providing a
 * full first-person shooter experience running inside the kernel as a
 * schedulable task. It bridges the DOOM engine (map, renderer, combat,
 * player state from the shared doom_*.c sources) with kernel services
 * (VGA framebuffer, keyboard/mouse input, process lifecycle, scheduler).
 *
 * Key responsibilities:
 *   - doom_init():   Initialize math tables, map, and VGA framebuffer.
 *   - doom_task():   Main game loop — polls input, updates game state,
 *                    renders frames, and yields the CPU each iteration.
 *   - doom_shutdown(): Clean up input state and restore VGA console.
 *   - doom_poll_input(): Read keyboard and mouse, dispatch fire/quit/move.
 *   - doom_update():  Advance map and combat simulation one tick.
 *
 * Module lifecycle (when built as a kernel module):
 *   - init_module()    → calls doom_init()
 *   - cleanup_module() → calls doom_shutdown()
 *
 * The module exports MODULE_LICENSE, MODULE_AUTHOR, MODULE_DESCRIPTION,
 * and MODULE_VERSION for module metadata tooling (lsmod, modinfo).
 *
 * Controls:
 *   WASD    — move / turn
 *   Mouse   — turn (left/right)
 *   LMB     — fire weapon
 *   1       — pistol
 *   2       — shotgun
 *   Space/E — use door
 *   Q/ESC   — quit
 *
 * Dependencies: doom.h (map/combat/player/render), vga.h, keyboard.h,
 *               mouse.h, scheduler.h, process.h, printf.h, module.h.
 */
#include "doom.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "scheduler.h"
#include "process.h"
#include "printf.h"
#include "module.h"

doom_state_t g_doom;

static int g_prev_mx = 512;
static int g_mouse_inited = 0;

void doom_init(void) {
    doom_math_init();
    if (!vga_is_framebuffer())
        vga_try_alloc_software_framebuffer();
    doom_map_init(&g_doom);
    g_doom.quit = 0;
    g_doom.won = 0;
    g_mouse_inited = 0;
    vga_clear_framebuffer(DOOM_COLOR(10, 10, 10));
}

void doom_shutdown(void) {
    keyboard_reset_state();
    vga_refresh_console();
}

void doom_poll_input(doom_state_t *st) {
    if (keyboard_escape_down()) {
        st->quit = 1;
        return;
    }

    while (keyboard_has_input()) {
        char c = keyboard_getchar();
        if (c == 27) st->quit = 1;
        (void)c;
    }

    int mx, my;
    mouse_get_pixel_pos(&mx, &my);
    if (!g_mouse_inited) {
        g_prev_mx = mx;
        g_mouse_inited = 1;
    }
    int mouse_dx = mx - g_prev_mx;
    g_prev_mx = mx;
    (void)my;

    uint8_t buttons = mouse_get_buttons();
    static uint8_t prev_buttons = 0;
    if ((buttons & 1) && !(prev_buttons & 1))
        doom_combat_fire(st);
    prev_buttons = buttons;

    doom_player_update(st, mouse_dx);

    if (keyboard_is_down('q') || keyboard_is_down('Q'))
        st->quit = 1;
}

void doom_update(doom_state_t *st) {
    doom_map_update(st);
    doom_combat_update(st);
}

void doom_task(void) {
    doom_init();
    kprintf("[doom] Click QEMU window. WASD move/turn, mouse turn, LMB fire.\n");
    kprintf("[doom] 1=pistol 2=shotgun, Space/E=use door, Q/ESC=quit.\n");
    while (!g_doom.quit) {
        doom_poll_input(&g_doom);
        doom_update(&g_doom);
        doom_render_frame(&g_doom);
        doom_render_blit();
        scheduler_yield();
        if (g_doom.won) {
            for (int i = 0; i < 120 && !g_doom.quit; i++) {
                doom_poll_input(&g_doom);
                doom_render_frame(&g_doom);
                doom_render_blit();
                scheduler_yield();
            }
            g_doom.quit = 1;
        }
    }
    doom_shutdown();
    kprintf("[doom] Game ended. Health=%d\n", g_doom.player.health);
    process_exit();
}

#ifdef MODULE
/* Module entry/exit points — the ELF loader looks for these symbols */
int init_module(void) {
    doom_init();
    return 0;
}
void cleanup_module(void) {
    doom_shutdown();
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("DOOM game — first-person shooter running inside the kernel");
MODULE_VERSION("1.0");
#endif /* MODULE */
