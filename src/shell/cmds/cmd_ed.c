/* cmd_ed.c — basic line editor */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

#define ED_MAX_LINES 256
#define ED_MAX_LINE_LEN 256

static char ed_buffer[ED_MAX_LINES][ED_MAX_LINE_LEN];
static int ed_num_lines = 0;
static int ed_current = 0;
static char ed_filename[128];
static int ed_modified = 0;
static int ed_running = 0;

static void ed_append_line(int after, const char *line) {
    if (ed_num_lines >= ED_MAX_LINES) {
        kprintf("?\n");
        return;
    }
    /* Shift lines down */
    for (int i = ed_num_lines; i > after; i--)
        strncpy(ed_buffer[i], ed_buffer[i-1], ED_MAX_LINE_LEN - 1);
    strncpy(ed_buffer[after], line, ED_MAX_LINE_LEN - 1);
    ed_buffer[after][ED_MAX_LINE_LEN - 1] = '\0';
    ed_num_lines++;
    ed_current = after;
    ed_modified = 1;
}

static void ed_delete_lines(int from, int to) {
    if (from < 0 || to >= ed_num_lines || from > to) return;
    int count = to - from + 1;
    for (int i = from; i + count < ed_num_lines; i++)
        strncpy(ed_buffer[i], ed_buffer[i + count], ED_MAX_LINE_LEN - 1);
    ed_num_lines -= count;
    if (ed_current >= ed_num_lines) ed_current = ed_num_lines - 1;
    ed_modified = 1;
}

static void ed_print_lines(int from, int to) {
    if (from < 0) from = 0;
    if (to >= ed_num_lines) to = ed_num_lines - 1;
    for (int i = from; i <= to; i++) {
        kprintf("%s\n", ed_buffer[i]);
    }
    ed_current = to;
}

static int ed_parse_addr(const char **p) {
    if (**p == '.') { (*p)++; return ed_current; }
    if (**p == '$') { (*p)++; return ed_num_lines - 1; }
    if (**p == '/') {
        (*p)++;
        char pat[128];
        int pi = 0;
        while (**p && **p != '/' && pi < 127) pat[pi++] = *(*p)++;
        pat[pi] = '\0';
        if (**p == '/') (*p)++;
        for (int i = ed_current + 1; i < ed_num_lines; i++) {
            if (strstr(ed_buffer[i], pat)) return i;
        }
        for (int i = 0; i <= ed_current; i++) {
            if (strstr(ed_buffer[i], pat)) return i;
        }
        return -1;
    }
    if (**p >= '0' && **p <= '9') {
        int n = 0;
        while (**p >= '0' && **p <= '9') n = n * 10 + *(*p)++ - '0';
        return n - 1; /* ed uses 1-based line numbers */
    }
    return -1;
}

