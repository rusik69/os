#include "telnetd.h"

#include "dhcp.h"
#include "e1000.h"
#include "fs.h"
#include "net.h"
#include "printf.h"
#include "process.h"
#include "scheduler.h"
#include "service.h"
#include "shell.h"
#include "spinlock.h"
#include "string.h"
#include "timer.h"

#define TELNET_PORT 23

/* Telnet negotiation bytes */
#define IAC 255
#define WILL 251
#define WONT 252
#define DO 253
#define DONT 254
#define OPT_ECHO 1
#define OPT_SUPPRESS_GA 3
#define OPT_LINEMODE 34

/* Telnet command bytes (RFC 854) */
#define SE 240
#define NOP 241
#define BRK 243
#define IP 244
#define AO 245
#define AYT 246
#define EC 247
#define EL 248
#define GA 249
#define SB 250

/* Per-connection state */
#define TELNET_BUF_SIZE 256
#define TELNET_OUT_SIZE 32768
#define TELNET_OUT_FLUSH (TELNET_OUT_SIZE - 512)

struct telnet_session {
    int conn_id;
    int active;
    int processing; /* 1 while a command is executing (prevents re-entry) */
    char cmd_buf[TELNET_BUF_SIZE];
    int cmd_len;
    char out_buf[TELNET_OUT_SIZE];
    int out_len;
    int negotiated;
    int hist_pos;  /* current history navigation position */
    int esc_state; /* 0=normal, 1=got ESC, 2=got ESC[ */
    char cwd[64];  /* per-session working directory */
};

static struct telnet_session sessions[8];

/* Global pointer to the active session's CWD buffer, set around each command.
 * This lets sys_chdir/sys_getcwd use the correct CWD regardless of which
 * process happens to call net_poll() and trigger on_data(). */
static char *g_session_cwd = NULL;

char *telnet_get_cwd_ctx(void) {
    return g_session_cwd;
}

static struct telnet_session *find_session(int conn_id) {
    for (int i = 0; i < 8; i++)
        if (sessions[i].active && sessions[i].conn_id == conn_id)
            return &sessions[i];
    return NULL;
}

static struct telnet_session *alloc_session(int conn_id) {
    for (int i = 0; i < 8; i++) {
        if (!sessions[i].active) {
            memset(&sessions[i], 0, sizeof(sessions[i]));
            sessions[i].conn_id = conn_id;
            sessions[i].active = 1;
            sessions[i].cwd[0] = '/';
            sessions[i].cwd[1] = '\0';
            return &sessions[i];
        }
    }
    return NULL;
}

static void ses_flush(struct telnet_session *s);

/* Append to session output buffer; flush when nearing capacity */
static void ses_write(struct telnet_session *s, const char *data, int len) {
    for (int i = 0; i < len; i++) {
        if (s->out_len >= TELNET_OUT_FLUSH)
            ses_flush(s);
        if (s->out_len < TELNET_OUT_SIZE)
            s->out_buf[s->out_len++] = data[i];
    }
}

static void ses_flush(struct telnet_session *s) {
    if (s->out_len > 0) {
        /* Defer the send when called from inside a spinlock critical
         * section: the packet-dispatch path (net_rx_dispatch → TCP
         * receive) holds tcp_lock, and net_tcp_send would deadlock
         * re-acquiring it on the same CPU (observed: spinlock lockup
         * in tcp_lock when a shell command over telnet printed output
         * while netd was dispatching the incoming packet).  The
         * telnetd poll loop flushes deferred output when the locks
         * are free. */
        if (lockdep_holding_spinlock())
            return;
        net_tcp_send(s->conn_id, s->out_buf, (uint16_t)s->out_len);
        s->out_len = 0;
    }
}

/* kprintf hook: redirects output to telnet session */
static void telnet_output_hook(char c, void *ctx) {
    struct telnet_session *s = (struct telnet_session *)ctx;
    if (c == '\n') {
        /* Send \r\n for telnet */
        char crlf[2] = {'\r', '\n'};
        ses_write(s, crlf, 2);
    } else {
        ses_write(s, &c, 1);
    }
}

/* kprintf flush hook: sends buffered output to the telnet client */
static void ses_flush_hook(void *ctx) {
    ses_flush((struct telnet_session *)ctx);
}

