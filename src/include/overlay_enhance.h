#ifndef OVERLAY_ENHANCE_H
#define OVERLAY_ENHANCE_H

/* overlay_enhance.c — Whiteout and opaque directory support for overlay FS
 *
 * C17: Whiteout entries — hide files from lower layers
 * C18: Opaque directories — prevent readdir from merging lower layers
 */

/* ── Initialisation ─────────────────────────────────────────────────── */

void overlay_enhance_init(void);

/* ── Whiteout API (C17) ─────────────────────────────────────────────── */

/* Create a whiteout entry, hiding 'name' in the overlay at 'mountpoint' */
int overlay_whiteout_create(const char *mountpoint, const char *name);

/* Check whether 'name' is whiteouted in the overlay at 'mountpoint' */
int overlay_whiteout_check(const char *mountpoint, const char *name);

/* Remove a whiteout entry */
int overlay_whiteout_remove(const char *mountpoint, const char *name);

/* ── Opaque directory API (C18) ─────────────────────────────────────── */

/* Mark a directory as opaque, preventing layer merging */
int overlay_opaque_set(const char *mountpoint, const char *dir_path);

/* Check whether a directory is opaque */
int overlay_opaque_check(const char *mountpoint, const char *dir_path);

/* Clear the opaque flag on a directory */
int overlay_opaque_clear(const char *mountpoint, const char *dir_path);

#endif /* OVERLAY_ENHANCE_H */
