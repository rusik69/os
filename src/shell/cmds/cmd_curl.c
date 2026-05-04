/* cmd_curl.c — curl command */
#include "shell_cmds.h"
#include "printf.h"
#include "net.h"
#include "e1000.h"

void cmd_curl(const char *args) {
    if (!args || !*args) { kprintf("Usage: curl [-F] <url>\n"); return; }
    if (!e1000_is_present()) { kprintf("No network device\n"); return; }

    /* Detect -F flag (follow redirects) */
    int follow = 0;
    const char *p = args;
    if (p[0] == '-' && p[1] == 'F' && (p[2] == ' ' || p[2] == '\0')) {
        follow = 1;
        p += 2;
        while (*p == ' ') p++;
        if (!*p) { kprintf("Usage: curl [-F] <url>\n"); return; }
    }

    /* Parse: [http://]host[:port][/path] */
    char host[128];
    char path[256];
    uint16_t port = 80;
    int hi = 0;

    if (p[0]=='h' && p[1]=='t' && p[2]=='t' && p[3]=='p') {
        if (p[4]==':' && p[5]=='/' && p[6]=='/') { p += 7; }
        else if (p[4]=='s' && p[5]==':' && p[6]=='/' && p[7]=='/') { port = 443; p += 8; }
    }

    while (*p && *p != '/' && *p != ':' && *p != ' ' && hi < 127)
        host[hi++] = *p++;
    host[hi] = '\0';

    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
    }

    if (*p == '/') {
        int pi = 0;
        while (*p && *p != ' ' && pi < 255)
            path[pi++] = *p++;
        path[pi] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }

    kprintf("Connecting to %s:%u%s...%s\n", host, (uint64_t)port, path,
            follow ? " (following redirects)" : "");

    static char buf[4096];
    int n = net_http_get_ex(host, port, path, buf, sizeof(buf), follow);
    if (n < 0) {
        kprintf("Request failed\n");
    } else {
        kprintf("%s\n", buf);
    }
}
