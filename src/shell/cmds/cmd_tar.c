/* cmd_tar.c — tar command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

#define TAR_MAGIC "TARS"

void cmd_tar(const char *args) {
    if (!args) {
        kprintf("Usage: tar <archive> <file1> [file2 ...]\n");
        return;
    }

    char arg_copy[512];
    strncpy(arg_copy, args, 511);
    arg_copy[511] = '\0';

    char *files[32];
    int file_count = 0;
    char *token = strtok(arg_copy, " ");

    if (!token) return;
    char archive_name[64];
    strncpy(archive_name, token, 63);
    archive_name[63] = '\0';
    token = strtok(NULL, " ");

    while (token && file_count < 32) {
        files[file_count++] = token;
        token = strtok(NULL, " ");
    }

    if (file_count == 0) {
        kprintf("Error: No files to archive\n");
        return;
    }

    /* First pass: calculate total archive size */
    uint32_t sizes[32];
    uint32_t total = 4; /* magic */
    for (int i = 0; i < file_count; i++) {
        uint32_t sz = 0;
        if (libc_fs_read_file(files[i], NULL, 0, &sz) != 0) {
            kprintf("Warning: Could not stat file %s, skipping\n", files[i]);
            sizes[i] = (uint32_t)-1;
            continue;
        }
        sizes[i] = sz;
        uint32_t name_len = strlen(files[i]);
        total += 4 + name_len + 4 + sz; /* name_len field + name + size field + content */
    }

    /* Allocate single archive buffer */
    char *arch = (char *)libc_malloc(total);
    if (!arch) {
        kprintf("Error: Out of memory\n");
        return;
    }

    /* Fill buffer */
    uint32_t pos = 0;
    memcpy(arch, TAR_MAGIC, 4);
    pos += 4;

    int archived = 0;
    for (int i = 0; i < file_count; i++) {
        if (sizes[i] == (uint32_t)-1) continue;

        uint32_t name_len = strlen(files[i]);
        memcpy(arch + pos, &name_len, 4); pos += 4;
        memcpy(arch + pos, files[i], name_len); pos += name_len;
        memcpy(arch + pos, &sizes[i], 4); pos += 4;

        uint32_t read_bytes = 0;
        if (libc_fs_read_file(files[i], arch + pos, sizes[i], &read_bytes) != 0) {
            kprintf("Warning: Could not read file %s\n", files[i]);
            libc_free(arch);
            return;
        }
        pos += sizes[i];
        archived++;
    }

    /* Write archive in a single call */
    vfs_write(archive_name, arch, pos);
    libc_free(arch);

    kprintf("Archive %s created successfully with %d files\n", archive_name, archived);
}
