#include "telnetd.h"
#include "net.h"
#include "e1000.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "shell.h"
#include "scheduler.h"
#include "editor.h"
#include "fs.h"
#include "service.h"

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
    int hist_pos;    /* current history navigation position */
    int esc_state;   /* 0=normal, 1=got ESC, 2=got ESC[ */
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

/* kprintf flush hook: sends buffered output to the telnet client */
static void ses_flush_hook(void *ctx) {
    ses_flush((struct telnet_session *)ctx);
}

static void process_telnet_cmd(struct telnet_session *s) {
    char *cmd = s->cmd_buf;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    /* Handle telnet-specific commands before full processing */
    {
        char *c = cmd;
        while (*c == ' ') c++;
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

    /* Add to history before executing */
    shell_history_add(s->cmd_buf);

    /* Set processing flag to prevent re-entrant command execution
     * (e.g., if net_poll is called during a long-running command) */
    s->processing = 1;

    /* Redirect kprintf output to this session, run the full command line
     * processor (supports pipes, redirection, background &) */
    kprintf_set_hook(telnet_output_hook, s);
    kprintf_set_flush(ses_flush_hook, s);
    shell_process_line(cmd);
    kprintf_set_flush(0, 0);
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
    service_log("telnetd", "client connected");

    /* Send telnet negotiation: server will echo, suppress go-ahead */
    uint8_t neg[] = {
        IAC, WILL, OPT_ECHO,
        IAC, WILL, OPT_SUPPRESS_GA,
        IAC, DONT, OPT_LINEMODE,
    };
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
    s->hist_pos = shell_history_count();
    s->esc_state = 0;
}

static void on_data(int conn_id, const void *data, uint16_t len) {
    struct telnet_session *s = find_session(conn_id);
    if (!s) return;

    const uint8_t *p = (const uint8_t *)data;

    /* If the editor is active, route all characters to the editor ring buffer.
     * Bypass the processing guard so net_poll() inside the editor loop works. */
    if (editor_is_active()) {
        for (uint16_t i = 0; i < len; i++) {
            uint8_t c = p[i];
            if (c == IAC && i + 2 < len) { i += 2; continue; }
            editor_feed_char((char)c);
        }
        return;
    }

    /* Prevent re-entrant command processing */
    if (s->processing) return;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t c = p[i];

        /* Skip telnet IAC sequences */
        if (c == IAC && i + 2 < len) {
            i += 2; /* skip command + option */
            continue;
        }

        /* ANSI escape sequence state machine for arrow keys */
        if (s->esc_state == 1) {
            if (c == '[') { s->esc_state = 2; continue; }
            s->esc_state = 0;
            /* fall through to process c normally */
        } else if (s->esc_state == 2) {
            s->esc_state = 0;
            if (c == 'A' || c == 'B') {
                /* Erase current input */
                for (int k = 0; k < s->cmd_len; k++) ses_write(s, "\b ", 2);
                for (int k = 0; k < s->cmd_len; k++) ses_write(s, "\b", 1);
                if (c == 'A') {
                    /* Up: go back in history */
                    int cnt = shell_history_count();
                    int oldest = cnt > HISTORY_SIZE ? cnt - HISTORY_SIZE : 0;
                    if (s->hist_pos > oldest) s->hist_pos--;
                    const char *entry = shell_history_entry(s->hist_pos);
                    if (entry) {
                        strncpy(s->cmd_buf, entry, TELNET_BUF_SIZE - 1);
                        s->cmd_buf[TELNET_BUF_SIZE - 1] = '\0';
                        s->cmd_len = strlen(s->cmd_buf);
                        ses_write(s, s->cmd_buf, s->cmd_len);
                    } else { s->cmd_buf[0] = '\0'; s->cmd_len = 0; }
                } else {
                    /* Down: go forward in history */
                    int cnt = shell_history_count();
                    if (s->hist_pos < cnt - 1) {
                        s->hist_pos++;
                        const char *entry = shell_history_entry(s->hist_pos);
                        if (entry) {
                            strncpy(s->cmd_buf, entry, TELNET_BUF_SIZE - 1);
                            s->cmd_buf[TELNET_BUF_SIZE - 1] = '\0';
                            s->cmd_len = strlen(s->cmd_buf);
                            ses_write(s, s->cmd_buf, s->cmd_len);
                        }
                    } else {
                        s->hist_pos = cnt;
                        s->cmd_buf[0] = '\0';
                        s->cmd_len = 0;
                    }
                }
                ses_flush(s);
            }
            /* ignore other escape sequences */
            continue;
        }

        if (c == 0x1b) { s->esc_state = 1; continue; }

        if (c == '\r') continue; /* ignore CR, handle LF */
        if (c == '\n' || c == '\0') {
            /* Execute command */
            s->cmd_buf[s->cmd_len] = '\0';
            ses_write(s, "\r\n", 2);
            s->hist_pos = shell_history_count(); /* reset navigation */
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

        /* Tab completion */
        if (c == '\t') {
            s->cmd_buf[s->cmd_len] = '\0';
            shell_tab_complete_telnet(s->cmd_buf, &s->cmd_len, s);
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
    service_log("telnetd", "client disconnected");
}

static int telnetd_running = 0;

int telnetd_start(void) {
    if (telnetd_running) return 0;
    memset(sessions, 0, sizeof(sessions));
    net_tcp_listen(TELNET_PORT, on_connect, on_data, on_close);
    telnetd_running = 1;
    return 0;
}

void telnetd_stop(void) {
    if (!telnetd_running) return;
    net_tcp_unlisten(TELNET_PORT);
    memset(sessions, 0, sizeof(sessions));
    telnetd_running = 0;
}

void telnetd_init(void) {
    telnetd_start();
}

void telnetd_task(void) {
    for (;;) {
        net_poll();
        scheduler_yield();
    }
}
