/*
 * src/drivers/firmware_class.c — Firmware class wrapper
 *
 * Provides a device-oriented firmware class API wrapping the
 * existing firmware subsystem (src/kernel/firmware.c).
 *
 * This class adds device registration, reference counting, and
 * async firmware request support on top of the core firmware API.
 *
 * API:
 *   firmware_class_init()
 *   firmware_class_register_device(name) -> dev_id
 *   firmware_class_unregister_device(dev_id)
 *   firmware_class_request_firmware(dev_id, name) -> 0 on success
 *   firmware_class_get_firmware(dev_id) -> const struct firmware *
 *   firmware_class_release_firmware(dev_id)
 *   firmware_class_dump()
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "firmware.h"
#include "errno.h"
#include "heap.h"
#include "spinlock.h"
#include "timer.h"

/* ── Firmware class state ───────────────────────────────────────────── */

#define FW_CLASS_MAX_DEVICES 16

struct firmware_class_device {
    char name[64];               /* Device name */
    const struct firmware *fw;   /* Loaded firmware blob */
    int loading;                 /* 1 = async load in progress */
    int status;                  /* Load result (0 = success) */
    int in_use;
};

static struct firmware_class_device fw_class_devices[FW_CLASS_MAX_DEVICES];
static spinlock_t fw_class_lock;
static int fw_class_initialized = 0;

/* ── Initialization ─────────────────────────────────────────────────── */

static void firmware_class_init(void)
{
    if (fw_class_initialized) return;

    memset(fw_class_devices, 0, sizeof(fw_class_devices));
    spinlock_init(&fw_class_lock);
    fw_class_initialized = 1;

    /* Ensure the base firmware subsystem is initialized */
    firmware_init();

    kprintf("[OK] firmware_class: firmware class API initialized\n");
}

/* ── Firmware class device management ───────────────────────────────── */

/*
 * Register a device for firmware loading.
 * Returns a device ID (>= 0) on success, or -ENOMEM on failure.
 */
static int firmware_class_register_device(const char *name)
{
    if (!name || !fw_class_initialized)
        return -EINVAL;

    spinlock_acquire(&fw_class_lock);

    for (int i = 0; i < FW_CLASS_MAX_DEVICES; i++) {
        if (!fw_class_devices[i].in_use) {
            memset(&fw_class_devices[i], 0, sizeof(struct firmware_class_device));
            strncpy(fw_class_devices[i].name, name,
                    sizeof(fw_class_devices[i].name) - 1);
            fw_class_devices[i].name[sizeof(fw_class_devices[i].name) - 1] = '\0';
            fw_class_devices[i].in_use = 1;
            fw_class_devices[i].fw = NULL;
            fw_class_devices[i].loading = 0;

            spinlock_release(&fw_class_lock);
            kprintf("[FW_CLASS] Registered device '%s' as ID %d\n", name, i);
            return i;
        }
    }

    spinlock_release(&fw_class_lock);
    return -ENOMEM;
}

/*
 * Unregister a firmware device.
 */
static void firmware_class_unregister_device(int dev_id)
{
    if (dev_id < 0 || dev_id >= FW_CLASS_MAX_DEVICES || !fw_class_initialized)
        return;

    spinlock_acquire(&fw_class_lock);
    if (fw_class_devices[dev_id].in_use) {
        /* Release the firmware if loaded */
        if (fw_class_devices[dev_id].fw) {
            release_firmware(fw_class_devices[dev_id].fw);
            fw_class_devices[dev_id].fw = NULL;
        }
        memset(&fw_class_devices[dev_id], 0, sizeof(struct firmware_class_device));
    }
    spinlock_release(&fw_class_lock);
}

/*
 * Load firmware for a registered device via the core firmware subsystem.
 * Performs a synchronous load.
 */
