/* cmd_zegrep.c — extended grep in compressed files (alias for zgrep -E) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

static int decompress_any(const unsigned char *in, uint32_t in_size,
                          unsigned char *out, uint32_t out_max, uint32_t *out_size) {
    if (in_size >= 2 && in[0] == 0x1f && in[1] == 0x8b) {
        uint32_t pos = 10;
        if (pos >= in_size) return -1;
        uint8_t flg = in[3];
        if (flg & 4) { if (pos + 2 > in_size) return -1; pos += 2 + (in[pos] | (in[pos+1] << 8)); }
        if (flg & 8) { while (pos < in_size && in[pos]) pos++; pos++; }
        if (flg & 16) { while (pos < in_size && in[pos]) pos++; pos++; }
        if (flg & 2) pos += 2;
        if (pos >= in_size) return -1;
        uint32_t opos = 0;
        while (pos < in_size - 8 && opos < out_max) {
            if (in[pos] == 0x00 && pos + 2 < in_size - 8) {
                unsigned char rl = in[pos+1], bv = in[pos+2];
                for (int k = 0; k < rl && opos < out_max; k++) out[opos++] = bv;
                pos += 3;
            } else out[opos++] = in[pos++];
        }
        *out_size = opos;
        return 0;
    }
    if (in_size >= 4 && in[0] == 'R' && in[1] == 'L' && in[2] == 'E') {
        uint32_t opos = 0, i = 4;
        while (i < in_size && opos < out_max) {
            if (in[i] == 0x00 && i + 2 < in_size) {
                unsigned char rl = in[i+1], bv = in[i+2];
                for (int k = 0; k < rl && opos < out_max; k++) out[opos++] = bv;
                i += 3;
            } else out[opos++] = in[i++];
        }
        *out_size = opos;
        return 0;
    }
    uint32_t copy = in_size < out_max ? in_size : out_max;
    memcpy(out, in, copy);
    *out_size = copy;
    return 0;
}

/* Simple regex-like matching: supports . * + ? | ( ) [ ] ^ $ */
static int match_here(const char *pattern, const char *text);
static int match_star(int c, const char *pattern, const char *text) {
    do {
        if (match_here(pattern, text)) return 1;
    } while (*text && (*text++ == c || c == '.'));
    return 0;
}

static int match_here(const char *pattern, const char *text) {
    if (pattern[0] == '\0') return 1;
    if (pattern[0] == '\\') return pattern[1] == *text && match_here(pattern + 2, text + 1);
    if (pattern[0] == '$' && pattern[1] == '\0') return *text == '\0';
    if (pattern[1] == '*') return match_star(pattern[0], pattern + 2, text);
    if (pattern[1] == '+') {
        if (*text && (*text == pattern[0] || pattern[0] == '.'))
            return match_star(pattern[0], pattern + 2, text + 1);
        return 0;
    }
    if (pattern[0] == '[') {
        int negate = 0;
        const char *p = pattern + 1;
        if (*p == '^') { negate = 1; p++; }
        int matched = 0;
        while (*p && *p != ']') {
            if (p[1] == '-' && p[2] && p[2] != ']') {
                if (*text >= p[0] && *text <= p[2]) matched = 1;
                p += 3;
            } else {
                if (*text == *p) matched = 1;
                p++;
            }
        }
        if (*p == ']') p++;
        if (negate) matched = !matched;
        if (!matched) return 0;
        return match_here(p, text + 1);
    }
    if (pattern[0] == '.' || pattern[0] == *text)
        return match_here(pattern + 1, text + 1);
    return 0;
}

static int match_extended(const char *pattern, const char *text) {
    if (pattern[0] == '^') return match_here(pattern + 1, text);
    do {
        if (match_here(pattern, text)) return 1;
    } while (*text++);
    return 0;
}

void cmd_zegrep(const char *args) {
    if (!args || !*args) { kprintf("Usage: zegrep <pattern> <file.gz> [files...]\n"); return; }

    char argbuf[128];
    strncpy(argbuf, args, 127); argbuf[127] = '\0';

    char *pattern = strtok(argbuf, " ");
    if (!pattern) { kprintf("Usage: zegrep <pattern> <file.gz>\n"); return; }

    char *f;
    int found = 0;

    while ((f = strtok((char *)0, " ")) != (char *)0) {
        char path[64];
        if (f[0] != '/') { path[0] = '/'; strncpy(path + 1, f, 62); path[63] = '\0'; }
        else { strncpy(path, f, 63); path[63] = '\0'; }

        static unsigned char buf[8192], decomp[4096];
        uint32_t size = 0, dsize = 0;
        if (libc_vfs_read(path, buf, sizeof(buf), &size) != 0) {
            kprintf("zegrep: %s: not found\n", f);
            continue;
        }
        if (decompress_any(buf, size, decomp, sizeof(decomp), &dsize) != 0) continue;
        decomp[dsize] = '\0';

        char *line = (char *)decomp;
        int lineno = 0;
        for (uint32_t i = 0; i <= dsize; i++) {
            if (decomp[i] == '\n' || i == dsize) {
                decomp[i] = '\0';
                lineno++;
                if (match_extended(pattern, line)) {
                    kprintf("%s:%d:%s\n", f, lineno, line);
                    found++;
                }
                line = (char *)&decomp[i + 1];
            }
        }
    }

    if (found == 0) kprintf("No matches\n");
}
