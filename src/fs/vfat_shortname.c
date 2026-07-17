/* vfat_shortname.c — FAT32 VFAT 8.3 short name generation
 *
 * Generates 8.3 short filenames from long filenames according to
 * the VFAT specification (Microsoft extension to FAT).
 *
 * Algorithm:
 *   1. Strip invalid characters, convert to uppercase
 *   2. Truncate name to 8 chars, extension to 3 chars
 *   3. Generate unique numeric tail (~N) on collision
 */

#include "types.h"
#include "fat32.h"
#include "string.h"
#include "printf.h"

/* ── Character classification for short names ────────────────────── */

/* Valid characters for 8.3 short names (FAT specification) */
#define SHORTNAME_VALID_UPPER "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~"

static int is_valid_short_char(char c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    /* Valid special characters */
    const char *special = "!#$%&'()-@^_`{}~";
    while (*special) {
        if (c == *special) return 1;
        special++;
    }
    return 0;
}

/* ── char_to_upper ───────────────────────────────────────────────── */

static char char_to_upper(char c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    return c;
}

/* ── vfat_generate_short_name ──────────────────────────────────────
 *
 * Generate an 8.3 short name from a long filename.
 *
 * Parameters:
 *   long_name  — the long filename (null-terminated)
 *   short_out  — output buffer for the 8.3 name (must be 12+ bytes:
 *                8 bytes name + '.' + 3 bytes ext + null)
 *   existing   — callback to check if a name already exists
 *                (or NULL if no collision checking needed)
 *
 * Returns 0 on success, -1 on error.
 */
static int vfat_generate_short_name(const char *long_name, char *short_out,
                              int (*existing)(const char *short_name))
{
    if (!long_name || !short_out)
        return -1;

    /* Temporary buffers */
    char name_part[9];   /* 8 chars + null */
    char ext_part[4];    /* 3 chars + null */
    int name_len = 0;
    int ext_len = 0;

    memset(name_part, 0, sizeof(name_part));
    memset(ext_part, 0, sizeof(ext_part));

    /* Find the last dot for extension */
    const char *dot = strrchr(long_name, '.');
    const char *name_start = long_name;
    const char *ext_start = NULL;

    if (dot && dot > long_name) {
        /* Has extension */
        size_t base_len = (size_t)(dot - long_name);
        ext_start = dot + 1;

        /* Copy and sanitize name part (max 8 chars) */
        for (size_t i = 0; i < base_len && name_len < 8; i++) {
            char c = char_to_upper(long_name[i]);
            if (c == '.' || c == ' ') continue; /* skip dots and spaces in name */
            if (is_valid_short_char(c) || c == '_') {
                name_part[name_len++] = c;
            } else {
                name_part[name_len++] = '_';
            }
        }

        /* Copy and sanitize extension (max 3 chars) */
        for (size_t i = 0; ext_start[i] && ext_len < 3; i++) {
            char c = char_to_upper(ext_start[i]);
            if (is_valid_short_char(c) || c == '_') {
                ext_part[ext_len++] = c;
            } else {
                ext_part[ext_len++] = '_';
            }
        }
    } else {
        /* No extension */
        for (int i = 0; long_name[i] && name_len < 8; i++) {
            char c = char_to_upper(long_name[i]);
            if (c == '.' || c == ' ') continue;
            if (is_valid_short_char(c) || c == '_') {
                name_part[name_len++] = c;
            } else {
                name_part[name_len++] = '_';
            }
        }
    }

    /* Ensure we have at least something */
    if (name_len == 0 && ext_len == 0) {
        /* Generate a fallback name */
        strncpy(name_part, "DUMMY", sizeof(name_part) - 1);
        name_part[sizeof(name_part) - 1] = '\0';
        name_len = 5;
    }
    if (name_len == 0) {
        name_part[0] = '_';
        name_len = 1;
    }

    /* Build the 8.3 name: NAME.EXT */
    char candidate[13];
    memset(candidate, 0, sizeof(candidate));

    int pos = 0;
    for (int i = 0; i < name_len; i++)
        candidate[pos++] = name_part[i];

    if (ext_len > 0) {
        candidate[pos++] = '.';
        for (int i = 0; i < ext_len; i++)
            candidate[pos++] = ext_part[i];
    }
    candidate[pos] = '\0';

    /* Check for collision and uniquify if needed */
    if (existing) {
        int suffix = 1;
        char test[13];
        strncpy(test, candidate, sizeof(test) - 1);
        test[sizeof(test) - 1] = '\0';

        while (existing(test)) {
            /* Generate ~N suffix: trim name to 6 chars + ~N */
            int suffix_len = snprintf(NULL, 0, "~%d", suffix);
            int available = 8 - suffix_len;
            if (available < 1) available = 1;

            memset(test, 0, sizeof(test));
            pos = 0;
            int copy_len = name_len < available ? name_len : available;
            for (int i = 0; i < copy_len; i++)
                test[pos++] = name_part[i];
            pos += snprintf(test + pos, 12 - pos, "~%d", suffix);

            if (ext_len > 0) {
                test[pos++] = '.';
                for (int i = 0; i < ext_len; i++)
                    test[pos++] = ext_part[i];
            }
            test[pos] = '\0';

            suffix++;
            if (suffix > 99999) {
                /* Give up — use a hash of the actual filename content */
                uint32_t hash = 0;
                for (const char *p = long_name; *p; p++)
                    hash = hash * 31 + (uint8_t)(*p);
                snprintf(test, sizeof(test), "~%05X.%03X",
                         (unsigned int)(hash & 0xFFFFF),
                         (unsigned int)((hash >> 20) & 0xFFF));
                break;
            }
        }

        strncpy(short_out, test, 12);
        short_out[12] = '\0';
    } else {
        strncpy(short_out, candidate, 12);
        short_out[12] = '\0';
    }

    return 0;
}

