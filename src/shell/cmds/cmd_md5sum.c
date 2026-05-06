/* cmd_md5sum.c — Compute simple checksums (djb2 hash) */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

/* Simple hash for a kernel without crypto libraries.
 * Uses djb2 variant — not cryptographic but useful for integrity checks. */
static uint32_t hash_djb2(const uint8_t *data, uint32_t len) {
    uint32_t hash = 5381;
    for (uint32_t i = 0; i < len; i++)
        hash = ((hash << 5) + hash) + data[i];
    return hash;
}

static uint32_t hash_fnv1a(const uint8_t *data, uint32_t len) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

void cmd_md5sum(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: md5sum <file>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(path, buf, 4096, &size) != 0) {
        kprintf("md5sum: cannot read '%s'\n", path);
        return;
    }

    uint32_t h1 = hash_djb2((uint8_t *)buf, size);
    uint32_t h2 = hash_fnv1a((uint8_t *)buf, size);
    kprintf("%08x%08x  %s\n", (uint64_t)h1, (uint64_t)h2, path);
}
