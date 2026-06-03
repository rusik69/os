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
