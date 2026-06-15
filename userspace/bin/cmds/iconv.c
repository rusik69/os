/* iconv.c — character set conversion */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* Simple charset conversion between ASCII, UTF-8, ISO-8859-1 */

/* Check if UTF-8 sequence is valid and return codepoint */
static int utf8_decode(const char *s, unsigned long *codepoint) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *codepoint = c;
        return 1;
    } else if (c < 0xC0) {
        return -1; /* Continuation byte as start */
    } else if (c < 0xE0) {
        if ((s[1] & 0xC0) != 0x80) return -1;
        *codepoint = ((unsigned long)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    } else if (c < 0xF0) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return -1;
        *codepoint = ((unsigned long)(c & 0x0F) << 12) |
                     ((unsigned long)(s[1] & 0x3F) << 6) |
                     (s[2] & 0x3F);
        return 3;
    } else if (c < 0xF8) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return -1;
        *codepoint = ((unsigned long)(c & 0x07) << 18) |
                     ((unsigned long)(s[1] & 0x3F) << 12) |
                     ((unsigned long)(s[2] & 0x3F) << 6) |
                     (s[3] & 0x3F);
        return 4;
    }
    return -1;
}

/* Encode codepoint as UTF-8 */
static int utf8_encode(unsigned long cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = 0xC0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    } else if (cp < 0x10000) {
        out[0] = 0xE0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    } else if (cp < 0x110000) {
        out[0] = 0xF0 | (cp >> 18);
        out[1] = 0x80 | ((cp >> 12) & 0x3F);
        out[2] = 0x80 | ((cp >> 6) & 0x3F);
        out[3] = 0x80 | (cp & 0x3F);
        return 4;
    }
    return -1;
}

static int strcaseeq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Convert buffer from one charset to another */
static int convert(const char *from, const char *to,
                   const char *inbuf, unsigned long inlen,
                   char *outbuf, unsigned long *outlen) {
    unsigned long out_pos = 0;
    unsigned long in_pos = 0;

    int to_utf8 = strcaseeq(to, "UTF-8") || strcaseeq(to, "utf8");
    int to_latin1 = strcaseeq(to, "ISO-8859-1") || strcaseeq(to, "LATIN1");

    int from_utf8 = strcaseeq(from, "UTF-8") || strcaseeq(from, "utf8");
    int from_latin1 = strcaseeq(from, "ISO-8859-1") || strcaseeq(from, "LATIN1");

    while (in_pos < inlen) {
        unsigned long codepoint;
        int consumed;

        if (from_utf8) {
            consumed = utf8_decode(inbuf + in_pos, &codepoint);
            if (consumed < 0) { /* Invalid UTF-8 */
                codepoint = 0xFFFD; /* Replacement character */
                consumed = 1;
            }
        } else if (from_latin1) {
            codepoint = (unsigned char)inbuf[in_pos];
            consumed = 1;
        } else { /* ASCII or default */
            codepoint = (unsigned char)inbuf[in_pos];
            consumed = 1;
        }

        in_pos += (unsigned long)consumed;

        /* Encode to output charset */
        if (to_utf8) {
            int n = utf8_encode(codepoint, outbuf + out_pos);
            if (n > 0) out_pos += (unsigned long)n;
        } else if (to_latin1) {
            if (codepoint < 0x100) {
                outbuf[out_pos++] = (char)codepoint;
            } else {
                outbuf[out_pos++] = '?'; /* Replace non-Latin1 */
            }
        } else { /* ASCII */
            if (codepoint < 0x80) {
                outbuf[out_pos++] = (char)codepoint;
            } else {
                outbuf[out_pos++] = '?'; /* Replace non-ASCII */
            }
        }

        if (out_pos + 8 > *outlen) break; /* Prevent overflow */
    }

    *outlen = out_pos;
    return 0;
}

int main(int argc, char *argv[]) {
    const char *from = NULL;
    const char *to = NULL;
    const char *file = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            from = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            to = argv[++i];
        } else if (argv[i][0] == '-') {
            printf("iconv: invalid option '%s'\n", argv[i]);
            printf("Usage: iconv -f <from> -t <to> [<file>]\n");
            return 1;
        } else {
            file = argv[i];
        }
    }

    if (!from || !to) {
        printf("Usage: iconv -f <from> -t <to> [<file>]\n");
        return 1;
    }

    int fd;
    if (file) {
        fd = open(file, O_RDONLY, 0);
        if (fd < 0) {
            printf("iconv: cannot open '%s'\n", file);
            return 1;
        }
    } else {
        fd = 0; /* stdin */
    }

    /* Read input */
    char inbuf[16384];
    unsigned long inlen = 0;
    int n;
    while ((n = read(fd, inbuf + inlen, sizeof(inbuf) - inlen)) > 0) {
        inlen += (unsigned long)n;
        if (inlen >= sizeof(inbuf)) break;
    }

    if (file) close(fd);

    /* Convert */
    char outbuf[65536];
    unsigned long outlen = sizeof(outbuf);
    convert(from, to, inbuf, inlen, outbuf, &outlen);

    /* Write output */
    write(1, outbuf, outlen);

    return 0;
}
