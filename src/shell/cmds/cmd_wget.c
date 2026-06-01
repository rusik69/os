/* cmd_wget.c — wget <url> [filename] */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_wget(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: wget <url> [filename]\n");
        shell_set_exit_status(1);
        return;
    }

    char url[256]; char saveas[64]; saveas[0] = '\0';
    int i = 0;
    while (args[i] == ' ') i++;
    int j = 0;
    while (args[i] && args[i] != ' ' && j < 255) url[j++] = args[i++];
    url[j] = '\0';
    while (args[i] == ' ') i++;
    if (args[i]) { j = 0; while (args[i] && j < 63) saveas[j++] = args[i++]; saveas[j] = '\0'; }

    /* Parse http://host[:port]/path */
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else { kprintf("wget: only http:// supported\n"); shell_set_exit_status(1); return; }

    char host[128]; uint16_t port = 80; char path[128]; path[0] = '/'; path[1] = '\0';
    j = 0;
    while (*p && *p != '/' && *p != ':' && j < 127) host[j++] = *p++;
    host[j] = '\0';
    if (*p == ':') {
        p++; port = 0;
        while (*p >= '0' && *p <= '9') { port = (uint16_t)(port*10 + (*p-'0')); p++; }
    }
    if (*p == '/') strncpy(path, p, 127);
    path[127] = '\0';

    /* derive filename from path if not given */
    if (!saveas[0]) {
        const char *sl = path; const char *last = path;
        while (*sl) { if (*sl == '/') last = sl+1; sl++; }
        if (*last) strncpy(saveas, last, 63);
        else strncpy(saveas, "index.html", 63);
        saveas[63] = '\0';
    }

    static char buf[8192];
    int n = libc_net_http_get_ex(host, port, path, buf, sizeof(buf)-1, 1);
    if (n <= 0) {
        kprintf("wget: download failed\n");
        shell_set_exit_status(1);
        return;
    }

    char fpath[68];
    if (saveas[0] == '/') strncpy(fpath, saveas, 67);
    else { fpath[0]='/'; strncpy(fpath+1, saveas, 62); }
    fpath[67] = '\0';

    libc_vfs_write(fpath, buf, (uint32_t)n);
    kprintf("Saved %d bytes to %s\n", (unsigned long)n, fpath);
}
