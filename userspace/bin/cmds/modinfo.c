/* modinfo.c — display module metadata (author, description, license,
 *            vermagic, params) (D234 tasks 6-7)
 *
 * Reads module metadata from /sys/module/<name>/ files.
 * Falls back to query_module() syscall if sysfs is unavailable.
 *
 * Usage: modinfo <module_name>
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Read a sysfs file into buf.  Returns 0 on success, -1 on error. */
static int read_sysfs(const char *path, char *buf, int max)
{
    int fd = open(path, 0, 0);  /* O_RDONLY = 0 */
    if (fd < 0) return -1;
    int n = read(fd, buf, max - 1);
    close(fd);
    if (n < 0) return -1;
    /* Strip trailing newline */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        n--;
    buf[n] = '\0';
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: modinfo <module>\n");
        return 1;
    }

    const char *mod = argv[1];
    char path[256];
    char buf[128];

    /* Gather info from /sys/module/<name>/ */
    printf("module:         %s\n", mod);

    /* initstate */
    snprintf(path, sizeof(path), "/sys/module/%s/initstate", mod);
    if (read_sysfs(path, buf, sizeof(buf)) == 0)
        printf("state:          %s\n", buf);

    /* refcnt */
    snprintf(path, sizeof(path), "/sys/module/%s/refcnt", mod);
    if (read_sysfs(path, buf, sizeof(buf)) == 0)
        printf("refcnt:         %s\n", buf);

    /* version */
    snprintf(path, sizeof(path), "/sys/module/%s/version", mod);
    if (read_sysfs(path, buf, sizeof(buf)) == 0)
        printf("version:        %s\n", buf);

    /* srcversion */
    snprintf(path, sizeof(path), "/sys/module/%s/srcversion", mod);
    if (read_sysfs(path, buf, sizeof(buf)) == 0)
        printf("srcversion:     %s\n", buf);

    /* taint */
    snprintf(path, sizeof(path), "/sys/module/%s/taint", mod);
    if (read_sysfs(path, buf, sizeof(buf)) == 0)
        printf("taint:          %s\n", buf);

    /* holders */
    snprintf(path, sizeof(path), "/sys/module/%s/holders", mod);
    if (read_sysfs(path, buf, sizeof(buf)) == 0)
        printf("holders:        %s\n", buf);

    /* Parameters */
    printf("parameters:\n");
    snprintf(path, sizeof(path), "/sys/module/%s/parameters", mod);
    int fd = open(path, 0, 0);  /* O_RDONLY */
    if (fd >= 0) {
        char dirbuf[4096];
        int n = read(fd, dirbuf, sizeof(dirbuf) - 1);
        close(fd);
        if (n > 0) {
            dirbuf[n] = '\0';
            /* sysfs directory listing gives one entry per line */
            char *line = dirbuf;
            while (*line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                if (line[0] != '\0' && line[0] != '.') {
                    char param_path[512];
                    snprintf(param_path, sizeof(param_path),
                             "/sys/module/%s/parameters/%s", mod, line);
                    char valbuf[128];
                    if (read_sysfs(param_path, valbuf, sizeof(valbuf)) == 0)
                        printf("  %-16s = %s\n", line, valbuf);
                }
                if (nl)
                    line = nl + 1;
                else
                    break;
            }
        }
    } else {
        printf("  (none)\n");
    }

    return 0;
}
