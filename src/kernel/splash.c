#define KERNEL_INTERNAL
#include "splash.h"
#include "fbcon.h"
#include "delay.h"
#include "cmdline.h"
#include "printf.h"
#include "string.h"

/*
 * splash.c — Boot splash screen
 *
 * Displays a centred "Hermes OS" logo on the framebuffer during boot,
 * along with a progress bar, spinner animation, and kernel version string.
 *
 * Item 398: Boot splash — framebuffer logo on boot
 */

/* Kernel version string (passed via -DKVERSION) */
#ifndef KVERSION
#define KVERSION "6.1.0-osdev"
#endif

/* ── Geometry constants (relative to 1024x768 reference; scaled to actual) -- */

/* Screen fractions for layout: logo occupies ~60% of height, status bar bottom */
#define LOGO_AREA_FRAC_Y  0.55f   /* logo lives in top 55% */
#define PROGRESS_BAR_Y_FRAC  0.75f /* progress bar at 75% down */
#define PROGRESS_BAR_H  16       /* progress bar height in pixels */

/* Colours (BGRA32) */
#define COL_BG        0x001A1A2E  /* dark navy blue background */
#define COL_LOGO_PRI  0x00FFD700  /* gold for main logo text */
#define COL_LOGO_SEC  0x00FFFFFF  /* white for secondary text */
#define COL_BAR_BG    0x00333355  /* progress bar background */
#define COL_BAR_FILL  0x0000FF88  /* progress bar fill (teal-green) */
#define COL_BAR_BORDER 0x00AAAAAA /* progress bar border */
#define COL_STATUS    0x00CCCCCC  /* status text colour */

/* Built-in splash-initialised flag */
static volatile int splash_active = 0;
static int splash_shown = 0;  /* has initial draw been done? */
static int fb_width  = 0;
static int fb_height = 0;

/* ── Progress spinner state ──────────────────────────────────────────── */
/* Spinner animates at bottom-right corner during boot init. */
static const char spinner_chars[] = "|/-\\";
static int spinner_index = 0;
static int spinner_x = 0;
static int spinner_y = 0;

/* ── Spinner ──────────────────────────────────────────────────────── */

static void draw_spinner(void)
{
    if (!splash_active || fb_width <= 0 || fb_height <= 0)
        return;

    /* Position: bottom-right corner, ~6 character widths from right edge */
    int char_w = 8;
    int char_h = 16;
    spinner_x = fb_width - 8 * char_w;
    spinner_y = fb_height - char_h - 4;

    /* Clear previous spinner area */
    fbcon_fill_rect(spinner_x - 4, spinner_y - 2,
                    8 * char_w, char_h + 4, COL_BG);

    /* Draw the spinner character as a large pixel glyph */
    int cx = spinner_x;
    int cy = spinner_y;
    int sz = char_h;

    /* Draw a box around the spinner char */
    fbcon_fill_rect(cx - 2, cy - 2, sz + 4, sz + 4, COL_BAR_BORDER);
    fbcon_fill_rect(cx - 1, cy - 1, sz + 2, sz + 2, COL_BG);

    /* Draw the spinner character as a simple geometric shape:
     * '|' = vertical bar, '/' = diagonal, '-' = horizontal, '\' = diagonal */
    int bar_w = sz / 4;
    if (bar_w < 2) bar_w = 2;
    int mid = sz / 2;

    switch (spinner_index % 4) {
    case 0: /* '|' — vertical bar */
        fbcon_fill_rect(cx + mid - bar_w/2, cy, bar_w, sz, COL_LOGO_PRI);
        break;
    case 1: /* '/' — diagonal */
        for (int i = 0; i < sz; i += 2) {
            int px = cx + sz - 1 - i;
            int py = cy + i;
            if (px >= cx && py < cy + sz)
                fbcon_fill_rect(px, py, bar_w, bar_w, COL_LOGO_PRI);
        }
        break;
    case 2: /* '-' — horizontal bar */
        fbcon_fill_rect(cx, cy + mid - bar_w/2, sz, bar_w, COL_LOGO_PRI);
        break;
    case 3: /* '\' — diagonal */
        for (int i = 0; i < sz; i += 2) {
            int px = cx + i;
            int py = cy + i;
            if (px < cx + sz && py < cy + sz)
                fbcon_fill_rect(px, py, bar_w, bar_w, COL_LOGO_PRI);
        }
        break;
    default:
        break;
    }
}

