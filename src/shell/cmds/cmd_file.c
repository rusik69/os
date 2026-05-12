/* cmd_file.c — determine file type */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_file(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: file <path>\n");
        return;
    }

    char path[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) path[i++] = *p++;
    path[i] = '\0';
    /* strip trailing spaces */
    while (i > 0 && path[i-1] == ' ') path[--i] = '\0';

    char fpath[64];
    if (path[0] != '/') { fpath[0] = '/'; strncpy(fpath+1, path, 62); }
    else strncpy(fpath, path, 63);
    fpath[63] = '\0';

    struct vfs_stat st;
    if (vfs_stat(fpath, &st) != 0) {
        kprintf("%s: no such file or directory\n", path);
        return;
    }

    /* Check if it's a directory */
    if (st.type == 2) {
        kprintf("%s: directory\n", path);
        return;
    }

    if (st.size == 0) {
        kprintf("%s: empty\n", path);
        return;
    }

    /* Read first 8 bytes for magic detection */
    static char hdr[16];
    uint32_t got = 0;
    if (vfs_read(fpath, hdr, 8, &got) != 0 || got < 4) {
        kprintf("%s: data\n", path);
        return;
    }

    /* ELF magic */
    if (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
        kprintf("%s: ELF executable\n", path);
        return;
    }

    /* Check for printable text */
    static char buf[512];
    uint32_t sz = 0;
    int text = 1;
    uint32_t check = (st.size < 512) ? st.size : 512;
    if (vfs_read(fpath, buf, check, &sz) == 0) {
        for (uint32_t k = 0; k < sz; k++) {
            unsigned char c = (unsigned char)buf[k];
            if (c < 8 || (c > 13 && c < 32 && c != 27)) { text = 0; break; }
        }
    } else {
        text = 0;
    }

    if (text)
        kprintf("%s: ASCII text (%u bytes)\n", path, (uint64_t)st.size);
    else
        kprintf("%s: binary data (%u bytes)\n", path, (uint64_t)st.size);
}
