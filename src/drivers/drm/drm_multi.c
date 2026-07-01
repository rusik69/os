/*
 * drm_multi.c — DRM multi-display management (clone / extend)
 *
 * Provides the userspace-facing ioctl for configuring clone-mode and
 * extend-mode multi-display layouts.  This module sits on top of the
 * CRTC-connector routing primitives in drm_core.c.
 *
 * Architecture:
 *   - drm_multi_init() / drm_multi_exit() — module lifecycle
 *   - drm_ioctl_mode_setlayout() — ioctl handler for DRM_IOCTL_MODE_SET_LAYOUT
 *   - drm_multi_set_layout() — programmatic entry point for clone/extend
 *   - drm_multi_check_layout() — validates a proposed layout before applying
 *
 * Clone mode:  all listed connectors are assigned to a single CRTC,
 *              displaying identical content on every output.
 * Extend mode: each connector gets its own CRTC (sequentially assigned
 *              starting from a base CRTC id), forming an extended desktop.
 * Single mode: restores the default 1:1 mapping (connector[i] → CRTC[i]).
 *
 * Item D143 task 12 — Multiple display support (clone/extend)
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "heap.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Module state
 * ═══════════════════════════════════════════════════════════════════ */

static int g_drm_multi_inited = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  Layout validation
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_multi_check_layout — Validate a proposed multi-display layout.
 *
 * Checks that:
 *   - layout_type is a known type
 *   - connector_ids_ptr is valid (non-NULL) for clone/extend with >0 connectors
 *   - All referenced connector IDs exist in the device
 *   - For clone mode: the specified CRTC exists
 *   - For extend mode: enough CRTCs exist for the connectors
 *
 * @dev:    DRM device
 * @layout: Layout configuration to validate
 *
 * Returns 0 if valid, negative errno if any check fails.
 */
