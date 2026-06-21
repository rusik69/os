/*
 * smtp.c — SMTP client implementation (RFC 5321)
 *
 * Connects to an SMTP server via TCP, performs EHLO, MAIL FROM,
 * RCPT TO, DATA sequences.  Supports AUTH LOGIN for submission
 * ports.
 */

#define KERNEL_INTERNAL
#include "smtp.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "base64.h"
#include "timer.h"

#define SMTP_BUF_SIZE 1024
#define SMTP_RESPONSE_TIMEOUT 300  /* 3 seconds in centiseconds */

/* Private helper: read an SMTP response line into buf, returns line length */
static int smtp_read_response(int conn_id, char *buf, int bufsize) {
    int pos = 0;
    uint64_t deadline = timer_get_ticks() + SMTP_RESPONSE_TIMEOUT;

    while (timer_get_ticks() < deadline) {
        net_poll();
        int avail = net_tcp_available(conn_id);
        if (avail > 0) {
            /* Read one byte at a time until \n */
            char c;
            while (avail > 0 && pos < bufsize - 1) {
                if (net_tcp_recv(conn_id, &c, 1, 10) == 1) {
                    buf[pos++] = c;
                    if (c == '\n') {
                        buf[pos] = '\0';
                        return pos;
                    }
                }
                avail = net_tcp_available(conn_id);
            }
        }
        if (net_tcp_has_closed(conn_id)) break;
    }

    buf[pos] = '\0';
    return pos;
}

/* Check if response starts with a positive code (2xx) */
static int smtp_status_ok(const char *resp) {
    if (resp[0] >= '2' && resp[0] <= '3' && resp[1] == '0' && resp[2] == '0')
        return 1;
    return 0;
}

/* Send a command and check response */
static int smtp_cmd(int conn_id, const char *cmd, char *resp, int resp_size) {
    int len = strlen(cmd);
    if (net_tcp_send(conn_id, cmd, len) < 0) return -1;
    smtp_read_response(conn_id, resp, resp_size);
    return smtp_status_ok(resp) ? 0 : -1;
}

static int smtp_connect_and_send(uint32_t server_ip, uint16_t port,
                                  const char *from, const char *to,
                                  const char *subject, const char *body,
                                  const char *username, const char *password) {
    char buf[SMTP_BUF_SIZE];
    int ret = -1;

    /* Connect to SMTP server */
    int conn_id = net_tcp_connect(server_ip, port);
    if (conn_id < 0) {
        kprintf("[SMTP] connect failed\n");
        return -1;
    }

    /* Wait for greeting */
    smtp_read_response(conn_id, buf, sizeof(buf));
    if (!smtp_status_ok(buf)) {
        kprintf("[SMTP] bad greeting: %s\n", buf);
        goto out;
    }

    /* EHLO */
    snprintf(buf, sizeof(buf), "EHLO osdev\r\n");
    if (smtp_cmd(conn_id, buf, buf, sizeof(buf)) < 0) {
        kprintf("[SMTP] EHLO failed: %s\n", buf);
        goto out;
    }

    /* AUTH LOGIN if credentials provided */
    if (username && password) {
        snprintf(buf, sizeof(buf), "AUTH LOGIN\r\n");
        if (smtp_cmd(conn_id, buf, buf, sizeof(buf)) < 0) {
            kprintf("[SMTP] AUTH LOGIN failed: %s\n", buf);
            goto out;
        }

        /* Send base64-encoded username */
        char encoded[256];
        size_t elen = base64_encode(encoded, (const uint8_t*)username, strlen(username));
        if (elen == (size_t)-1) goto out;
        snprintf(buf, sizeof(buf), "%s\r\n", encoded);
        if (smtp_cmd(conn_id, buf, buf, sizeof(buf)) < 0) {
            kprintf("[SMTP] AUTH username failed: %s\n", buf);
            goto out;
        }

        /* Send base64-encoded password */
        elen = base64_encode(encoded, (const uint8_t*)password, strlen(password));
        if (elen == (size_t)-1) goto out;
        snprintf(buf, sizeof(buf), "%s\r\n", encoded);
        if (smtp_cmd(conn_id, buf, buf, sizeof(buf)) < 0) {
            kprintf("[SMTP] AUTH password failed: %s\n", buf);
            goto out;
        }
    }

    /* MAIL FROM */
    snprintf(buf, sizeof(buf), "MAIL FROM:<%s>\r\n", from);
    if (smtp_cmd(conn_id, buf, buf, sizeof(buf)) < 0) {
        kprintf("[SMTP] MAIL FROM failed: %s\n", buf);
        goto out;
    }

    /* RCPT TO */
    snprintf(buf, sizeof(buf), "RCPT TO:<%s>\r\n", to);
    if (smtp_cmd(conn_id, buf, buf, sizeof(buf)) < 0) {
        kprintf("[SMTP] RCPT TO failed: %s\n", buf);
        goto out;
    }

    /* DATA */
    snprintf(buf, sizeof(buf), "DATA\r\n");
    if (smtp_cmd(conn_id, buf, buf, sizeof(buf)) < 0) {
        kprintf("[SMTP] DATA failed: %s\n", buf);
        goto out;
    }

    /* Send email headers and body */
    char data_buf[2048];
    int n = snprintf(data_buf, sizeof(data_buf),
                     "From: <%s>\r\n"
                     "To: <%s>\r\n"
                     "Subject: %s\r\n"
                     "MIME-Version: 1.0\r\n"
                     "Content-Type: text/plain; charset=UTF-8\r\n"
                     "\r\n"
                     "%s\r\n"
                     ".\r\n",
                     from, to, subject, body);

    if (net_tcp_send(conn_id, data_buf, n) < 0) goto out;

    /* Read response */
    smtp_read_response(conn_id, buf, sizeof(buf));
    if (!smtp_status_ok(buf)) {
        kprintf("[SMTP] DATA content failed: %s\n", buf);
        goto out;
    }

    /* QUIT */
    snprintf(buf, sizeof(buf), "QUIT\r\n");
    net_tcp_send(conn_id, buf, strlen(buf));

    ret = 0;

out:
    net_tcp_close(conn_id);
    return ret;
}

int smtp_send(uint32_t server_ip, uint16_t port,
              const char *from, const char *to,
              const char *subject, const char *body) {
    return smtp_connect_and_send(server_ip, port, from, to, subject, body, NULL, NULL);
}

int smtp_send_auth(uint32_t server_ip, uint16_t port,
                   const char *from, const char *to,
                   const char *subject, const char *body,
                   const char *username, const char *password) {
    return smtp_connect_and_send(server_ip, port, from, to, subject, body, username, password);
}

/* ── Implement: smtp_connect ────────────────── */
int smtp_connect(const char *host, int port)
{
    kprintf("[smtp] smtp_connect: stub (basic)\n");
    return -EOPNOTSUPP;
}
