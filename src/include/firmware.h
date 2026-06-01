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

/* Load firmware by name. Looks up built-in table first, then
 * tries /lib/firmware/<name> via VFS.
 * Returns 0 on success with fw->data pointing to the loaded blob,
 * negative on error. */
int firmware_load(const char *name, struct firmware *fw);

/* Release firmware data. Frees memory if dynamically allocated. */
void firmware_release(struct firmware *fw);

/* Register a built-in firmware blob (for small/essential firmware). */
int firmware_register_builtin(const char *name, const uint8_t *data, size_t size);

/* Initialize firmware subsystem. */
void firmware_init(void);

#endif /* FIRMWARE_H */
