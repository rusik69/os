#ifndef CONFIG_GZ_H
#define CONFIG_GZ_H

#include "types.h"

/* Return a pointer to the embedded gzip-compressed config data.
 * Sets *out_size to the compressed data length in bytes. */
const void *config_gz_get_data(uint32_t *out_size);

/* Return the uncompressed config text into buf (max max_len bytes).
 * Returns the number of bytes written, or -1 if not supported. */
int config_gz_get_uncompressed(char *buf, int max_len);

/* Initialize the config_gz subsystem — called at boot. */
void config_gz_init(void);

#endif /* CONFIG_GZ_H */
