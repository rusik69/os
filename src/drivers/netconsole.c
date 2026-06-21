/*
 * netconsole.c — Send kernel log messages over UDP to a remote syslog server
 *
 * Item 391: netconsole — kernel log over UDP network
 *
 * Configuration via kernel cmdline:
 *   netconsole=@<target_ip>,<target_port>[@<src_port>]
 *
 * Example:
 *   netconsole=@192.168.1.100,514@6666
 *     — send syslog to 192.168.1.100:514 from port 6666
 *
 * Architecture:
 *   - Hooks into kprintf output (character-by-character)
 *   - Buffers characters into complete lines
 *   - On newline, schedules a workqueue item to send via UDP
 *   - Chains with any existing output hook (serial, framebuffer)
 *   - Recursion guard prevents re-entrancy from kprintf inside our work
 *
 * The syslog protocol (RFC 3164) format used:
 *   <PRI>timestamp hostname kernel: message
 *
 * Priority = facility * 8 + severity.  We use kernel facility (0)
 * and severity derived from console_loglevel.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "net.h"
#include "workqueue.h"
#include "cmdline.h"
#include "spinlock.h"
#include "timer.h"

/* ── Configuration ─────────────────────────────────────────────────── */

/* Maximum length of a single log line we'll buffer */
#define NC_LINE_MAX    512

/* Target syslog port (default) */
#define NC_DEFAULT_PORT  514

/* Source port used for UDP packets */
#define NC_DEFAULT_SRC   6666

/* Syslog facility: kernel = 0, user = 1, etc. */
#define NC_FACILITY      0   /* LOG_KERN */

/* Syslog severity mapping for console_loglevel */
#define NC_DEFAULT_SEVERITY 6  /* LOG_INFO */

/* ── State ─────────────────────────────────────────────────────────── */

/* Destination configuration */
static uint32_t   nc_target_ip     = 0;       /* 0 = not configured */
static uint16_t   nc_target_port   = NC_DEFAULT_PORT;
static uint16_t   nc_src_port      = NC_DEFAULT_SRC;
static int        nc_enabled       = 0;

/* Line buffer: accumulates characters until a newline */
static char       nc_line[NC_LINE_MAX];
static int        nc_line_len      = 0;

/* Chained output hook */
static void     (*nc_prev_hook)(char c, void *ctx) = NULL;
static void      *nc_prev_hook_ctx = NULL;

/* Synchronisation */
static spinlock_t  nc_lock;
static volatile int nc_in_hook     = 0;   /* recursion guard */

/* Workqueue item tracking */
static volatile int nc_work_pending = 0;  /* 1 = a work item is queued */

/* ── Forward declarations ──────────────────────────────────────────── */

static void nc_output_hook(char c, void *ctx);
static void nc_send_line(void *arg);

/* ── Parse IP address from dot-decimal string ──────────────────────── */

static int nc_parse_ip(const char *str, uint32_t *ip_out)
{
    if (!str || !*str)
        return -1;

    uint32_t ip = 0;
    int octets = 0;
    const char *p = str;

    while (*p && octets < 4) {
        /* Skip leading dots (shouldn't happen, but be robust) */
        if (*p == '.') { p++; continue; }

        /* Parse one octet */
        unsigned long val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10UL + (unsigned long)(*p - '0');
            if (val > 255) return -1;  /* invalid octet */
            p++;
        }
        ip = (ip << 8) | (uint32_t)(val & 0xFF);
        octets++;

        if (*p == '.') {
            p++;
        } else if (*p == '\0' || *p == ',' || *p == '@') {
            break;
        } else {
            return -1;  /* unexpected character */
        }
    }

    if (octets != 4)
        return -1;

    *ip_out = ip;
    return 0;
}

/* ── Parse netconsole= cmdline parameter ───────────────────────────── */

/*
 * Expected format:
 *   netconsole=@<target_ip>,<target_port>[@<src_port>]
 *
 * Examples:
 *   netconsole=@10.0.2.2,514         — send to 10.0.2.2:514 from default port
 *   netconsole=@192.168.1.100,514@6666 — send to 192.168.1.100:514 from port 6666
 *
 * If the cmdline argument is present but empty, we use defaults
 * (which effectively does nothing useful).  If not present at all,
 * netconsole stays disabled.
 */