/* ── vfat_shortname_create ──────────────────────────────── */
static int vfat_shortname_create(const char *long_name, char *short_name)
{
    if (!long_name || !short_name) return -EINVAL;
    /* Simple short name generation: uppercase first 6 chars + ~1 */
    int i = 0;
    while (long_name[i] && i < 6) {
        char c = long_name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        short_name[i] = c;
        i++;
    }
    short_name[i] = '~';
    short_name[i+1] = '1';
    short_name[i+2] = '\0';
    return 0;
}

/* ── vfat_shortname_checksum ────────────────────────────── */
static int vfat_shortname_checksum(const char *short_name)
{
    if (!short_name) return 0;
    unsigned char sum = 0;
    for (int i = 0; short_name[i]; i++) {
        sum = (unsigned char)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (unsigned char)short_name[i]);
    }
    return (int)sum;
}

/* ── vfat_shortname_match ───────────────────────────────── */
static int vfat_shortname_match(const char *short_name, const char *long_name)
{
    if (!short_name || !long_name) return 0;
    /* Build an 8.3 short name from the long name, then compare
     * case-insensitively against the given short_name */
    char n8[8];
    char n3[3];
    vfat_build_83_name(long_name, n8, n3);

    /* Compare: skip spaces in name/extension (space = no char) */
    int si = 0, ni = 0;
    while (short_name[si] && short_name[si] != '.' && ni < 8) {
        char a = short_name[si];
        char b = n8[ni];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (a != b) return 0;
        si++;
        ni++;
    }
    /* Skip the dot */
    if (short_name[si] == '.') si++;

    /* Compare extension */
    ni = 0;
    while (short_name[si] && ni < 3) {
        char a = short_name[si];
        char b = n3[ni];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (a != b) return 0;
        si++;
        ni++;
    }
    /* Both should be exhausted */
    return (short_name[si] == '\0' || short_name[si] == '.') ? 1 : 0;
}
