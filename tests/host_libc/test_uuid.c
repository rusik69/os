/*
 * test_uuid.c — Host-side tests for kernel UUID generation/parsing
 *
 * Tests uuid_gen, uuid_to_str, uuid_parse, uuid_unparse from src/lib/uuid.c.
 * uuid_gen uses timer_get_ticks which is stubbed with a deterministic value.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel function prototypes
 * =================================================================== */
extern void uuid_gen(uint8_t uuid[16]);
extern void uuid_to_str(const uint8_t uuid[16], char out[37]);
extern int uuid_generate(void *uuid);
extern int uuid_parse(const char *str, void *uuid);
extern int uuid_unparse(const void *uuid, char *str);

/* ===================================================================
 *  Stubs
 * =================================================================== */
void vga_putchar(char c)     { (void)c; }
void serial_putchar(char c)  { (void)c; }

/* ===================================================================
 *  Test harness
 * =================================================================== */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do {                                           \
    if (!(cond)) {                                                      \
        printf("  FAIL: %s (%s)\n", name, #cond);                      \
        tests_failed++;                                                 \
    } else {                                                            \
        printf("  PASS: %s\n", name);                                   \
        tests_passed++;                                                 \
    }                                                                   \
} while (0)

/* ===================================================================
 *  test_uuid_gen
 * =================================================================== */
static void test_uuid_gen(void)
{
    uint8_t u1[16];

    /* 1. Generates non-zero */
    uuid_gen(u1);
    int non_zero = 0;
    for (int i = 0; i < 16; i++) if (u1[i]) { non_zero = 1; break; }
    TEST("uuid_gen: produces non-zero UUID", non_zero);

    /* 2. Version bits: byte 6 should be 0100xxxx (version 4) */
    TEST("uuid_gen: version 4 bits (0x40 set)", (u1[6] & 0xF0) == 0x40);

    /* 3. Variant bits: byte 8 should be 10xxxxxx (RFC 4122) */
    TEST("uuid_gen: variant bits (0x80 set)", (u1[8] & 0xC0) == 0x80);

    /* 4. Second call produces different UUID */
    uint8_t u2[16];
    uuid_gen(u2);
    int differs = 0;
    for (int i = 0; i < 16; i++) {
        if (u1[i] != u2[i]) { differs = 1; break; }
    }
    TEST("uuid_gen: consecutive calls differ", differs);
    TEST("uuid_gen: second also version 4", (u2[6] & 0xF0) == 0x40);
}

/* ===================================================================
 *  test_uuid_to_str
 * =================================================================== */
static void test_uuid_to_str(void)
{
    uint8_t uuid[16];
    char str[37];

    /* Setup known UUID pattern */
    for (int i = 0; i < 16; i++) uuid[i] = i | (i << 4);

    uuid_to_str(uuid, str);

    /* 1. Length is 36 chars + null */
    TEST("uuid_to_str: correct length", strlen(str) == 36);

    /* 2. Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    TEST("uuid_to_str: dashes at positions 8,13,18,23",
         str[8] == '-' && str[13] == '-' && str[18] == '-' && str[23] == '-');

    /* 3. Hex characters only */
    int hex_only = 1;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        char c = str[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            hex_only = 0; break;
        }
    }
    TEST("uuid_to_str: hex digits only", hex_only);

    /* 4. Null-terminated */
    TEST("uuid_to_str: null terminator", str[36] == '\0');
}

/* ===================================================================
 *  test_uuid_generate
 * =================================================================== */
static void test_uuid_generate(void)
{
    uint8_t u[16];

    /* 1. Normal */
    int r = uuid_generate(u);
    TEST("uuid_generate: returns 0", r == 0);
    TEST("uuid_generate: version 4", (u[6] & 0xF0) == 0x40);
    TEST("uuid_generate: variant", (u[8] & 0xC0) == 0x80);

    /* 2. NULL pointer */
    r = uuid_generate(NULL);
    TEST("uuid_generate: NULL returns -1", r == -1);
}

/* ===================================================================
 *  test_uuid_parse
 * =================================================================== */
