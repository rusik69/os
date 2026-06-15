#ifndef SPLASH_H
#define SPLASH_H

/*
 * splash.h — Boot splash screen
 *
 * Displays a centred "Hermes OS" logo on the framebuffer during boot,
 * along with a progress bar that advances as kernel subsystems initialise.
 *
 * Usage:
 *   splash_init()      — called after fbcon init; draws the logo
 *   splash_progress(i) — advance to stage i (0..SPLASH_MAX_STAGES-1)
 *   splash_fade_out()  — smooth fade transition before userspace
 *
 * Kernel cmdline:
 *   splash=off         — disable splash entirely (normal verbose boot)
 *   quiet              — also enables splash (suppresses [OK] messages)
 */

#include "types.h"

/* Total number of boot stages for progress bar */
#define SPLASH_MAX_STAGES  32

/* ── Public API ───────────────────────────────────────────────── */

/* Initialise the splash screen: clear framebuffer, draw background,
 * logo, and empty progress bar.  Safe to call multiple times (no-op
 * after first).  When the framebuffer is not available this is a
 * silent no-op. */
void splash_init(void);

/* Advance the progress bar to @stage (0-based, clamped to max).
 * Redraws the filled portion of the bar.  If splash_init() has not
 * been called this is a no-op. */
void splash_progress(int stage);

/* Set a text label below the progress bar (e.g. "Initialising VFS").
 * The label is truncated to fit the screen width.  Pass NULL to
 * clear the label. */
void splash_status(const char *text);

/* Smooth fade-out transition: gradually dim the framebuffer from
 * current brightness to black over ~500 ms, then return.  The
 * caller should fbcon_redraw() or print the shell prompt after
 * this. */
void splash_fade_out(void);

/*
 * splash_done — Finalise boot splash and transition to userspace.
 *
 * Alias for splash_fade_out().  Called when kernel init is complete
 * and userspace is about to start.  If splash is not active, no-op.
 */
void splash_done(void);

/*
 * splash_spinner_tick — Advance the progress spinner by one frame.
 *
 * Called periodically during init to animate the spinner at the
 * bottom-right of the screen.  Safe to call when splash is not
 * active (no-op).
 */
void splash_spinner_tick(void);

/* Check whether the splash screen is active.  Returns 1 if splash
 * was initialised and is still displayed, 0 otherwise. */
int splash_is_active(void);

/* Check whether splash should be shown (checks cmdline). */
int splash_should_show(void);

#endif /* SPLASH_H */
