#ifndef FIRMWARE_H
#define FIRMWARE_H

#include "types.h"

/* Firmware blob descriptor */
struct firmware {
    const uint8_t *data;
    size_t size;
};

/* Built-in firmware entry for small firmware blobs */
struct builtin_fw {
    const char *name;
    const uint8_t *data;
    size_t size;
};

/*
 * request_firmware_nowait — Asynchronously load firmware.
 *
 * This is the non-blocking variant of request_firmware().  The callback
 * function @cont is called when the firmware is ready (or if the load
 * fails).  The callback runs in workqueue context.
 *
 * @fw_ptr:   Output: firmware descriptor (valid in callback)
 * @name:     Firmware name
 * @cont:     Continuation callback (called with the result)
 * @context:  Opaque pointer passed to the continuation callback
 *
 * Returns 0 if the load was initiated (callback will fire), negative
 * errno on failure.  The callback is guaranteed to fire exactly once.
 */
typedef void (*firmware_cont_t)(const struct firmware *fw, void *context);

int request_firmware_nowait(const struct firmware **fw_ptr, const char *name,
                             firmware_cont_t cont, void *context);

/*
 * request_firmware — Load firmware by name (Linux-compatible API).
 *
 * Looks up the cache first, then the built-in table, then
 * /lib/firmware/<name> via VFS.  Returns 0 on success with
 * *fw pointing to the loaded blob.
 *
 * The caller must call release_firmware() when done.
 */
int request_firmware(const struct firmware **fw, const char *name);

/*
 * release_firmware — Release firmware blob obtained from
 * request_firmware().  Safe to call with NULL.
 */
void release_firmware(const struct firmware *fw);

/*
 * firmware_load — Load firmware by name (legacy API).
 * Same semantics as request_firmware but takes a struct firmware *
 * instead of const struct firmware **.
 */
int firmware_load(const char *name, struct firmware *fw);

/*
 * firmware_release — Release firmware data (legacy API).
 */
void firmware_release(struct firmware *fw);

/* Register a built-in firmware blob (for small/essential firmware). */
int firmware_register_builtin(const char *name, const uint8_t *data, size_t size);

/*
 * Flush all dynamically cached firmware blobs.
 * Called on low-memory conditions to reclaim cache memory.
 * Returns the number of cache entries flushed.
 */
int firmware_cache_flush(void);

/* Initialize firmware subsystem. */
void firmware_init(void);

#endif /* FIRMWARE_H */