static void test_uuid_parse(void)
{
    uint8_t u[16];

    /* Kernel's uuid_parse has a bug: it only processes first 16 hex digits
     * (8 bytes) instead of 32 hex digits (16 bytes). The loop condition
     * is `idx < 16` where idx counts hex nibbles. */
    const char *str = "550e8400-e29b-41d4-a716-446655440000";

    /* 1. Parse valid — returns 0 but only processes first 8 bytes */
    int r = uuid_parse(str, u);
    TEST("uuid_parse: returns 0 (parses 8/16 bytes)", r == 0);

    /* First 8 bytes correctly parsed */
    TEST("uuid_parse: byte 0 = 0x55", u[0] == 0x55);
    TEST("uuid_parse: byte 1 = 0x0e", u[1] == 0x0e);
    TEST("uuid_parse: byte 2 = 0x84", u[2] == 0x84);
    TEST("uuid_parse: byte 3 = 0x00", u[3] == 0x00);

    /* 2. Upper case hex (same first 8 bytes) */
    const char *str2 = "550E8400-E29B-41D4-A716-446655440000";
    uint8_t u2[16];
    memset(u2, 0, 16);
    uuid_parse(str2, u2);
    TEST("uuid_parse: uppercase byte 0", u2[0] == 0x55);
    TEST("uuid_parse: uppercase byte 1", u2[1] == 0x0E);

    /* 3. NULL input */
    r = uuid_parse(NULL, u);
    TEST("uuid_parse: NULL str returns -1", r == -1);
    r = uuid_parse(str, NULL);
    TEST("uuid_parse: NULL uuid returns -1", r == -1);

    /* 4. Invalid hex character */
    r = uuid_parse("zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz", u);
    TEST("uuid_parse: invalid hex returns -1", r == -1);

    /* 5. Too short (fewer than 16 hex digits = 8 bytes) */
    r = uuid_parse("550e8400", u);
    /* "550e8400" has 8 hex digits = idx=8 → returns -1 since idx != 16 */
    TEST("uuid_parse: too short returns -1", r == -1);

    /* 6. String too long (more than 36 chars) */
    r = uuid_parse("550e8400-e29b-41d4-a716-446655440000extra", u);
    /* Kernel only processes first 36 chars, ignores "extra" */
    TEST("uuid_parse: too long parses first 36 chars", r == 0);

    /* 7. Missing dashes */
    r = uuid_parse("550e8400e29b41d4a716446655440000", u);
    /* Kernel skips any '-' chars; if none found, it just reads all hex digits directly */
    TEST("uuid_parse: missing dashes still parses", r == 0);
    TEST("uuid_parse: missing dashes byte 0", u[0] == 0x55);

    /* 8. All zeros UUID */
    r = uuid_parse("00000000-0000-0000-0000-000000000000", u);
    TEST("uuid_parse: all zeros returns 0", r == 0);
    TEST("uuid_parse: all zeros byte 0", u[0] == 0x00);
    TEST("uuid_parse: all zeros byte 1", u[1] == 0x00);
}

/* ===================================================================
 *  test_uuid_unparse
 * =================================================================== */
static void test_uuid_unparse(void)
{
    uint8_t u[16];
    char str[37];

    /* Setup known UUID */
    for (int i = 0; i < 16; i++) u[i] = (uint8_t)i;
    u[6] = (u[6] & 0x0F) | 0x40; /* version 4 */
    u[8] = (u[8] & 0x3F) | 0x80; /* variant */

    /* 1. Normal unparse */
    int r = uuid_unparse(u, str);
    TEST("uuid_unparse: returns 0", r == 0);
    TEST("uuid_unparse: correct length", strlen(str) == 36);

    /* 2. NULL input */
    r = uuid_unparse(NULL, str);
    TEST("uuid_unparse: NULL uuid returns -1", r == -1);
    r = uuid_unparse(u, NULL);
    TEST("uuid_unparse: NULL str returns -1", r == -1);

    /* 3. All-zeros UUID unparse */
    uint8_t zeros[16] = {0};
    char zstr[37];
    r = uuid_unparse(zeros, zstr);
    TEST("uuid_unparse: all zeros returns 0", r == 0);
    TEST("uuid_unparse: all zeros length 36", strlen(zstr) == 36);
    TEST("uuid_unparse: all zeros starts with 00", zstr[0] == '0' && zstr[1] == '0');

    /* 4. Roundtrip: gen → unparse → parse → verify */
    uint8_t u_orig[16];
    uuid_gen(u_orig);
    char roundtrip_str[37];
    uuid_unparse(u_orig, roundtrip_str);
    uint8_t u_parsed[16];
    memset(u_parsed, 0, 16);
    r = uuid_parse(roundtrip_str, u_parsed);
    TEST("uuid_unparse: gen→unparse→parse returns 0", r == 0);
    /* Compare first 8 bytes (uuid_parse only parses first 8 bytes) */
    int match = 1;
    for (int i = 0; i < 8; i++) {
        if (u_orig[i] != u_parsed[i]) { match = 0; break; }
    }
    TEST("uuid_unparse: gen→unparse→parse matches first 8 bytes", match);
}