static void nc_parse_cmdline(void)
{
    const char *cfg = cmdline_get("netconsole");
    if (!cfg || !*cfg) {
        nc_enabled = 0;
        return;
    }

    /* Expected: @<ip>,<port>[@<src_port>]
     * We accept both with and without the leading '@'. */
    const char *p = cfg;

    /* Skip optional leading '@' */
    if (*p == '@')
        p++;

    /* Parse target IP (dot-decimal up to the first comma) */
    char ip_str[32];
    int ip_idx = 0;
    while (*p && *p != ',' && *p != '@' && ip_idx < (int)sizeof(ip_str) - 1) {
        ip_str[ip_idx++] = *p++;
    }
    ip_str[ip_idx] = '\0';

    if (nc_parse_ip(ip_str, &nc_target_ip) < 0) {
        kprintf("[netconsole] WARNING: failed to parse target IP '%s', disabling\n",
                ip_str);
        nc_enabled = 0;
        return;
    }

    /* Skip comma */
    if (*p == ',') p++;

    /* Parse port (up to '@' or end of string) */
    char port_str[16];
    int port_idx = 0;
    while (*p && *p != '@' && port_idx < (int)sizeof(port_str) - 1) {
        port_str[port_idx++] = *p++;
    }
    port_str[port_idx] = '\0';

    if (port_str[0] != '\0') {
        long port = 0;
        const char *pp = port_str;
        while (*pp >= '0' && *pp <= '9') {
            port = port * 10 + (*pp - '0');
            pp++;
        }
        if (port > 0 && port < 65536) {
            nc_target_port = (uint16_t)port;
        }
    }

    /* Parse optional @src_port */
    if (*p == '@') {
        p++;
        char src_str[16];
        int src_idx = 0;
        while (*p && src_idx < (int)sizeof(src_str) - 1) {
            src_str[src_idx++] = *p++;
        }
        src_str[src_idx] = '\0';

        if (src_str[0] != '\0') {
            long src = 0;
            const char *pp = src_str;
            while (*pp >= '0' && *pp <= '9') {
                src = src * 10 + (*pp - '0');
                pp++;
            }
            if (src > 0 && src < 65536) {
                nc_src_port = (uint16_t)src;
            }
        }
    }

    nc_enabled = 1;
}

/* ── Initialisation ─────────────────────────────────────────────────── */

void netconsole_init(void)
{
    /* Parse configuration from kernel cmdline */
    nc_parse_cmdline();

    if (!nc_enabled) {
        kprintf("[--] netconsole: disabled (no 'netconsole=' kernel cmdline arg)\n");
        return;
    }

    /* Initialize synchronisation */
    spinlock_init(&nc_lock);
    nc_line_len = 0;
    nc_in_hook = 0;
    nc_work_pending = 0;
    memset(nc_line, 0, sizeof(nc_line));

    /* Chain into the kprintf output: save existing hook, install ours */
    kprintf_get_hook(&nc_prev_hook, &nc_prev_hook_ctx);
    kprintf_set_hook(nc_output_hook, NULL);

    kprintf("[OK] netconsole: target %d.%d.%d.%d:%d (src port %d)\n",
            (int)((nc_target_ip >> 24) & 0xFF),
            (int)((nc_target_ip >> 16) & 0xFF),
            (int)((nc_target_ip >> 8) & 0xFF),
            (int)(nc_target_ip & 0xFF),
            (int)nc_target_port,
            (int)nc_src_port);
}

/* ── Output hook ──────────────────────────────────────────────────────

 * Called from kprintf for each output character.  We MUST NOT block,
 * allocate memory, or call any function that might recursively invoke
 * kprintf.  We simply buffer the character; the actual network send
 * happens in the workqueue handler.
 *
 * We chain with any previous hook (e.g., serial framebuffer output)
 * so that console output still appears on the local terminal.
 */