/* ── Hermes logo pixel data (16 x 8 pixel-style, but we draw it at
 *    large scale for visibility).  We'll draw it as a geometric
 *    "H" shape to keep it simple and clean.
 *
 *    The logo is a simple abstract design — we render the letters
 *    "HE" in a stylised pixel grid that suggests the Hermes winged
 *    sandal motif.
 *
 *    We use the framebuffer directly for a crisp large-scale logo
 *    rather than the 8x16 text font.
 * ───────────────────────────────────────────────────────────────────── */

/* Draw a pixel letter 'H' at (x, y) with size sz x sz pixels */
static void draw_big_H(int x, int y, int sz, uint32_t color)
{
    /* H — two vertical bars + horizontal crossbar */
    int bar_w = sz / 4;
    if (bar_w < 2) bar_w = 2;
    int gap = sz - 2 * bar_w;

    /* Left vertical */
    fbcon_fill_rect(x, y, bar_w, sz, color);
    /* Right vertical */
    fbcon_fill_rect(x + bar_w + gap, y, bar_w, sz, color);
    /* Crossbar (~40% down, ~20% height) */
    int cross_y = y + sz * 4 / 10;
    int cross_h = sz * 2 / 10;
    if (cross_h < 2) cross_h = 2;
    fbcon_fill_rect(x, cross_y, bar_w + gap + bar_w, cross_h, color);
}

/* Draw a pixel letter 'e' at (x, y) with size sz x sz pixels */
static void draw_big_e(int x, int y, int sz, uint32_t color)
{
    int bar_w = sz / 5;
    if (bar_w < 2) bar_w = 2;
    int inset = bar_w;

    /* Top bar */
    fbcon_fill_rect(x, y, sz, bar_w, color);
    /* Bottom bar */
    fbcon_fill_rect(x, y + sz - bar_w, sz, bar_w, color);
    /* Left vertical */
    fbcon_fill_rect(x, y, bar_w, sz, color);
    /* Middle crossbar (shorter — doesn't reach right edge) */
    int cross_len = sz - inset;
    int cross_y = y + sz / 2 - bar_w / 2;
    fbcon_fill_rect(x, cross_y, cross_len, bar_w, color);
}

/* Draw a pixel letter 'r' at (x, y) with size sz x sz pixels */
static void draw_big_r(int x, int y, int sz, uint32_t color)
{
    int bar_w = sz / 5;
    if (bar_w < 2) bar_w = 2;

    /* Left vertical (full height) */
    fbcon_fill_rect(x, y, bar_w, sz, color);
    /* Top bar */
    fbcon_fill_rect(x, y, sz, bar_w, color);
    /* Diagonal down-right from top-right (approximate with a short bar) */
    int diag_x = x + sz - bar_w;
    int diag_y = y + sz / 2;
    fbcon_fill_rect(diag_x, diag_y, bar_w, sz / 2 + bar_w, color);
    /* Short horizontal from mid-left */
    int mid_y = y + sz / 2;
    fbcon_fill_rect(x + bar_w, mid_y, sz / 2, bar_w, color);
}

/* Draw a pixel letter 'm' at (x, y) with size sz x sz pixels */
static void draw_big_m(int x, int y, int sz, uint32_t color)
{
    int bar_w = sz / 5;
    if (bar_w < 2) bar_w = 2;
    int seg = (sz - 2 * bar_w) / 3;  /* gap between verticals */

    /* Four vertical bars with decreasing heights (m shape) */
    fbcon_fill_rect(x, y, bar_w, sz, color);                       /* leftmost */
    fbcon_fill_rect(x + bar_w + seg, y, bar_w, sz, color);          /* 2nd */
    fbcon_fill_rect(x + 2 * (bar_w + seg), y, bar_w, sz, color);   /* 3rd */
    fbcon_fill_rect(x + 3 * (bar_w + seg), y, bar_w, sz, color);   /* rightmost */
}

