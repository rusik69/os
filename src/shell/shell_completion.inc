/* ------------------------------------------------------------------ */
/*  Command name table for tab completion                              */
/* ------------------------------------------------------------------ */

static const char *all_cmds[] = {
    "arp", "base64", "basename", "beep", "calc", "cat", "cc", "clear",
    "cmos", "color", "cp", "cpuinfo", "curl", "cut", "date", "df", "diff", "dmesg",
    "dirname", "dns", "du", "echo", "edit", "env", "exec", "exit", "expr",
    "fg", "find", "format", "free", "grep", "head", "help", "hexdump",
    "history", "hostname", "hwinfo", "id", "ifconfig", "jobs", "kill", "ls", "lspci",
    "fbinfo",
    "md5sum", "meminfo", "mkdir", "mouse", "mv", "nl", "od", "paste",
    "ping", "play", "printf", "ps", "reboot", "rev", "rm", "route",
    "run", "seq", "serial", "shutdown", "sleep", "sort", "stat", "strings", "tac",
    "tail", "tee", "test", "time", "tmux", "touch", "tr", "udpsend",
    "uname", "uniq", "uptime", "users", "useradd", "userdel", "wait", "wc",
    "whoami", "write", "xargs", "xxd", "yes",
    "fat", "login", "logout", "lsblk", "lsusb", "passwd",
    "chmod", "chown", 0
};

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

        /* Find all matching commands */
        const char *matches[64]; int nmatch = 0;
        for (int i = 0; all_cmds[i]; i++) {
            if (strncmp(all_cmds[i], prefix, plen) == 0 && nmatch < 64)
                matches[nmatch++] = all_cmds[i];
        }
        if (nmatch == 0) return;
        if (nmatch == 1) {
            /* Unique match — complete it */
            erase_line(*len);
            int clen = strlen(matches[0]);
            memcpy(buf, matches[0], clen);
            buf[clen] = ' ';
            *len = clen + 1;
            kprintf("%s ", matches[0]);
        } else {
            /* Multiple matches — find common prefix and show options */
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

        char fnames[16][FS_MAX_NAME]; /* up to 16 matches */
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
            /* Find common prefix */
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
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("os> ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void erase_line(int len) {
    for (int i = 0; i < len; i++) putchar_both('\b');
    for (int i = 0; i < len; i++) putchar_both(' ');
    for (int i = 0; i < len; i++) putchar_both('\b');
}

void shell_tab_complete_telnet(char *buf, int *len, void *session) {
    (void)session; /* kprintf hooks are already set by telnetd */
    int has_space = 0;
    for (int i = 0; i < *len; i++) if (buf[i] == ' ') { has_space = 1; break; }

    if (!has_space) {
        /* Command completion */
        char prefix[64];
        int plen = *len > 63 ? 63 : *len;
        memcpy(prefix, buf, plen); prefix[plen] = '\0';

        const char *matches[64]; int nmatch = 0;
        for (int i = 0; all_cmds[i]; i++)
            if (strncmp(all_cmds[i], prefix, plen) == 0 && nmatch < 64)
                matches[nmatch++] = all_cmds[i];

        if (nmatch == 0) return;
        if (nmatch == 1) {
            /* Erase and rewrite */
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
        /* Filename completion */
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
