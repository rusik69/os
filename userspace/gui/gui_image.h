#ifndef GUI_IMAGE_H
#define GUI_IMAGE_H

#include <stdint.h>

/* Image decoding for the GUI Image Viewer.
 *
 * These functions are deliberately GUI-agnostic: they operate only on a raw
 * pixel buffer of uint32_t (0xFFRRGGBB) and a byte buffer, so they can be
 * compiled and verified on the host as well as inside the freestanding GUI.
 *
 * Supported formats:
 *   - BMP: uncompressed (BI_RGB) 24- and 32-bit, bottom-up or top-down.
 *   - PNG: color types 0 (gray), 2 (RGB), 3 (palette) and 6 (RGBA),
 *     bit depth 8, compression 0, filter 0, non-interlaced.
 *
 * Both decoders reject out-of-capability inputs (large dims, other bit
 * depths, other color types) rather than silently mis-render.
 */

/* Decode a BMP or PNG image from `data` (of `len` bytes) and scale it to fit
 * within the `fbw` x `fbh` framebuffer, preserving aspect ratio and centering
 * with a light background.  On success writes `status` (dimensions/format) and
 * returns 1; on failure fills `status` with a reason and returns 0.
 */
int gui_img_decode(const unsigned char *data, unsigned long len,
                   uint32_t *fb, uint32_t fbw, uint32_t fbh,
                   char *status, unsigned long status_sz);

/* Synthesize a self-contained demo "test card" image into the framebuffer.
 * Used when no image file is loadable so the viewer is never blank. */
void gui_img_demo(uint32_t *fb, uint32_t fbw, uint32_t fbh);

#endif /* GUI_IMAGE_H */