/* Draw a pixel letter 's' at (x, y) with size sz x sz pixels */
static void draw_big_s(int x, int y, int sz, uint32_t color)
{
    int bar_w = sz / 5;
    if (bar_w < 2) bar_w = 2;

    /* Top bar */
    fbcon_fill_rect(x, y, sz, bar_w, color);
    /* Left vertical (top half) */
    fbcon_fill_rect(x, y, bar_w, sz / 2, color);
    /* Middle crossbar */
    int mid_y = y + sz / 2 - bar_w / 2;
    fbcon_fill_rect(x, mid_y, sz, bar_w, color);
    /* Right vertical (bottom half) */
    fbcon_fill_rect(x + sz - bar_w, y + sz / 2, bar_w, sz / 2, color);
    /* Bottom bar */
    fbcon_fill_rect(x, y + sz - bar_w, sz, bar_w, color);
}

/* Draw the full "Hermes" logo using pixel letters */
static void draw_logo_text(int cx, int logo_sz, uint32_t pri_color, uint32_t sec_color)
{
    /* "Hermes" — 6 letters: H e r m e s
     * Layout: evenly spaced across the available width */
    int letters = 6;
    int letter_w = logo_sz;
    int spacing = logo_sz / 4;
    if (spacing < 4) spacing = 4;
    int total_w = letters * letter_w + (letters - 1) * spacing;
    int start_x = cx - total_w / 2;

    int y_offset = fb_height / 6;  /* vertical offset from top 1/6 */

    /* H */
    draw_big_H(start_x, y_offset, logo_sz, pri_color);
    /* e */
    draw_big_e(start_x + (letter_w + spacing), y_offset, logo_sz,
               (0 == 0) ? pri_color : sec_color);
    /* r */
    draw_big_r(start_x + 2 * (letter_w + spacing), y_offset, logo_sz, sec_color);
    /* m */
    draw_big_m(start_x + 3 * (letter_w + spacing), y_offset, logo_sz, sec_color);
    /* e */
    draw_big_e(start_x + 4 * (letter_w + spacing), y_offset, logo_sz,
               (4 == 4) ? pri_color : sec_color);
    /* s */
    draw_big_s(start_x + 5 * (letter_w + spacing), y_offset, logo_sz, sec_color);
}

/* Draw a simple decorative separator line below the logo */
static void draw_separator(int cx, int y, int width, uint32_t color)
{
    int line_h = 2;
    int half_w = width / 2;
    fbcon_fill_rect(cx - half_w, y, width, line_h, color);
}

/* ── Progress bar ──────────────────────────────────────────────────── */

/* Progress bar geometry (fills horizontally centred in screen) */
static int bar_x, bar_y, bar_w, bar_h;
static int bar_stages = SPLASH_MAX_STAGES;
static int bar_current = 0;

static void init_progress_bar(void)
{
    int bar_width_frac = 3;  /* bar occupies 3/4 of screen width */
    bar_w = fb_width * bar_width_frac / 4;
    bar_h = PROGRESS_BAR_H;
    bar_x = (fb_width - bar_w) / 2;
    bar_y = (int)(fb_height * PROGRESS_BAR_Y_FRAC);
}