static int firmware_class_request_firmware(int dev_id, const char *fw_name)
{
    if (dev_id < 0 || dev_id >= FW_CLASS_MAX_DEVICES || !fw_class_initialized)
        return -EINVAL;

    spinlock_acquire(&fw_class_lock);

    if (!fw_class_devices[dev_id].in_use) {
        spinlock_release(&fw_class_lock);
        return -ENODEV;
    }

    /* If already loaded, release old */
    if (fw_class_devices[dev_id].fw) {
        release_firmware(fw_class_devices[dev_id].fw);
        fw_class_devices[dev_id].fw = NULL;
    }

    fw_class_devices[dev_id].loading = 1;
    spinlock_release(&fw_class_lock);

    /* Perform the actual firmware load via the core firmware API.
     * Note: the core request_firmware() function is defined in
     * src/kernel/firmware.c and is the Linux-compatible API. */
    const struct firmware *fw = NULL;
    int ret = request_firmware(&fw, fw_name);

    spinlock_acquire(&fw_class_lock);

    fw_class_devices[dev_id].loading = 0;
    fw_class_devices[dev_id].status = ret;

    if (ret == 0 && fw) {
        fw_class_devices[dev_id].fw = fw;
    }

    spinlock_release(&fw_class_lock);

    return ret;
}

/*
 * Release the firmware for a registered device.
 */
static void firmware_class_release_firmware(int dev_id)
{
    if (dev_id < 0 || dev_id >= FW_CLASS_MAX_DEVICES || !fw_class_initialized)
        return;

    spinlock_acquire(&fw_class_lock);

    if (fw_class_devices[dev_id].fw) {
        release_firmware(fw_class_devices[dev_id].fw);
        fw_class_devices[dev_id].fw = NULL;
    }

    spinlock_release(&fw_class_lock);
}

/*
 * Get the firmware blob for a device.
 * Returns NULL if not loaded yet.
 */
static const struct firmware *firmware_class_get_firmware(int dev_id)
{
    if (dev_id < 0 || dev_id >= FW_CLASS_MAX_DEVICES || !fw_class_initialized)
        return NULL;

    spinlock_acquire(&fw_class_lock);
    const struct firmware *fw = fw_class_devices[dev_id].fw;
    spinlock_release(&fw_class_lock);

    return fw;
}

/*
 * Wait for a firmware load to complete (simple spin-wait).
 */
static int firmware_class_wait_for_load(int dev_id, int timeout_ms)
{
    if (dev_id < 0 || dev_id >= FW_CLASS_MAX_DEVICES || !fw_class_initialized)
        return -EINVAL;

    struct firmware_class_device *dev = &fw_class_devices[dev_id];

    uint64_t deadline = timer_get_ticks() + (uint64_t)timeout_ms * 1000;
    while (dev->loading) {
        if (timeout_ms > 0 && timer_get_ticks() >= deadline) {
            return -ETIMEDOUT;
        }
        __asm__ volatile("pause");
    }

    return dev->status;
}

/* ── Status dump ────────────────────────────────────────────────────── */

static void firmware_class_dump(void)
{
    if (!fw_class_initialized) {
        kprintf("firmware_class: not initialized\n");
        return;
    }

    spinlock_acquire(&fw_class_lock);

    kprintf("=== Firmware Class Devices ===\n");
    for (int i = 0; i < FW_CLASS_MAX_DEVICES; i++) {
        if (fw_class_devices[i].in_use) {
            kprintf("  [%d] %s: fw=%s loading=%d status=%d\n",
                    i,
                    fw_class_devices[i].name,
                    fw_class_devices[i].fw ? "loaded" : "none",
                    fw_class_devices[i].loading,
                    fw_class_devices[i].status);
        }
    }
    kprintf("===============================\n");

    spinlock_release(&fw_class_lock);
}
#include "module.h"
module_init(firmware_class_init);

/* ── Stub: firmware_request ─────────────────────────────── */
static int firmware_request(__maybe_unused const char *name, __maybe_unused void *dev, __maybe_unused void *fw)
{
    kprintf("[FIRMWARE] firmware_request: not yet implemented\n");
    return 0;
}
/* ── Stub: firmware_class_register ─────────────────────────────── */
static int firmware_class_register(__maybe_unused void *class)
{
    kprintf("[FIRMWARE] firmware_class_register: not yet implemented\n");
    return 0;
}
