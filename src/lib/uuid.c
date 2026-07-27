#include "types.h"
#include "string.h"
#include "timer.h"
/* Generate a random UUID v4 using timer-based entropy */
void uuid_gen(uint8_t uuid[16]) {
    for (int i = 0; i < 16; i++) {
        uint64_t t = timer_get_ticks();
        uint64_t a = (uint64_t)(uintptr_t)&t;
        uuid[i] = (uint8_t)((t ^ a ^ (t >> 8) ^ (a >> 4)) & 0xFF);
    }
    uuid[6] = (uuid[6] & 0x0F) | 0x40; /* Version 4 */
    uuid[8] = (uuid[8] & 0x3F) | 0x80; /* Variant 1 */
}
void uuid_to_str(const uint8_t uuid[16], char out[37]) {
    const char *hex = "0123456789abcdef";
    int pos = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[pos++] = '-';
        out[pos++] = hex[(uuid[i] >> 4) & 0xF];
        out[pos++] = hex[uuid[i] & 0xF];
    }
    out[pos] = '\0';
}

/* ── uuid_generate ─────────────────────────────── */
int uuid_generate(void *uuid)
{
    if (!uuid)
        return -1;
    uuid_gen((uint8_t *)uuid);
    return 0;
}
/* ── uuid_parse ─────────────────────────────── */
int uuid_parse(const char *str, void *uuid)
{
    if (!str || !uuid)
        return -1;
    uint8_t *u = (uint8_t *)uuid;
    int idx = 0;
    for (int i = 0; i < 36 && idx < 32; i++) {
        char c = str[i];
        if (c == '-')
            continue;
        uint8_t nibble = 0;
        if (c >= '0' && c <= '9')
            nibble = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            nibble = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            nibble = (uint8_t)(c - 'A' + 10);
        else
            return -1;
        if (idx & 1)
            u[idx >> 1] |= nibble;
        else
            u[idx >> 1] = nibble << 4;
        idx++;
    }
    return (idx == 32) ? 0 : -1;
}
/* ── uuid_unparse ─────────────────────────────── */
int uuid_unparse(const void *uuid, char *str)
{
    if (!uuid || !str)
        return -1;
    uuid_to_str((const uint8_t *)uuid, str);
    return 0;
}