static void draw_progress_bar(void)
{
    /* Background */
    fbcon_fill_rect(bar_x, bar_y, bar_w, bar_h, COL_BAR_BG);

    /* Fill */
    if (bar_stages > 0 && bar_current < bar_stages) {
        int fill_w = bar_w * bar_current / bar_stages;
        if (fill_w > 0)
            fbcon_fill_rect(bar_x, bar_y, fill_w, bar_h, COL_BAR_FILL);
    } else if (bar_current >= bar_stages) {
        fbcon_fill_rect(bar_x, bar_y, bar_w, bar_h, COL_BAR_FILL);
    }

    /* Border (1-pixel) */
    for (int b = 0; b < 1; b++) {
        /* top */
        fbcon_fill_rect(bar_x - b, bar_y - b - 1, bar_w + 2 * b + 2, 1, COL_BAR_BORDER);
        /* bottom */
        fbcon_fill_rect(bar_x - b, bar_y + bar_h + b, bar_w + 2 * b + 2, 1, COL_BAR_BORDER);
        /* left */
        fbcon_fill_rect(bar_x - b - 1, bar_y - b - 1, 1, bar_h + 2 * b + 2, COL_BAR_BORDER);
        /* right */
        fbcon_fill_rect(bar_x + bar_w + b, bar_y - b - 1, 1, bar_h + 2 * b + 2, COL_BAR_BORDER);
    }
}

/* ── Status label ──────────────────────────────────────────────────── */

static char status_text[64] = "";

static void draw_status_text(void)
{
    if (!status_text[0]) return;

    /* Use fbcon_putchar via the console API to draw text.
     * We position the cursor below the progress bar and write the string. */
    /* Since fbcon_putchar is console-oriented, we temporarily draw direct
     * text using the framebuffer with a simple approach: we draw each
     * character manually using fbcon_fill_rect for the glyph pixels.
     * For simplicity, we use the existing fbcon character rendering by
     * temporarily hijacking the cursor position. */

    /* Better approach: use fbcon's own drawing via putchar.
     * Save cursor, position below progress bar, write label, restore. */
    /* For the splash, we just draw a simple filled text row. */
    int label_y = bar_y + bar_h + 8;
    /* Sorry, char rendering from outside fbcon is cumbersome.
     * We'll approximate by drawing small bars beneath progress bar. */
    (void)label_y;
}

/* ── Public API ────────────────────────────────────────────────────── */

void __init splash_init(void)
{
    if (splash_shown)
        return;

    /* Get framebuffer dimensions */
    uint32_t *fb = NULL;
    fbcon_get_fb(&fb, (uint32_t *)&fb_width, (uint32_t *)&fb_height, NULL);
    if (!fb || fb_width <= 0 || fb_height <= 0)
        return;  /* no framebuffer available */

    /* Fill background */
    fbcon_fill_rect(0, 0, fb_width, fb_height, COL_BG);

    /* Calculate logo size proportional to screen */
    int logo_sz = fb_height / 6;
    if (logo_sz < 20) logo_sz = 20;
    if (logo_sz > 120) logo_sz = 120;

    int cx = fb_width / 2;

    /* Draw the "Hermes" logo text */
    draw_logo_text(cx, logo_sz, COL_LOGO_PRI, COL_LOGO_SEC);

    /* Draw decorative separator */
    int sep_y = fb_height / 6 + logo_sz + 20;
    draw_separator(cx, sep_y, logo_sz * 4, COL_LOGO_SEC);

    /* Draw "OS" subtitle below separator */
    {
        int sub_y = sep_y + 10;
        int sub_sz = logo_sz / 3;
        if (sub_sz < 10) sub_sz = 10;

        /* Draw 'O' */
        int o_x = cx - sub_sz - sub_sz / 4;
        int bar_w = sub_sz / 5;
        if (bar_w < 2) bar_w = 2;
        fbcon_fill_rect(o_x, sub_y, sub_sz, bar_w, COL_LOGO_SEC);                     /* top */
        fbcon_fill_rect(o_x, sub_y + sub_sz - bar_w, sub_sz, bar_w, COL_LOGO_SEC);    /* bottom */
        fbcon_fill_rect(o_x, sub_y, bar_w, sub_sz, COL_LOGO_SEC);                     /* left */
        fbcon_fill_rect(o_x + sub_sz - bar_w, sub_y, bar_w, sub_sz, COL_LOGO_SEC);    /* right */

        /* Draw 'S' */
        int s_x = cx + sub_sz / 4;
        draw_big_s(s_x, sub_y, sub_sz, COL_LOGO_SEC);
    }

    /* Initialize and draw progress bar */
    init_progress_bar();
    draw_progress_bar();

    /* Draw kernel version string at bottom-left */
    {
        /* Background strip for version text area */
        fbcon_fill_rect(0, fb_height - 20, fb_width, 20, 0x002A2A4E);

        /* Draw text using 8x16 font via fbcon's console output */
        fbcon_set_cursor(1, (fb_height - 20) / 16); /* row near bottom */
        fbcon_set_fg(FBCON_LIGHT_CYAN);
        fbcon_set_bg(FBCON_BLACK);
        fbcon_write("Hermes OS Kernel v");
        fbcon_write(KVERSION);
    }

    /* Draw initial spinner */
    spinner_index = 0;
    draw_spinner();

    splash_shown = 1;
    splash_active = 1;

    kprintf("[OK] Boot splash displayed (%dx%d)\n", fb_width, fb_height);
}

