#include "printf.h"
#include "string.h"
void print_hex_dump(const char *prefix, const void *buf, uint32_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i += 16) {
        kprintf("%s%04x: ", prefix ? prefix : "", i);
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < len) kprintf("%02x ", p[i + j]);
            else kprintf("   ");
            if (j == 7) kprintf(" ");
        }
        kprintf(" |");
        for (uint32_t j = 0; j < 16 && i + j < len; j++)
            kprintf("%c", p[i+j] >= 32 && p[i+j] < 127 ? p[i+j] : '.');
        kprintf("|\n");
    }
}

/* ── hexdump_print ─────────────────────────────── */
int hexdump_print(const void *buf, size_t len)
{
    print_hex_dump("", buf, (uint32_t)len);
    return 0;
}
/* ── hexdump_to_buf ─────────────────────────────── */
int hexdump_to_buf(const void *buf, size_t len, char *out, size_t out_len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 5 < out_len; i += 16) {
        pos += snprintf(out + pos, out_len - pos, "%04lx: ", (unsigned long)i);
        for (size_t j = 0; j < 16; j++) {
            if (pos + 3 >= out_len) break;
            if (i + j < len)
                pos += snprintf(out + pos, out_len - pos, "%02x ", p[i + j]);
            else
                pos += snprintf(out + pos, out_len - pos, "   ");
            if (j == 7 && pos < out_len)
                out[pos++] = ' ';
        }
        if (pos + 2 >= out_len) break;
        out[pos++] = '|';
        for (size_t j = 0; j < 16 && i + j < len && pos < out_len; j++)
            out[pos++] = (p[i+j] >= 32 && p[i+j] < 127) ? p[i+j] : '.';
        if (pos < out_len) out[pos++] = '|';
        if (pos < out_len) out[pos++] = '\n';
    }
    if (pos < out_len) out[pos] = '\0';
    return (int)pos;
}
/* ── hexdump_ascii ─────────────────────────────── */
int hexdump_ascii(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    kprintf("[hexdump] ");
    for (size_t i = 0; i < len; i++)
        kprintf("%c", (p[i] >= 32 && p[i] < 127) ? p[i] : '.');
    kprintf("\n");
    return 0;
}