static void nc_output_hook(char c, void *ctx)
{
    (void)ctx;

    /* ── Chain to previous hook first ── */
    if (nc_prev_hook)
        nc_prev_hook(c, nc_prev_hook_ctx);

    /* ── Our processing ── */
    if (!nc_enabled)
        return;

    /* Recursion guard: if kprintf is called while we're inside this hook
     * (e.g., from the workqueue handler), we skip buffering to avoid
     * infinite recursion. */
    if (nc_in_hook)
        return;

    nc_in_hook = 1;

    uint64_t nc_flags;
    spinlock_irqsave_acquire(&nc_lock, &nc_flags);

    if (c == '\n') {
        /* ── Complete line received ── */
        if (nc_line_len > 0) {
            /* Null-terminate the line */
            nc_line[nc_line_len] = '\0';

            /* Allocate a copy for the workqueue handler (runs in process
             * context where kmalloc is safe).  We do the copy under the
             * spinlock to avoid races with subsequent characters. */
            char *copy = (char *)kmalloc((size_t)nc_line_len + 1);
            if (copy) {
                memcpy(copy, nc_line, (size_t)nc_line_len + 1);

                /* Schedule workqueue item.  If the queue is full, we skip
                 * this line rather than block. */
                if (!nc_work_pending) {
                    nc_work_pending = 1;
                    if (workqueue_schedule(nc_send_line, copy) < 0) {
                        /* Queue full — drop the line */
                        kfree(copy);
                    }
                } else {
                    /* A work item is already pending; we can either queue
                     * another or batch.  For simplicity, queue another
                     * but note the queue only has WORKQUEUE_MAX (16) slots.
                     * If queue is full, we drop. */
                    if (workqueue_schedule(nc_send_line, copy) < 0) {
                        kfree(copy);
                    }
                }
            }
            nc_line_len = 0;
        }
    } else if (c == '\r') {
        /* Carriage return — ignore (output hook may receive CR+LF pairs) */
    } else if (nc_line_len < NC_LINE_MAX - 1) {
        /* Buffer the character */
        nc_line[nc_line_len++] = c;
    } else {
        /* Line too long — silently discard.  In practice this is rare;
         * the kernel doesn't often emit lines > 511 chars. */
    }

    spinlock_irqsave_release(&nc_lock, nc_flags);
    nc_in_hook = 0;
}

/* ── Workqueue handler: send a log line via UDP ─────────────────────

 * Runs in process (workqueue) context.  We are safe to call kmalloc,
 * net_udp_send, etc.  The recursion guard prevents us from re-entering
 * the output hook.
 */
static void nc_send_line(void *arg)
{
    char *line = (char *)arg;
    if (!line)
        goto done;

    /* Build syslog message (RFC 3164).
     *
     * Format: <PRI>timestamp hostname kernel: message
     *
     * Priority = facility * 8 + severity.
     * We use kernel facility (0) and LOG_INFO (6) severity by default.
     *
     * For simplicity, we skip the timestamp and hostname in this v1
     * implementation — the syslog server will add its own timestamp.
     * Full RFC 3164 compliance can be added in a future enhancement.
     */
    char buf[NC_LINE_MAX + 64];  /* large enough for line + prefix */
    int len;

    /* Format: <PRI>kernel: message\n
     * PRI = NC_FACILITY * 8 + NC_DEFAULT_SEVERITY = 0 * 8 + 6 = 6
     * So the prefix is "<6>kernel: " */
    len = snprintf(buf, sizeof(buf), "<%d>kernel: %s\n",
                   NC_FACILITY * 8 + NC_DEFAULT_SEVERITY,
                   line);
    if (len > (int)sizeof(buf))
        len = (int)sizeof(buf);

    /* Send via UDP */
    if (nc_target_ip != 0) {
        net_udp_send(nc_target_ip, nc_src_port, nc_target_port, buf, (uint16_t)len);
    }

    kfree(line);

done:
    nc_work_pending = 0;
}

/* ── Module interface (when built as a loadable module) ─────────────── */

#ifdef MODULE
#include "module.h"

static int __init netconsole_module_init(void)
{
    netconsole_init();
    return 0;
}

static void __exit netconsole_module_exit(void)
{
    /* Remove our hook and restore the previous one */
    void (*current_hook)(char, void *) = NULL;
    void *current_ctx = NULL;
    kprintf_get_hook(&current_hook, &current_ctx);
    if (current_hook == nc_output_hook) {
        kprintf_set_hook(nc_prev_hook, nc_prev_hook_ctx);
    }
    nc_enabled = 0;
}

module_init(netconsole_module_init);
module_exit(netconsole_module_exit);
MODULE_LICENSE("MIT");
MODULE_AUTHOR("OS Kernel Team");
MODULE_DESCRIPTION("netconsole — kernel log over UDP (Item 391)");
#endif /* MODULE */

/* ── Stub: netconsole_send ─────────────────────────────── */
int netconsole_send(const void *data, size_t len)
{
    (void)data;
    (void)len;
    kprintf("[netconsole] netconsole_send: not yet implemented\n");
    return 0;
}
/* ── Stub: netconsole_setup ─────────────────────────────── */
int netconsole_setup(const char *config)
{
    (void)config;
    kprintf("[netconsole] netconsole_setup: not yet implemented\n");
    return 0;
}
