/* cmd_rawsend.c — send a raw Ethernet frame (test/diagnostic tool) */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

/*
 * Usage: rawsend XX:XX:XX:XX:XX:XX XX:XX:XX:XX:XX:XX ethertype HEXDATA
 * Builds a minimal Ethernet II frame and sends it.
 * Example: rawsend ff:ff:ff:ff:ff:ff 00:11:22:33:44:55 0800 deadbeef
 */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_mac(const char *s, uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        int hi = hex_nibble(s[0]), lo = hex_nibble(s[1]);
        if (hi < 0 || lo < 0) return -1;
        mac[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
        if (i < 5) { if (*s != ':') return -1; s++; }
    }
    return 0;
}

void cmd_rawsend(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: rawsend DEST_MAC SRC_MAC ETHERTYPE HEXDATA\n");
        kprintf("  Example: rawsend ff:ff:ff:ff:ff:ff 00:11:22:33:44:55 0806 ...\n");
        return;
    }

    static uint8_t frame[1514];
    int flen = 0;

    /* Destination MAC */
    uint8_t dst[6], src[6];
    if (parse_mac(args, dst) < 0) { kprintf("rawsend: bad dst MAC\n"); return; }
    while (*args && *args != ' ') args++;
    while (*args == ' ') args++;
    /* Source MAC */
    if (parse_mac(args, src) < 0) { kprintf("rawsend: bad src MAC\n"); return; }
    while (*args && *args != ' ') args++;
    while (*args == ' ') args++;

    /* Build Ethernet header */
    for (int i = 0; i < 6; i++) frame[flen++] = dst[i];
    for (int i = 0; i < 6; i++) frame[flen++] = src[i];

    /* EtherType */
    int hi = hex_nibble(args[0]), lo2 = hex_nibble(args[1]);
    int hi2 = hex_nibble(args[2]), lo3 = hex_nibble(args[3]);
    if (hi < 0 || lo2 < 0 || hi2 < 0 || lo3 < 0) {
        kprintf("rawsend: bad ethertype (need 4 hex digits)\n"); return;
    }
    frame[flen++] = (uint8_t)((hi << 4) | lo2);
    frame[flen++] = (uint8_t)((hi2 << 4) | lo3);
    args += 4;
    while (*args == ' ') args++;

    /* Hex payload */
    while (args[0] && args[1] && flen < 1514) {
        int h = hex_nibble(args[0]), l = hex_nibble(args[1]);
        if (h < 0 || l < 0) break;
        frame[flen++] = (uint8_t)((h << 4) | l);
        args += 2;
        if (*args == ' ') args++;
    }

    /* Pad to minimum Ethernet frame size (64 bytes including header) */
    while (flen < 60) frame[flen++] = 0;

    int r = libc_raw_send(frame, (uint32_t)flen);
    if (r >= 0) kprintf("rawsend: sent %d bytes\n", (unsigned long)flen);
    else        kprintf("rawsend: failed\n");
}