static void process_telnet_cmd(struct telnet_session *s) {
    char *cmd = s->cmd_buf;
    while (*cmd == ' ')
        cmd++;
    if (*cmd == '\0')
        return;

    /* Handle telnet-specific commands before full processing */
    {
        char *c = cmd;
        while (*c == ' ')
            c++;
        if (strcmp(c, "exit") == 0 || strcmp(c, "quit") == 0) {
            kprintf_set_hook(telnet_output_hook, s);
            kprintf("Goodbye!\n");
            kprintf_set_hook(0, 0);
            ses_flush(s);
            net_tcp_close(s->conn_id);
            s->active = 0;
            return;
        }
    }

    /* Set processing flag to prevent re-entrant command execution */
    s->processing = 1;

    /* Point global CWD context at this session's cwd buffer */
    g_session_cwd = s->cwd;

    /* Redirect kprintf output to this session, execute the command via
     * the kernel shell (all builtins: dmesg, arp, route, uname, ...),
     * then restore the hooks.  The shell is a loadable module that
     * registers shell_process_line_ptr — guard in case it isn't loaded. */
    kprintf_set_hook(telnet_output_hook, s);
    kprintf_set_flush(ses_flush_hook, s);
    if (shell_process_line_ptr)
        shell_process_line_ptr(cmd);
    kprintf_set_flush(0, 0);
    kprintf_set_hook(0, 0);

    g_session_cwd = NULL;
    s->processing = 0;

    /* Prompt */
    char prompt[] = "os> ";
    ses_write(s, prompt, 4);
    ses_flush(s);
}

static void on_connect(int conn_id) {
    struct telnet_session *s = alloc_session(conn_id);
    if (!s) {
        net_tcp_close(conn_id);
        return;
    }
    service_log("telnetd", "client connected");

    /* Send telnet negotiation: server will echo, suppress go-ahead */
    uint8_t neg[] = {
        IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SUPPRESS_GA, IAC, DONT, OPT_LINEMODE,
    };
    net_tcp_set_nodelay(conn_id, 1); /* interactive: send every byte immediately */
    net_tcp_send(conn_id, neg, sizeof(neg));

    kprintf_set_hook(telnet_output_hook, s);
    kprintf_set_flush(ses_flush_hook, s);
    kprintf("\n=== OS Remote Shell ===\n");
    kprintf("Type 'help' for commands, 'exit' to disconnect.\n\n");
    kprintf("os> ");
    kprintf_set_flush(0, 0);
    kprintf_set_hook(0, 0);
    ses_flush(s);
    s->negotiated = 1;
    s->hist_pos = 0;
    s->esc_state = 0;
}

