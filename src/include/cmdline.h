#ifndef CMDLINE_H
#define CMDLINE_H

#include "types.h"

/* Kernel command-line parameter parsing.
 * Parameters are passed via multiboot cmdline (bootloader-provided).
 * Format: key=value key2=value2 ...
 *
 * Usage:
 *   if (cmdline_has("debug")) { ... }
 *   const char *val = cmdline_get("root");
 *   int num = cmdline_get_int("mem_size", 256);
 */

/* Initialize parser with cmdline string from multiboot info */
void cmdline_init(const char *cmdline);

/* Check if a parameter is present (boolean) */
int cmdline_has(const char *key);

/* Get string value of a parameter (NULL if not found) */
const char *cmdline_get(const char *key);

/* Get integer value of a parameter (default if not found) */
int cmdline_get_int(const char *key, int default_val);

/* Get the full raw cmdline */
const char *cmdline_raw(void);

#endif
