/* ------------------------------------------------------------------ */
/*  Tab completion (uses shell_cmd_table)                            */
/* ------------------------------------------------------------------ */

#include "shell_cmd_table.h"
#include "rtc.h"
#include "process.h"
#include "sysctl.h"

/* ------------------------------------------------------------------ */
/*  Tab completion                                                     */
/* ------------------------------------------------------------------ */

static void shell_prompt(void);
static void erase_line(int len);

static void shell_tab_complete(char *buf, int *len) {
    /* Determine if we're completing a command (first word) or filename */
    int has_space = 0;
    for (int i = 0; i < *len; i++) if (buf[i] == ' ') { has_space = 1; break; }

    if (!has_space) {
        /* Command completion */
        char prefix[64];
        int plen = *len > 63 ? 63 : *len;
        memcpy(prefix, buf, plen); prefix[plen] = '\0';

        const char *matches[64]; int nmatch = 0;
        int ncmds = shell_cmd_count();
        for (int i = 0; i < ncmds && nmatch < 64; i++) {
            const shell_cmd_entry_t *e = shell_cmd_entry(i);
            if (strncmp(e->name, prefix, plen) == 0)
                matches[nmatch++] = e->name;
        }
        if (nmatch == 0) return;
        if (nmatch == 1) {
            erase_line(*len);
            int clen = strlen(matches[0]);
            memcpy(buf, matches[0], clen);
            buf[clen] = ' ';
            *len = clen + 1;
            kprintf("%s ", matches[0]);
        } else {
            int common = strlen(matches[0]);
            for (int i = 1; i < nmatch; i++) {
                int j = 0;
                while (j < common && matches[0][j] == matches[i][j]) j++;
                common = j;
            }
            if (common > plen) {
                erase_line(*len);
                memcpy(buf, matches[0], common);
                *len = common;
                buf[*len] = '\0';
                kprintf("%s", buf);
            } else {
                kprintf("\n");
                for (int i = 0; i < nmatch; i++) kprintf("  %s", matches[i]);
                kprintf("\n");
                shell_prompt();
                buf[*len] = '\0';
                kprintf("%s", buf);
            }
        }
    } else {
        /* Filename completion — get the last word */
        int wstart = *len;
        while (wstart > 0 && buf[wstart - 1] != ' ') wstart--;
        char prefix[64];
        int plen = *len - wstart;
        if (plen > 63) plen = 63;
        memcpy(prefix, buf + wstart, plen); prefix[plen] = '\0';

        char fnames[16][FS_MAX_NAME];
        int nmatch = fs_list_names("/", prefix, fnames, 16);
        if (nmatch == 0) return;
        if (nmatch == 1) {
            erase_line(*len);
            int clen = strlen(fnames[0]);
            int newlen = wstart + clen;
            memcpy(buf + wstart, fnames[0], clen);
            buf[newlen] = ' ';
            *len = newlen + 1;
            buf[*len] = '\0';
            kprintf("%s", buf);
        } else {
            int common = strlen(fnames[0]);
            for (int i = 1; i < nmatch; i++) {
                int j = 0;
                while (j < common && fnames[0][j] == fnames[i][j]) j++;
                common = j;
            }
            if (common > plen) {
                erase_line(*len);
                memcpy(buf + wstart, fnames[0], common);
                *len = wstart + common;
                buf[*len] = '\0';
                kprintf("%s", buf);
            } else {
                kprintf("\n");
                for (int i = 0; i < nmatch; i++) kprintf("  %s", fnames[i]);
                kprintf("\n");
                shell_prompt();
                buf[*len] = '\0';
                kprintf("%s", buf);
            }
        }
    }
}