void cmd_ed(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: ed <file>\n");
        return;
    }

    /* Initialize */
    ed_num_lines = 0;
    ed_current = 0;
    ed_modified = 0;
    ed_running = 1;
    strncpy(ed_filename, args, 127);
    ed_filename[127] = '\0';
    if (ed_filename[0] != '/') {
        char tmp[128];
        tmp[0] = '/';
        strncpy(tmp + 1, ed_filename, 126);
        tmp[127] = '\0';
        strncpy(ed_filename, tmp, 127);
    }

    /* Try to load file */
    static unsigned char file_buf[4096];
    uint32_t file_size = 0;
    if (libc_vfs_read(ed_filename, file_buf, sizeof(file_buf) - 1, &file_size) == 0 && file_size > 0) {
        file_buf[file_size] = '\0';
        char *line = (char *)file_buf;
        while (*line && ed_num_lines < ED_MAX_LINES) {
            char *eol = line;
            while (*eol && *eol != '\n') eol++;
            int len = (int)(eol - line);
            if (len > ED_MAX_LINE_LEN - 1) len = ED_MAX_LINE_LEN - 1;
            memcpy(ed_buffer[ed_num_lines], line, len);
            ed_buffer[ed_num_lines][len] = '\0';
            ed_num_lines++;
            line = (*eol == '\n') ? eol + 1 : eol;
        }
        kprintf("%d\n", (uint64_t)ed_num_lines);
        ed_current = ed_num_lines - 1;
    } else {
        kprintf("?%s\n", ed_filename);
    }

    /* Command loop */
    static char cmd_buf[256];
    while (ed_running) {
        kprintf(": ");
        /* Read a line from stdin */
        if (shell_has_stdin()) {
            int len = shell_stdin_read(cmd_buf, 255);
            cmd_buf[len] = '\0';
        } else {
            /* Interactive mode via libc_shell_read_line */
            /* For now, non-interactive: read next line from stdin */
            break;
        }

        /* Strip trailing newline */
        int clen = strlen(cmd_buf);
        while (clen > 0 && (cmd_buf[clen-1] == '\n' || cmd_buf[clen-1] == '\r'))
            cmd_buf[--clen] = '\0';

        if (clen == 0) continue;

        const char *p = cmd_buf;

        /* Parse optional addresses */
        int addr1 = -1, addr2 = -1;
        addr1 = ed_parse_addr(&p);
        if (*p == ',') {
            p++;
            addr2 = ed_parse_addr(&p);
        } else {
            addr2 = addr1;
        }
        if (addr1 < 0) addr1 = ed_current;
        if (addr2 < 0) addr2 = ed_current;
        if (addr2 >= ed_num_lines) addr2 = ed_num_lines - 1;

        while (*p == ' ') p++;

        char cmd = *p++;
        switch (cmd) {
            case 'p':
            case 'P':
                ed_print_lines(addr1, addr2);
                break;

            case 'n':
            case 'N':
                for (int i = addr1; i <= addr2 && i < ed_num_lines; i++)
                    kprintf("%d\t%s\n", (uint64_t)(i+1), ed_buffer[i]);
                ed_current = addr2;
                break;

            case 'l':
            case 'L':
                for (int i = addr1; i <= addr2 && i < ed_num_lines; i++) {
                    const char *s = ed_buffer[i];
                    while (*s) {
                        if (*s == '\t') kprintf("\\t");
                        else if (*s < 32) { kprintf("^%c", (uint64_t)(*s + 64)); }
                        else kprintf("%c", (uint64_t)(uint8_t)*s);
                        s++;
                    }
                    kprintf("$\n");
                }
                ed_current = addr2;
                break;

            case 'a':
                /* Append after current line */
                while (*p == ' ') p++;
                kprintf("(enter lines, '.' to end)\n");
                /* Non-interactive: just read from input */
                break;

            case 'i':
                /* Insert before current line */
                kprintf("(enter lines, '.' to end)\n");
                break;

            case 'd':
            case 'D':
                if (addr1 >= 0 && addr2 >= addr1)
                    ed_delete_lines(addr1, addr2);
                break;

            case 's': {
                /* Substitute: s/old/new/ */
                if (*p != '/') break;
                p++;
                char old[128], new[128];
                int oi = 0, ni = 0;
                while (*p && *p != '/' && oi < 127) old[oi++] = *p++;
                old[oi] = '\0';
                if (*p == '/') p++;
                while (*p && *p != '/' && ni < 127) new[ni++] = *p++;
                new[ni] = '\0';
                if (*p == '/') p++;
                int global = 0;
                if (*p == 'g') { global = 1; p++; }

                int replaced = 0;
                for (int i = addr1; i <= addr2 && i < ed_num_lines; i++) {
                    char *pos = strstr(ed_buffer[i], old);
                    if (pos) {
                        char before[ED_MAX_LINE_LEN];
                        strncpy(before, ed_buffer[i], (int)(pos - ed_buffer[i]));
                        before[(int)(pos - ed_buffer[i])] = '\0';
                        char after[ED_MAX_LINE_LEN];
                        strncpy(after, pos + strlen(old), ED_MAX_LINE_LEN - 1);
                        after[ED_MAX_LINE_LEN - 1] = '\0';
                        snprintf(ed_buffer[i], ED_MAX_LINE_LEN, "%s%s%s", before, new, after);
                        replaced++;
                        ed_current = i;
                        if (!global) break;
                    }
                }
                if (replaced) ed_modified = 1;
                kprintf("%d\n", (uint64_t)replaced);
                break;
            }

            case 'w':
            case 'W': {
                /* Write to file */
                static unsigned char write_buf[4096];
                uint32_t wpos = 0;
                for (int i = 0; i < ed_num_lines && wpos < sizeof(write_buf) - 2; i++) {
                    int len = strlen(ed_buffer[i]);
                    if (wpos + len + 1 > sizeof(write_buf)) break;
                    memcpy(write_buf + wpos, ed_buffer[i], len);
                    wpos += len;
                    write_buf[wpos++] = '\n';
                }
                libc_vfs_write(ed_filename, write_buf, wpos);
                kprintf("%d\n", (uint64_t)wpos);
                ed_modified = 0;
                break;
            }

            case 'q':
            case 'Q':
                if (ed_modified && cmd == 'q') {
                    kprintf("?\n"); /* modified, use Q to force quit */
                    break;
                }
                ed_running = 0;
                break;

            case 'h':
            case 'H':
                kprintf("ed commands: p(print), n(number), l(list), d(elete), s/substitute/,\n");
                kprintf("  w(rite), q(uit), Q(uit force), h(elp)\n");
                break;

            default:
                kprintf("?\n");
                break;
        }
    }
}
