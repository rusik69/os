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

/* ── Stub: hexdump_print ─────────────────────────────── */
int hexdump_print(const void *buf, size_t len)
{
    (void)buf;
    (void)len;
    kprintf("[hexdump] hexdump_print: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hexdump_to_buf ─────────────────────────────── */
int hexdump_to_buf(const void *buf, size_t len, char *out, size_t out_len)
{
    (void)buf;
    (void)len;
    (void)out;
    (void)out_len;
    kprintf("[hexdump] hexdump_to_buf: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hexdump_ascii ─────────────────────────────── */
int hexdump_ascii(const void *buf, size_t len)
{
    (void)buf;
    (void)len;
    kprintf("[hexdump] hexdump_ascii: not yet implemented\n");
    return -ENOSYS;
}
