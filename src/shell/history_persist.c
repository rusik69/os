/* history_persist.c — Persist shell history to disk
 *
 * Loads and saves shell history to /home/user/.history so that
 * history survives reboots and is shared across shell sessions.
 *
 * Uses the kernel's VFS layer to read/write the history file.
 */

#include "types.h"
#include "shell.h"
#include "vfs.h"
#include "fs.h"
#include "printf.h"
#include "string.h"
#include "heap.h"

/* Path to the persistent history file */
#define HISTORY_PATH "/home/user/.history"

/* Maximum buffer for reading the history file */
#define HISTORY_MAX_BYTES (256 * 256)

/* ── history_persist_save ──────────────────────────────────────────
 *
 * Writes the current shell history ring buffer to the persistent file.
 */
void history_persist_save(void)
{
    /* Create or truncate the history file */
    vfs_create(HISTORY_PATH, VFS_TYPE_FILE);

    /* Count number of non-empty history entries */
    int total = 0;
    for (int i = 0; i < 256; i++) {
        const char *entry = shell_history_entry(i);
        if (entry && entry[0] != '\0')
            total++;
    }

    /* Write each history entry as a line using the VFS append API */
    char line[260];
    for (int i = 0; i < 256; i++) {
        const char *entry = shell_history_entry(i);
        if (!entry || entry[0] == '\0')
            continue;

        int len = strlen(entry);
        if (len > 256)
            len = 256;
        memcpy(line, entry, len);
        line[len] = '\n';
        line[len + 1] = '\0';

        vfs_append(HISTORY_PATH, line, len + 1);
    }
}

/* ── history_persist_load ──────────────────────────────────────────
 *
 * Reads the persistent history file and populates the shell history.
 * Called during shell initialisation.
 */
void history_persist_load(void)
{
    /* Check if the file exists by trying to stat it */
    struct vfs_stat st;
    if (vfs_stat(HISTORY_PATH, &st) < 0)
        return;

    /* Allocate buffer for the file */
    uint8_t *data = (uint8_t *)kmalloc(HISTORY_MAX_BYTES);
    if (!data)
        return;

    /* Read the entire file */
    uint32_t out_size = 0;
    if (vfs_read(HISTORY_PATH, data, HISTORY_MAX_BYTES, &out_size) < 0 || out_size == 0) {
        kfree(data);
        return;
    }

    /* Parse lines and add to shell history */
    char *line_start = (char *)data;
    for (uint32_t i = 0; i < out_size; i++) {
        if (data[i] == '\n') {
            data[i] = '\0';
            if (line_start[0] != '\0')
                shell_history_add(line_start);
            line_start = (char *)&data[i + 1];
        }
    }

    /* Last line if no trailing newline */
    if (line_start[0] != '\0')
        shell_history_add(line_start);

    kfree(data);
}