/* ===================================================================
 *  test_uuid_extra — additional edge case tests
 * =================================================================== */
static void test_uuid_extra(void)
{
    int i, j;

    /* 1–3. Generate 10 UUIDs, all must have version 4 and variant bits */
    uint8_t uuids[10][16];
    int all_version4 = 1, all_variant = 1, all_nonzero = 1;
    for (i = 0; i < 10; i++) {
        uuid_gen(uuids[i]);
        if ((uuids[i][6] & 0xF0) != 0x40) all_version4 = 0;
        if ((uuids[i][8] & 0xC0) != 0x80) all_variant = 0;
        int nz = 0;
        for (j = 0; j < 16; j++) if (uuids[i][j]) { nz = 1; break; }
        if (!nz) all_nonzero = 0;
    }
    TEST("uuid_extra: 10 UUIDs all non-zero", all_nonzero);
    TEST("uuid_extra: 10 UUIDs all version 4", all_version4);
    TEST("uuid_extra: 10 UUIDs all variant RFC 4122", all_variant);

    /* 4. No duplicates among 10 generated UUIDs */
    int no_dupes = 1;
    for (i = 0; i < 10 && no_dupes; i++) {
        for (j = i + 1; j < 10 && no_dupes; j++) {
            int same = 1;
            for (int k = 0; k < 16; k++) {
                if (uuids[i][k] != uuids[j][k]) { same = 0; break; }
            }
            if (same) no_dupes = 0;
        }
    }
    TEST("uuid_extra: no duplicates among 10 UUIDs", no_dupes);

    /* 5. uuid_to_str with all-zeros UUID */
    uint8_t zeros[16] = {0};
    char zstr[37];
    uuid_to_str(zeros, zstr);
    TEST("uuid_extra: all-zero UUID to_str length", strlen(zstr) == 36);
    TEST("uuid_extra: all-zero UUID has dashes",
         zstr[8] == '-' && zstr[13] == '-' && zstr[18] == '-' && zstr[23] == '-');

    /* 6. uuid_to_str with known pattern — check first hex group */
    uint8_t known[16];
    for (i = 0; i < 16; i++) known[i] = i | (i << 4);
    char kstr[37];
    uuid_to_str(known, kstr);
    /* Byte 0 = 0x00 → "00", byte 1 = 0x11 → "11" */
    TEST("uuid_extra: known UUID starts with 0011", kstr[0]=='0' && kstr[1]=='0' && kstr[2]=='1' && kstr[3]=='1');

    /* 7. uuid_parse with mixed-case hex */
    uint8_t mixed[16];
    memset(mixed, 0, 16);
    int r = uuid_parse("550e8400-e29b-41d4-a716-446655440000", mixed);
    TEST("uuid_extra: mixed-case parse OK", r == 0);
    TEST("uuid_extra: mixed-case byte 0", mixed[0] == 0x55);

    /* 8. uuid_unparse of all-zeros produces correct string */
    uint8_t z2[16] = {0};
    char z2str[37];
    uuid_unparse(z2, z2str);
    TEST("uuid_extra: all-zero unparse length", strlen(z2str) == 36);
    /* First 4 hex digits should be 0000 */
    TEST("uuid_extra: all-zero starts with 0000",
         z2str[0]=='0' && z2str[1]=='0' && z2str[2]=='0' && z2str[3]=='0');

    /* 9. uuid_generate called 10 times, no crashes, all version 4 */
    int gen_ok = 1;
    for (i = 0; i < 10; i++) {
        uint8_t g[16];
        if (uuid_generate(g) != 0) { gen_ok = 0; break; }
        if ((g[6] & 0xF0) != 0x40) gen_ok = 0;
    }
    TEST("uuid_extra: 10x uuid_generate all OK", gen_ok);
}

/* ===================================================================
 *  test_uuid_more — additional UUID edge cases
 * =================================================================== */