static void shell_prompt(void) {
    struct process *cur = process_get_current();
    const char *ps1 = shell_var_get("PS1");
    static int cmd_number = 0;
    cmd_number++;

    if (ps1 && ps1[0]) {
        /* Parse PS1 escape sequences and print directly */
        char buf[256];
        int pos = 0;
        for (const char *p = ps1; *p && pos < (int)sizeof(buf) - 2; p++) {
            if (*p == '\\') {
                p++;
                switch (*p) {
                    case 'h': { /* hostname */
                        const char *h = sysctl_get_hostname();
                        int hl = (int)strlen(h);
                        int rem = (int)sizeof(buf) - 1 - pos;
                        if (hl > rem) hl = rem;
                        memcpy(buf + pos, h, (size_t)hl);
                        pos += hl;
                        break;
                    }
                    case 'u': { /* username */
                        uint32_t uid = cur ? cur->uid : 0;
                        const char *uname = (uid == 0) ? "root" : "user";
                        int ul = (int)strlen(uname);
                        int rem = (int)sizeof(buf) - 1 - pos;
                        if (ul > rem) ul = rem;
                        memcpy(buf + pos, uname, (size_t)ul);
                        pos += ul;
                        break;
                    }
                    case 'w': { /* working directory */
                        const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
                        int wl = (int)strlen(cwd);
                        int rem = (int)sizeof(buf) - 1 - pos;
                        if (wl > rem) wl = rem;
                        memcpy(buf + pos, cwd, (size_t)wl);
                        pos += wl;
                        break;
                    }
                    case 't': { /* time (HH:MM:SS) */
                        struct rtc_time t;
                        rtc_get_time(&t);
                        int n = snprintf(buf + pos, (size_t)(sizeof(buf) - pos),
                                         "%02u:%02u:%02u",
                                         (unsigned)t.hour, (unsigned)t.minute,
                                         (unsigned)t.second);
                        if (n > 0) pos += n;
                        if (pos >= (int)sizeof(buf)) pos = (int)sizeof(buf) - 1;
                        break;
                    }
                    case '$': { /* prompt symbol */
                        uint32_t uid = cur ? cur->uid : 0;
                        buf[pos++] = (uid == 0) ? '#' : '$';
                        break;
                    }
                    case '#': { /* command number */
                        int n = snprintf(buf + pos, (size_t)(sizeof(buf) - pos),
                                         "%d", cmd_number);
                        if (n > 0) pos += n;
                        if (pos >= (int)sizeof(buf)) pos = (int)sizeof(buf) - 1;
                        break;
                    }
                    case '\\': { /* literal backslash */
                        buf[pos++] = '\\';
                        break;
                    }
                    case '\0': { /* backslash at end of string */
                        buf[pos++] = '\\';
                        p--; /* don't skip null */
                        break;
                    }
                    default: { /* unknown escape: emit as-is */
                        buf[pos++] = '\\';
                        buf[pos++] = *p;
                        break;
                    }
                }
            } else {
                buf[pos++] = *p;
            }
        }
        buf[pos] = '\0';
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        kprintf("%s", buf);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        /* Default prompt: username@hostname:cwd$ */
        const char *host = sysctl_get_hostname();
        uint32_t uid = cur ? cur->uid : 0;
        const char *uname = (uid == 0) ? "root" : "user";
        const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
        char sym = (uid == 0) ? '#' : '$';
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        kprintf("%s@%s:%s%c ", uname, host, cwd, sym);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void erase_line(int len) {
    for (int i = 0; i < len; i++) putchar_both('\b');
    for (int i = 0; i < len; i++) putchar_both(' ');
    for (int i = 0; i < len; i++) putchar_both('\b');
}

void shell_tab_complete_telnet(char *buf, int *len, void *session) {
    (void)session;
    int has_space = 0;
    for (int i = 0; i < *len; i++) if (buf[i] == ' ') { has_space = 1; break; }

    if (!has_space) {
        char prefix[64];
        int plen = *len > 63 ? 63 : *len;
        memcpy(prefix, buf, plen); prefix[plen] = '\0';

        const char *matches[64]; int nmatch = 0;
        int ncmds = shell_cmd_count();
        for (int i = 0; i < ncmds && nmatch < 64; i++) {
            const shell_cmd_entry_t *e = shell_cmd_entry(i);
            if (strncmp(e->name, prefix, plen) == 0)
                matches[nmatch++] = e->name;
        }

        if (nmatch == 0) return;
        if (nmatch == 1) {
            for (int k = 0; k < *len; k++) kprintf("\b \b");
            int clen = strlen(matches[0]);
            memcpy(buf, matches[0], clen);
            buf[clen] = ' ';
            *len = clen + 1;
            buf[*len] = '\0';
            kprintf("%s", buf);
        } else {
            int common = strlen(matches[0]);
            for (int i = 1; i < nmatch; i++) {
                int j = 0;
                while (j < common && matches[0][j] == matches[i][j]) j++;
                common = j;
            }
            if (common > plen) {
                for (int k = 0; k < *len; k++) kprintf("\b \b");
                memcpy(buf, matches[0], common);
                *len = common;
                buf[*len] = '\0';
                kprintf("%s", buf);
            } else {
                kprintf("\r\n");
                for (int i = 0; i < nmatch; i++) kprintf("  %s", matches[i]);
                kprintf("\r\nos> %s", buf);
            }
        }
    } else {
        int wstart = *len;
        while (wstart > 0 && buf[wstart - 1] != ' ') wstart--;
        char prefix[64];
        int plen = *len - wstart;
        if (plen > 63) plen = 63;
        memcpy(prefix, buf + wstart, plen); prefix[plen] = '\0';

        char fnames[16][FS_MAX_NAME];
        int nmatch = fs_list_names("/", prefix, fnames, 16);
        if (nmatch == 0) return;
        if (nmatch == 1) {
            for (int k = 0; k < *len; k++) kprintf("\b \b");
            int clen = strlen(fnames[0]);
            int newlen = wstart + clen;
            memcpy(buf + wstart, fnames[0], clen);
            buf[newlen] = ' ';
            *len = newlen + 1;
            buf[*len] = '\0';
            kprintf("%s", buf);
        } else {
            int common = strlen(fnames[0]);
            for (int i = 1; i < nmatch; i++) {
                int j = 0;
                while (j < common && fnames[0][j] == fnames[i][j]) j++;
                common = j;
            }
            if (common > plen) {
                for (int k = 0; k < *len; k++) kprintf("\b \b");
                memcpy(buf + wstart, fnames[0], common);
                *len = wstart + common;
                buf[*len] = '\0';
                kprintf("%s", buf);
            } else {
                kprintf("\r\n");
                for (int i = 0; i < nmatch; i++) kprintf("  %s", fnames[i]);
                kprintf("\r\nos> %s", buf);
            }
        }
    }
}