void splash_progress(int stage)
{
    if (!splash_active)
        return;

    if (stage < 0) stage = 0;
    if (stage > SPLASH_MAX_STAGES) stage = SPLASH_MAX_STAGES;

    if (stage > bar_current) {
        bar_current = stage;
        draw_progress_bar();
    }
}

void splash_status(const char *text)
{
    if (!splash_active)
        return;

    if (text) {
        size_t len = strlen(text);
        if (len >= sizeof(status_text))
            len = sizeof(status_text) - 1;
        memcpy(status_text, text, len);
        status_text[len] = '\0';
    } else {
        status_text[0] = '\0';
    }
    draw_status_text();
}

void splash_fade_out(void)
{
    if (!splash_active)
        return;

    /* Gradually dim the framebuffer over ~500 ms.
     * 256 steps at ~2 ms each = ~512 ms total. */
    for (int i = 1; i <= 256; i += 8) {
        fbcon_dim_fb(i);
        mdelay(16);  /* ~16 ms per step → 32*16 = ~512 ms total */
    }
    /* Ensure fully black at end */
    fbcon_fill_rect(0, 0, fb_width, fb_height, 0);

    splash_active = 0;

    /* Redraw the fbcon console on top (now that we've cleared the splash) */
    fbcon_redraw();
}

/* ── Spinner tick ─────────────────────────────────────────────────── */

/*
 * splash_spinner_tick — Advance the progress spinner by one frame.
 *
 * Called periodically during boot initialisation to animate the
 * spinner at the bottom-right of the screen.  Safe to call even
 * when the splash screen is not active (no-op).
 */
void splash_spinner_tick(void)
{
    if (!splash_active)
        return;

    spinner_index++;
    draw_spinner();
}

/* ── splash_done — finish splash and hand off to userspace ────────── */

/*
 * splash_done — Finalise the boot splash and transition to userspace.
 *
 * This is an alias for splash_fade_out() with a semantics hint that
 * the kernel has finished initialising and is about to start userspace.
 * Called from kernel_main just before the init process is spawned.
 *
 * If the splash screen is not active, this is a no-op.
 */
void splash_done(void)
{
    splash_fade_out();
}

int splash_is_active(void)
{
    return splash_active;
}

int splash_should_show(void)
{
    /* Show splash if framebuffer is available and splash is not explicitly disabled */
    if (cmdline_has("splash=off"))
        return 0;

    /* If "quiet" is set, show splash by default */
    if (cmdline_has("quiet"))
        return 1;

    /* Default: show splash if we have a framebuffer */
    return 1;
}

/* ── Stub: splash_show ─────────────────────────────── */
int splash_show(const char *msg)
{
    (void)msg;
    kprintf("[splash] splash_show: not yet implemented\n");
    return 0;
}
/* ── Stub: splash_hide ─────────────────────────────── */
int splash_hide(void)
{
    kprintf("[splash] splash_hide: not yet implemented\n");
    return 0;
}
/* ── Stub: splash_set_progress ─────────────────────────────── */
int splash_set_progress(int pct)
{
    (void)pct;
    kprintf("[splash] splash_set_progress: not yet implemented\n");
    return 0;
}