static void on_data(int conn_id, const void *data, uint16_t len) {
    struct telnet_session *s = find_session(conn_id);
    if (!s)
        return;

    const uint8_t *p = (const uint8_t *)data;

    /* Prevent re-entrant command processing */
    if (s->processing)
        return;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t c = p[i];

        /* ── Telnet IAC negotiation parsing (RFC 854) ────────────── */
        if (c == IAC) {
            if (i + 1 >= len)
                continue; /* truncated — skip lone IAC */
            uint8_t cmd = p[i + 1];

            if (cmd == IAC) {
                /* IAC IAC = escaped literal 0xFF (data byte 255) */
                i++;      /* skip second IAC byte */
                continue; /* 0xFF won't pass the printable check below */
            } else if (cmd == WILL || cmd == WONT || cmd == DO || cmd == DONT) {
                /* 3-byte: IAC WILL/WONT/DO/DONT <option> */
                if (i + 2 >= len)
                    continue; /* truncated — need option byte */
                i += 2;       /* skip command + option */
                continue;
            } else if (cmd == SB) {
                /* Variable-length subnegotiation: IAC SB <option> <params> IAC SE
                 * RFC 854 §3.3: within params, IAC is escaped as IAC IAC.
                 * Parse carefully to avoid premature SE detection on escaped IAC. */
                uint16_t j;
                for (j = i + 2; j < len; j++) {
                    if (p[j] == IAC) {
                        if (j + 1 < len && p[j + 1] == SE) {
                            i = j + 1; /* skip to end of SE */
                            break;
                        }
                        /* IAC IAC within subnegotiation data — skip the
                         * doubled byte and continue scanning. */
                        if (j + 1 < len && p[j + 1] == IAC)
                            j++;
                    }
                }
                if (j >= len)
                    i = len - 1; /* truncated — advance to end */
                continue;
            } else {
                /* 2-byte commands: NOP, DM, BRK, IP, AO, AYT, EC, EL, GA, etc. */
                i++; /* skip command byte */
                continue;
            }
        }

        /* ANSI escape sequence state machine for arrow keys */
        if (s->esc_state == 1) {
            if (c == '[') {
                s->esc_state = 2;
                continue;
            }
            s->esc_state = 0;
            /* fall through to process c normally */
        } else if (s->esc_state == 2) {
            s->esc_state = 0;
            if (c == 'A' || c == 'B') {
                /* Erase current input */
                for (int k = 0; k < s->cmd_len; k++)
                    ses_write(s, "\b ", 2);
                for (int k = 0; k < s->cmd_len; k++)
                    ses_write(s, "\b", 1);
                ses_flush(s);
            }
            /* ignore other escape sequences */
            continue;
        }

        if (c == 0x1b) {
            s->esc_state = 1;
            continue;
        }

        if (c == '\r')
            continue; /* ignore CR, handle LF */
        if (c == '\n' || c == '\0') {
            /* Execute command */
            s->cmd_buf[s->cmd_len] = '\0';
            ses_write(s, "\r\n", 2);
            s->hist_pos = 0;
            process_telnet_cmd(s);
            s->cmd_len = 0;
            continue;
        }

        if (c == 127 || c == '\b') {
            if (s->cmd_len > 0) {
                s->cmd_len--;
                ses_write(s, "\b \b", 3);
                ses_flush(s);
            }
            continue;
        }

        /* Tab completion - not available without kernel shell */
        if (c == '\t') {
            continue;
        }

        if (c >= 32 && c < 127 && s->cmd_len < TELNET_BUF_SIZE - 1) {
            s->cmd_buf[s->cmd_len++] = c;
            /* Echo character back */
            char echo = c;
            ses_write(s, &echo, 1);
            ses_flush(s);
        }
    }
}

static void on_close(int conn_id) {
    struct telnet_session *s = find_session(conn_id);
    if (s)
        s->active = 0;
    service_log("telnetd", "client disconnected");
}

static int telnetd_running = 0;

int telnetd_start(void) {
    if (telnetd_running)
        return 0;
    memset(sessions, 0, sizeof(sessions));
    net_tcp_listen(TELNET_PORT, on_connect, on_data, on_close);
    telnetd_running = 1;
    return 0;
}

void telnetd_stop(void) {
    if (!telnetd_running)
        return;
    net_tcp_unlisten(TELNET_PORT);
    memset(sessions, 0, sizeof(sessions));
    telnetd_running = 0;
}

void telnetd_init(void) {
    telnetd_start();
}

void telnetd_task(void) {
    uint8_t pkt[2048];
    for (;;) {
        process_sleep_ticks(2);

        /* Flush deferred telnet output (ses_flush skips sending while a
         * spinlock — tcp_lock — is held, to avoid same-CPU deadlock;
         * here the locks are free so pending output goes out). */
        for (int i = 0; i < 8; i++) {
            if (sessions[i].active && sessions[i].out_len > 0)
                ses_flush(&sessions[i]);
        }

        /* Skip while a DHCP transaction is in flight: the DHCP client's
         * direct spin-poll owns the RX descriptors during that window
         * (racing it corrupts the descriptor ring). */
        if (dhcp_client_busy())
            continue;

        /* Drain ALL pending packets — the RX ring is small (32 descs);
         * taking one packet per 2-tick poll lets it overflow and the NIC
         * silently drops (the client's data then never arrives). */
        int n;
        while ((n = net_link_recv(pkt, sizeof(pkt))) > 0)
            net_rx_dispatch(pkt, (uint16_t)n);
        net_dhcp_renew_if_needed();
    }
}

/* ── Implement: telnetd_handle_client ────────────────── */
static int telnetd_handle_client(void *client) {
    if (!client) {
        kprintf("[telnetd] telnetd_handle_client: NULL client\n");
        return -EINVAL;
    }
    kprintf("[telnetd] telnetd_handle_client: client=%p (stub)\n", client);
    return -EOPNOTSUPP;
}