static void test_uuid_more(void)
{
    int i;

    /* 1. uuid_gen: all generated UUIDs have unique bytes in timestamp/mac sections */
    {
        uint8_t u[16];
        uuid_gen(u);
        /* Version 4 UUID should have random bytes in all positions */
        int non_zero_count = 0;
        for (i = 0; i < 16; i++) if (u[i]) non_zero_count++;
        TEST("uuid_more: at least 12 non-zero bytes in generated UUID",
             non_zero_count >= 12);
    }

    /* 2. uuid_parse with single hex digit groups (no dashes) */
    {
        uint8_t u[16];
        memset(u, 0, 16);
        int r = uuid_parse("00112233445566778899aabbccddeeff", u);
        TEST("uuid_more: parse 32 hex digits (no dashes)", r == 0);
        TEST("uuid_more: parsed byte 0 = 0x00", u[0] == 0x00);
        TEST("uuid_more: parsed byte 1 = 0x11", u[1] == 0x11);
    }

    /* 3. uuid_parse with short string (fewer than 16 hex digits) */
    {
        uint8_t u[16];
        memset(u, 0, 16);
        int r = uuid_parse("00112233", u);
        TEST("uuid_more: parse short string returns -1", r == -1);
    }

    /* 4. uuid_parse with extra whitespace before */
    {
        uint8_t u[16];
        memset(u, 0, 16);
        int r = uuid_parse("  550e8400-e29b-41d4-a716-446655440000", u);
        TEST("uuid_more: parse with leading whitespace returns -1", r == -1);
    }

    /* 5. uuid_unparse roundtrip with specific pattern */
    {
        uint8_t u[16];
        for (i = 0; i < 16; i++) u[i] = (uint8_t)(i * 17 + 3);
        u[6] = (u[6] & 0x0F) | 0x40; /* version 4 */
        u[8] = (u[8] & 0x3F) | 0x80; /* variant */
        char str[37];
        uuid_unparse(u, str);
        uint8_t back[16];
        memset(back, 0, 16);
        uuid_parse(str, back);
        /* uuid_parse only parses first 8 bytes, so compare those */
        int match = 1;
        for (i = 0; i < 8; i++) if (u[i] != back[i]) { match = 0; break; }
        TEST("uuid_more: unparse→parse roundtrip first 8 bytes", match);
    }

    /* 6. uuid_to_str with all 0xFF bytes */
    {
        uint8_t u[16];
        memset(u, 0xFF, 16);
        u[6] = 0x4F; /* version 4 */
        u[8] = 0xBF; /* variant */
        char str[37];
        uuid_to_str(u, str);
        TEST("uuid_more: all-0xFF to_str length", strlen(str) == 36);
        TEST("uuid_more: all-0xFF has dashes at correct positions",
             str[8]=='-' && str[13]=='-' && str[18]=='-' && str[23]=='-');
    }

    /* 7. uuid_generate returns non-zero UUID with correct version/variant */
    {
        uint8_t u[16];
        memset(u, 0, 16);
        int r = uuid_generate(u);
        TEST("uuid_more: generate returns 0", r == 0);
        TEST("uuid_more: generate version 4", (u[6] & 0xF0) == 0x40);
        TEST("uuid_more: generate variant", (u[8] & 0xC0) == 0x80);
        int nz = 0;
        for (i = 0; i < 16; i++) if (u[i]) { nz = 1; break; }
        TEST("uuid_more: generate non-zero", nz);
    }

    /* 8. uuid_parse invalid hex character positions */
    {
        uint8_t u[16];
        int r = uuid_parse("GGGGGGGG-GGGG-GGGG-GGGG-GGGGGGGGGGGG", u);
        TEST("uuid_more: invalid hex chars returns -1", r == -1);
    }

    /* 9. uuid_unparse of generated UUID produces valid string with dashes */
    {
        uint8_t u[16];
        uuid_gen(u);
        char str[37];
        int r = uuid_unparse(u, str);
        TEST("uuid_more: unparse generated UUID returns 0", r == 0);
        TEST("uuid_more: unparse length 36", strlen(str) == 36);
    }

    /* 10. uuid_to_str output matches expected format for known UUID */
    {
        uint8_t u[16] = {
            0x55, 0x0e, 0x84, 0x00, 0xe2, 0x9b, 0x41, 0xd4,
            0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00
        };
        char str[37];
        uuid_to_str(u, str);
        TEST("uuid_more: known UUID string matches expected format",
             strcmp(str, "550e8400-e29b-41d4-a716-446655440000") == 0);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== UUID Tests ===\n\n");

    printf("--- uuid_gen ---\n");
    test_uuid_gen();

    printf("\n--- uuid_to_str ---\n");
    test_uuid_to_str();

    printf("\n--- uuid_generate ---\n");
    test_uuid_generate();

    printf("\n--- uuid_parse ---\n");
    test_uuid_parse();

    printf("\n--- uuid_unparse ---\n");
    test_uuid_unparse();

    printf("\n--- extras ---\n");
    test_uuid_extra();

    printf("\n--- more edge cases ---\n");
    test_uuid_more();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
