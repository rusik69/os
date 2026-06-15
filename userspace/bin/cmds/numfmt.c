/* numfmt.c — convert numbers to/from IEC human-readable format */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static unsigned long long iec_suffix_mult(const char *s) {
    /* Find end of number, then check suffix */
    unsigned long len = strlen(s);
    if (len == 0) return 0;

    char suffix = s[len - 1];
    /* Check if last char is a letter */
    if (suffix >= 'A' && suffix <= 'Z') {
        switch (suffix) {
            case 'K': return 1024ULL;
            case 'M': return 1024ULL * 1024;
            case 'G': return 1024ULL * 1024 * 1024;
            case 'T': return 1024ULL * 1024 * 1024 * 1024;
            case 'P': return 1024ULL * 1024 * 1024 * 1024 * 1024;
            default: return 0;
        }
    } else if (suffix >= 'a' && suffix <= 'z') {
        switch (suffix) {
            case 'k': return 1024ULL;
            case 'm': return 1024ULL * 1024;
            case 'g': return 1024ULL * 1024 * 1024;
            case 't': return 1024ULL * 1024 * 1024 * 1024;
            case 'p': return 1024ULL * 1024 * 1024 * 1024 * 1024;
            default: return 0;
        }
    }
    return 1; /* No suffix = bytes */
}

static unsigned long long parse_iec(const char *s) {
    unsigned long long val = 0;
    int has_dot = 0;
    unsigned long long frac = 0;
    int frac_digits = 0;
    int frac_mult = 1;

    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }

    if (*s == '.') {
        s++;
        has_dot = 1;
        while (*s >= '0' && *s <= '9') {
            frac = frac * 10 + (*s - '0');
            frac_digits++;
            frac_mult *= 10;
            s++;
        }
    }

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t') s++;

    unsigned long long mult = iec_suffix_mult(s);
    if (mult == 0) {
        printf("numfmt: invalid suffix\n");
        return 0;
    }

    val = val * mult;
    if (has_dot && mult > 1) {
        /* Add fractional part */
        unsigned long long frac_val = (frac * mult) / frac_mult;
        val += frac_val;
    }

    return val;
}

static int fmt_iec(unsigned long long bytes, char *buf, int bufsz) {
    const char *suffixes[] = {"B", "K", "M", "G", "T", "P", "E"};
    int si = 0;
    unsigned long long val = bytes;

    while (val >= 1024 && si < 6) {
        val /= 1024;
        si++;
    }

    if (si == 0) {
        return snprintf(buf, bufsz, "%llu", bytes);
    }

    /* Show with one decimal if not exact */
    unsigned long long divisor = 1;
    for (int i = 0; i < si; i++) divisor *= 1024;
    unsigned long long whole = bytes / divisor;
    unsigned long long rem = bytes % divisor;
    unsigned long long dec = (rem * 10) / divisor;

    if (dec > 0)
        return snprintf(buf, bufsz, "%llu.%llu%s", whole, dec, suffixes[si]);
    else
        return snprintf(buf, bufsz, "%llu%s", whole, suffixes[si]);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage: numfmt --from=iec <number>\n");
        printf("       numfmt --to=iec <number>\n");
        printf("Suffixes: K=1024, M=1048576, G, T\n");
        return 1;
    }

    if (strcmp(argv[1], "--from=iec") == 0) {
        if (argc < 3) {
            printf("numfmt: missing number\n");
            return 1;
        }
        unsigned long long result = parse_iec(argv[2]);
        if (result == 0 && argv[2][0] != '0') {
            printf("numfmt: invalid number '%s'\n", argv[2]);
            return 1;
        }
        printf("%llu\n", result);
        return 0;
    }

    if (strcmp(argv[1], "--to=iec") == 0) {
        if (argc < 3) {
            printf("numfmt: missing number\n");
            return 1;
        }
        unsigned long long bytes = 0;
        const char *p = argv[2];
        while (*p >= '0' && *p <= '9') {
            bytes = bytes * 10 + (*p - '0');
            p++;
        }
        if (*p != '\0') {
            printf("numfmt: invalid number '%s'\n", argv[2]);
            return 1;
        }

        char buf[32];
        fmt_iec(bytes, buf, sizeof(buf));
        printf("%s\n", buf);
        return 0;
    }

    printf("numfmt: unknown option '%s'\n", argv[1]);
    return 1;
}
