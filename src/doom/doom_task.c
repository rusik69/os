#include "doom.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "scheduler.h"
#include "process.h"
#include "printf.h"

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