static int drm_multi_check_layout(struct drm_device *dev,
                                  const struct drm_mode_set_layout *layout)
{
    if (!dev || !layout)
        return -EINVAL;

    /* Validate layout type */
    switch (layout->layout_type) {
        case DRM_MODE_LAYOUT_CLONE:
        case DRM_MODE_LAYOUT_EXTEND:
        case DRM_MODE_LAYOUT_SINGLE:
            break;
        default:
            return -EINVAL;
    }

    /* For SINGLE mode, no further checks needed */
    if (layout->layout_type == DRM_MODE_LAYOUT_SINGLE)
        return 0;

    /* Check connector pointer and count */
    if (layout->count_connectors == 0 || !layout->connector_ids_ptr)
        return -EINVAL;

    if (layout->count_connectors > (uint32_t)DRM_MAX_CONNECTOR)
        return -E2BIG;

    /* For clone mode: verify the CRTC exists */
    if (layout->layout_type == DRM_MODE_LAYOUT_CLONE) {
        int found = 0;
        for (int i = 0; i < DRM_MAX_CRTC; i++) {
            if (dev->crtcs[i].in_use &&
                dev->crtcs[i].crtc_id == layout->crtc_id) {
                found = 1;
                break;
            }
        }
        if (!found)
            return -ENOENT;
    }

    /* For extend mode: verify enough CRTCs exist */
    if (layout->layout_type == DRM_MODE_LAYOUT_EXTEND) {
        int crtc_count = 0;
        for (int i = 0; i < DRM_MAX_CRTC; i++) {
            if (dev->crtcs[i].in_use)
                crtc_count++;
        }
        if (crtc_count < (int)layout->count_connectors) {
            kprintf("[DRM multi] extend mode: need %d CRTCs, have %d\n",
                    layout->count_connectors, crtc_count);
            return -E2BIG;
        }
    }

    /* Verify each connector ID exists in the device */
    uint32_t *conn_ids = (uint32_t *)(uintptr_t)layout->connector_ids_ptr;

    for (uint32_t i = 0; i < layout->count_connectors; i++) {
        int found = 0;
        for (int j = 0; j < DRM_MAX_CONNECTOR; j++) {
            if (dev->connectors[j].in_use &&
                dev->connectors[j].connector_id == conn_ids[i]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            kprintf("[DRM multi] connector %u not found in device\n",
                    conn_ids[i]);
            return -ENOENT;
        }
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Layout application
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_multi_set_layout — Apply a multi-display layout configuration.
 *
 * Must only be called after drm_multi_check_layout() succeeds.
 *
 * For DRM_MODE_LAYOUT_CLONE:
 *   Assigns every connector in the list to the specified @crtc_id using
 *   drm_crtc_assign_connector().  All connectors then display identical
 *   content from the same scanout buffer.
 *
 * For DRM_MODE_LAYOUT_EXTEND:
 *   Assigns each connector to its own CRTC, starting from @crtc_id.
 *   The i-th connector is assigned to CRTC (base_crtc_id + i), wrapping
 *   modulo the device's CRTC count.  Each CRTC can then have its own
 *   framebuffer and mode, forming an extended desktop.
 *
 * For DRM_MODE_LAYOUT_SINGLE:
 *   Restores the default layout where connector[i] is assigned to
 *   CRTC[i] (1:1 mapping by index).  This undoes any previous
 *   clone/extend configuration.
 *
 * @dev:    DRM device
 * @layout: Layout configuration to apply
 *
 * Returns 0 on success, negative errno on failure.
 */
int drm_multi_set_layout(struct drm_device *dev,
                          const struct drm_mode_set_layout *layout)
{
    int ret;

    if (!dev || !layout)
        return -EINVAL;

    /* Validate first */
    ret = drm_multi_check_layout(dev, layout);
    if (ret < 0)
        return ret;

    switch (layout->layout_type) {
        case DRM_MODE_LAYOUT_CLONE: {
            kprintf("[DRM multi] clone mode: CRTC %u driving %u connectors\n",
                    layout->crtc_id, layout->count_connectors);

            uint32_t *conn_ids = (uint32_t *)(uintptr_t)layout->connector_ids_ptr;
            for (uint32_t i = 0; i < layout->count_connectors; i++) {
                ret = drm_crtc_assign_connector(dev, layout->crtc_id,
                                                 conn_ids[i]);
                if (ret < 0) {
                    kprintf("[DRM multi] clone: assign conn %u failed (%d)\n",
                            conn_ids[i], ret);
                    return ret;
                }
            }

            kprintf("[DRM multi] clone mode active: %u connectors on CRTC %u\n",
                    layout->count_connectors, layout->crtc_id);
            break;
        }

        case DRM_MODE_LAYOUT_EXTEND: {
            kprintf("[DRM multi] extend mode: %u connectors\n",
                    layout->count_connectors);

            uint32_t *conn_ids = (uint32_t *)(uintptr_t)layout->connector_ids_ptr;

            /* Build a list of available CRTC ids */
            uint32_t crtc_ids[DRM_MAX_CRTC];
            int num_crtcs = 0;
            for (int i = 0; i < DRM_MAX_CRTC; i++) {
                if (dev->crtcs[i].in_use) {
                    crtc_ids[num_crtcs++] = dev->crtcs[i].crtc_id;
                }
            }

            if (num_crtcs == 0) {
                kprintf("[DRM multi] extend: no CRTCs available\n");
                return -ENODEV;
            }

            /* Assign connector i to CRTC (i % num_crtcs) */
            for (uint32_t i = 0; i < layout->count_connectors; i++) {
                uint32_t crtc_idx = (uint32_t)(i % (uint32_t)num_crtcs);
                uint32_t target_crtc = crtc_ids[crtc_idx];

                ret = drm_crtc_assign_connector(dev, target_crtc,
                                                 conn_ids[i]);
                if (ret < 0) {
                    kprintf("[DRM multi] extend: conn %u → CRTC %u "
                            "failed (%d)\n",
                            conn_ids[i], target_crtc, ret);
                    return ret;
                }

                kprintf("[DRM multi] extend: connector %u → CRTC %u\n",
                        conn_ids[i], target_crtc);
            }

            kprintf("[DRM multi] extend mode active: %u connectors\n",
                    layout->count_connectors);
            break;
        }

        case DRM_MODE_LAYOUT_SINGLE: {
            kprintf("[DRM multi] single mode: restoring 1:1 mapping\n");

            /* Restore default: connector[i] → CRTC[i] (by index) */
            for (int i = 0; i < DRM_MAX_CONNECTOR; i++) {
                if (!dev->connectors[i].in_use)
                    continue;

                /* Find the CRTC with the same index */
                uint32_t crtc_id = (uint32_t)(i + 1); /* crtc_ids start at 1 */
                int crtc_found = 0;
                for (int j = 0; j < DRM_MAX_CRTC; j++) {
                    if (dev->crtcs[j].in_use &&
                        dev->crtcs[j].crtc_id == crtc_id) {
                        crtc_found = 1;
                        break;
                    }
                }
                if (!crtc_found) {
                    /* Fallback: assign to CRTC 1 (primary) */
                    for (int j = 0; j < DRM_MAX_CRTC; j++) {
                        if (dev->crtcs[j].in_use) {
                            crtc_id = dev->crtcs[j].crtc_id;
                            break;
                        }
                    }
                }

                ret = drm_crtc_assign_connector(dev, crtc_id,
                                                 dev->connectors[i].connector_id);
                if (ret < 0) {
                    kprintf("[DRM multi] single: assign conn %u → CRTC %u "
                            "failed (%d)\n",
                            dev->connectors[i].connector_id, crtc_id, ret);
                }
            }
            break;
        }

        default:
            return -EINVAL;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  ioctl handler
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_ioctl_mode_setlayout — Handle DRM_IOCTL_MODE_SET_LAYOUT.
 *
 * Reads the layout configuration from userspace, validates it, and
 * applies the clone/extend/single layout via drm_multi_set_layout().
 */
int drm_ioctl_mode_setlayout(struct drm_device *dev,
                              struct drm_file *fp, void *arg)
{
    (void)fp;

    if (!dev || !arg)
        return -EINVAL;

    struct drm_mode_set_layout *layout =
        (struct drm_mode_set_layout *)arg;

    kprintf("[DRM multi] set_layout: type=%u crtc=%u count=%u\n",
            layout->layout_type, layout->crtc_id,
            layout->count_connectors);

    return drm_multi_set_layout(dev, layout);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Module lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

int drm_multi_init(void)
{
    if (g_drm_multi_inited)
        return 0;

    g_drm_multi_inited = 1;

    kprintf("[DRM multi] multi-display management initialised\n");
    return 0;
}

void drm_multi_exit(void)
{
    if (!g_drm_multi_inited)
        return;

    g_drm_multi_inited = 0;

    kprintf("[DRM multi] multi-display management shut down\n");
}
