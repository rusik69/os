#include "telnetd.h"
#include "net.h"
#include "e1000.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "shell.h"
#include "scheduler.h"

#define TELNET_PORT 23

/* Telnet negotiation bytes */
#define IAC  255
#define WILL 251
#define WONT 252
#define DO   253
#define DONT 254
#define OPT_ECHO       1
#define OPT_SUPPRESS_GA 3
#define OPT_LINEMODE   34

/* Per-connection state */
#define TELNET_BUF_SIZE 256
#define TELNET_OUT_SIZE 4096

struct telnet_session {
    int conn_id;
    int active;
    int processing;  /* 1 while a command is executing (prevents re-entry) */
    char cmd_buf[TELNET_BUF_SIZE];
    int cmd_len;
    char out_buf[TELNET_OUT_SIZE];
    int out_len;
    int negotiated;
};

static struct telnet_session sessions[8];

static struct telnet_session *find_session(int conn_id) {
    for (int i = 0; i < 8; i++)
        if (sessions[i].active && sessions[i].conn_id == conn_id) return &sessions[i];
    return NULL;
}

static struct telnet_session *alloc_session(int conn_id) {
    for (int i = 0; i < 8; i++) {
        if (!sessions[i].active) {
            memset(&sessions[i], 0, sizeof(sessions[i]));
            sessions[i].conn_id = conn_id;
            sessions[i].active = 1;
            return &sessions[i];
        }
    }
    return NULL;
}

/* Append to session output buffer */
static void ses_write(struct telnet_session *s, const char *data, int len) {
    for (int i = 0; i < len && s->out_len < TELNET_OUT_SIZE; i++)
        s->out_buf[s->out_len++] = data[i];
}

static void ses_flush(struct telnet_session *s) {
    if (s->out_len > 0) {
        net_tcp_send(s->conn_id, s->out_buf, s->out_len);
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

static void process_telnet_cmd(struct telnet_session *s) {
    char *cmd = s->cmd_buf;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    char *args = cmd;
    while (*args && *args != ' ') args++;
    if (*args) { *args = '\0'; args++; while (*args == ' ') args++; }
    else args = NULL;

    /* Handle telnet-specific commands */
    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        kprintf_set_hook(telnet_output_hook, s);
        kprintf("Goodbye!\n");
        kprintf_set_hook(0, 0);
        ses_flush(s);
        net_tcp_close(s->conn_id);
        s->active = 0;
        return;
    }

    /* Add to history before executing */
    shell_history_add(s->cmd_buf);

    /* Set processing flag to prevent re-entrant command execution
     * (e.g., if net_poll is called during a long-running command) */
    s->processing = 1;

    /* Redirect kprintf output to this session, run the shared command handler */
    kprintf_set_hook(telnet_output_hook, s);
    shell_exec_cmd(cmd, args);
    kprintf_set_hook(0, 0);

    s->processing = 0;

    /* Prompt */
    char prompt[] = "os> ";
    ses_write(s, prompt, 4);
    ses_flush(s);
}

static void on_connect(int conn_id) {
    struct telnet_session *s = alloc_session(conn_id);
    if (!s) { net_tcp_close(conn_id); return; }

    /* Send telnet negotiation: server will echo, suppress go-ahead */
    uint8_t neg[] = {
        IAC, WILL, OPT_ECHO,
        IAC, WILL, OPT_SUPPRESS_GA,
        IAC, DONT, OPT_LINEMODE,
    };
    net_tcp_send(conn_id, neg, sizeof(neg));

    kprintf_set_hook(telnet_output_hook, s);
    kprintf("\n=== OS Remote Shell ===\n");
    kprintf("Type 'help' for commands, 'exit' to disconnect.\n\n");
    kprintf("os> ");
    kprintf_set_hook(0, 0);
    ses_flush(s);
    s->negotiated = 1;
}

static void on_data(int conn_id, const void *data, uint16_t len) {
    struct telnet_session *s = find_session(conn_id);
    if (!s) return;
    /* Prevent re-entrant command processing (e.g. when net_poll is called
     * while a command is already executing) */
    if (s->processing) return;

    const uint8_t *p = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t c = p[i];

        /* Skip telnet IAC sequences */
        if (c == IAC && i + 2 < len) {
            i += 2; /* skip command + option */
            continue;
        }

        if (c == '\r') continue; /* ignore CR, handle LF */
        if (c == '\n' || c == '\0') {
            /* Execute command */
            s->cmd_buf[s->cmd_len] = '\0';
            ses_write(s, "\r\n", 2);
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
    if (s) s->active = 0;
}

void telnetd_init(void) {
    memset(sessions, 0, sizeof(sessions));
    net_tcp_listen(TELNET_PORT, on_connect, on_data, on_close);
}

void telnetd_task(void) {
    for (;;) {
        net_poll();
        scheduler_yield();
    }
}
