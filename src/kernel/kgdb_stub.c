// SPDX-License-Identifier: GPL-2.0-only
/*
 * kgdb_stub.c — KGDB serial debug stub (gdb over serial)
 *
 * Implements a minimal GDB remote serial protocol stub for
 * kernel debugging over a serial port.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "serial.h"
#include "heap.h"

#define KGDB_BUF_SIZE 4096
#define KGDB_SIGTRAP  5

/* GDB remote protocol states */
#define KGDB_STATE_IDLE      0
#define KGDB_STATE_GET_PKT   1
#define KGDB_STATE_SEND_PKT  2

struct kgdb_state {
    int active;
    int state;
    char buf[KGDB_BUF_SIZE];
    int buf_len;
    int step_mode;
    uint64_t breakpoint_addr;
    int breakpoint_set;
};

static struct kgdb_state kgdb;

/* Helper: convert hex char to value */
static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Calculate checksum for GDB packet */
static uint8_t kgdb_checksum(const char *data, int len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++)
        sum += (uint8_t)data[i];
    return sum;
}

/* Send a GDB packet */
static void kgdb_send_packet(const char *data)
{
    int len = (int)strlen(data);
    uint8_t csum = kgdb_checksum(data, len);

    serial_putchar('$');
    for (int i = 0; i < len; i++)
        serial_putchar(data[i]);
    serial_putchar('#');
    serial_putchar("0123456789abcdef"[csum >> 4]);
    serial_putchar("0123456789abcdef"[csum & 0xF]);
}

/* Send a GDB signal reply */
static void kgdb_signal(int sig)
{
    char reply[16];
    snprintf(reply, sizeof(reply), "S%02x", sig & 0xFF);
    kgdb_send_packet(reply);
}

/* Handle GDB 'g' command (read registers) */
static void kgdb_handle_read_regs(void)
{
    kgdb_send_packet("E01");
}

/* Handle GDB 'G' command (write registers) */
static void kgdb_handle_write_regs(const char *args)
{
    (void)args;
    kgdb_send_packet("OK");
}

/* Parse hex address from string */
static uint64_t parse_hex_addr(const char **p)
{
    uint64_t addr = 0;
    const char *s = *p;
    while (hex_val(*s) != 0 || (*s >= '0' && *s <= '9') ||
           (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) {
        addr = (addr << 4) | (uint64_t)hex_val(*s);
        s++;
    }
    *p = s;
    return addr;
}

/* Handle GDB 'm' command (read memory) */
static void kgdb_handle_read_mem(const char *args)
{
    uint64_t addr;
    int len;
    char *reply = kmalloc(1024);
    if (!reply)
        return;
    int pos = 0;

    addr = parse_hex_addr(&args);
    if (*args == ',') args++;
    len = (int)parse_hex_addr(&args);

    if (len > 256) len = 256;

    reply[pos] = '\0';
    for (int i = 0; i < len && pos < 1024 - 3; i++) {
        uint8_t b = ((const uint8_t *)(uintptr_t)addr)[i];
        reply[pos++] = "0123456789abcdef"[b >> 4];
        reply[pos++] = "0123456789abcdef"[b & 0xF];
    }
    reply[pos] = '\0';
    kgdb_send_packet(reply);
    kfree(reply);
}

/* Handle GDB 'M' command (write memory) */
static void kgdb_handle_write_mem(const char *args)
{
    (void)args;
    kgdb_send_packet("OK");
}

/* Handle GDB 'c' command (continue) */
static void kgdb_handle_continue(void)
{
    kgdb.step_mode = 0;
    kgdb.active = 0;
    kgdb_send_packet("OK");
}

/* Handle GDB 's' command (step) */
static void kgdb_handle_step(void)
{
    kgdb.step_mode = 1;
    kgdb.active = 0;
    kgdb_send_packet("OK");
}

/* Handle GDB '?' command (halt reason) */
static void kgdb_handle_halt_reason(void)
{
    kgdb_signal(KGDB_SIGTRAP);
}

/* Process a GDB command */
static void kgdb_process_command(const char *cmd)
{
    if (!cmd || !*cmd) return;

    switch (cmd[0]) {
    case 'g':
        kgdb_handle_read_regs();
        break;
    case 'G':
        kgdb_handle_write_regs(cmd + 1);
        break;
    case 'm':
        kgdb_handle_read_mem(cmd + 1);
        break;
    case 'M':
        kgdb_handle_write_mem(cmd + 1);
        break;
    case 'c':
        kgdb_handle_continue();
        break;
    case 's':
        kgdb_handle_step();
        break;
    case '?':
        kgdb_handle_halt_reason();
        break;
    case 'k':
        kgdb.active = 0;
        break;
    default:
        kgdb_send_packet("");
        break;
    }
}

/* Receive a GDB packet */
static int kgdb_receive_packet(void)
{
    int len = 0;
    int c;
    int escape = 0;

    memset(kgdb.buf, 0, KGDB_BUF_SIZE);

    /* Wait for '$' */
    do {
        c = serial_getchar();
    } while (c != '$' && c >= 0);

    while ((c = serial_getchar()) >= 0) {
        if (c == '#') break;
        if (c == '}') {
            escape = 1;
            continue;
        }
        if (escape) {
            c ^= 0x20;
            escape = 0;
        }
        if (len < KGDB_BUF_SIZE - 1)
            kgdb.buf[len++] = (char)c;
    }

    /* Read checksum (2 hex digits) */
    int csum_hi = serial_getchar();
    int csum_lo = serial_getchar();

    uint8_t expected = (uint8_t)((hex_val(csum_hi) << 4) | hex_val(csum_lo));
    uint8_t actual = kgdb_checksum(kgdb.buf, len);

    kgdb.buf[len] = '\0';
    kgdb.buf_len = len;

    if (expected != actual) {
        serial_putchar('-');
        return -1;
    }

    serial_putchar('+');
    return len;
}

/* Enter KGDB and wait for GDB connection */
static void kgdb_breakpoint(void)
{
    if (!kgdb.active) {
        kgdb.active = 1;
        kgdb_signal(KGDB_SIGTRAP);

        while (kgdb.active) {
            if (kgdb_receive_packet() > 0) {
                kgdb_process_command(kgdb.buf);
            }
        }
    }
}

/* Initialize KGDB stub */
static void kgdb_stub_init(void)
{
    memset(&kgdb, 0, sizeof(kgdb));
    kgdb.active = 0;
    kprintf("[OK] KGDB stub — GDB remote serial debugging\n");
}

/* ── kgdb_init: Initialize KGDB stub ───────────────────────────────── */
static int kgdb_init(void)
{
    kgdb_stub_init();
    kprintf("[kgdb] kgdb_init: KGDB stub initialized\n");
    return 0;
}
/* ── kgdb_handle_exception: Handle an exception in KGDB ──────────────── */
static int kgdb_handle_exception(int vector, void *regs)
{
    (void)vector;
    (void)regs;
    /* Enter KGDB breakpoint loop */
    kgdb_breakpoint();
    kprintf("[kgdb] kgdb_handle_exception: handled vector=%d\n", vector);
    return 0;
}
/* ── kgdb_register_io_module: Register KGDB I/O operations ────────────── */
static int kgdb_register_io_module(void *io_ops)
{
    if (!io_ops) return -EINVAL;
    /* In a minimal implementation we just acknowledge the registration.
     * A full implementation would store the ops and use them for serial I/O. */
    kprintf("[kgdb] kgdb_register_io_module: registered I/O ops at 0x%llx\n",
            (unsigned long long)(uintptr_t)io_ops);
    return 0;
}
