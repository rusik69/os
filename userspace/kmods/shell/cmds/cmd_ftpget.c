/* cmd_ftpget.c — FTP download */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "net.h"

/* Helper: send an FTP command and read the response */
static int ftp_send_cmd(int conn, const char *cmd, char *resp, int resp_max)
{
    char buf[512];
    int slen = strlen(cmd);
    memcpy(buf, cmd, (size_t)slen);
    buf[slen] = '\r';
    buf[slen + 1] = '\n';
    int ret = net_tcp_send(conn, buf, slen + 2);
    if (ret < 0) return ret;
    if (resp && resp_max > 0) {
        int n = net_tcp_recv(conn, resp, resp_max - 1, 100);
        if (n > 0) resp[n] = '\0';
        else resp[0] = '\0';
    }
    return 0;
}

/* Parse PASV response like "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" */
static int parse_pasv(const char *resp, uint32_t *ip_out, uint16_t *port_out)
{
    int h[6];
    int count = 0;
    const char *p = resp;

    while (*p && *p != '(') p++;
    if (*p != '(') return -1;
    p++;

    while (*p && *p != ')' && count < 6) {
        if (*p >= '0' && *p <= '9') {
            int val = 0;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
            }
            h[count++] = val;
        } else {
            p++;
        }
    }

    if (count != 6) return -1;

    *ip_out = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
              ((uint32_t)h[2] << 8)  | (uint32_t)h[3];
    *port_out = (uint16_t)(h[4] * 256 + h[5]);
    return 0;
}

int cmd_ftpget(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: ftpget <host> <remote-file> [local-file]\n");
        return 1;
    }

    const char *host = argv[1];
    const char *remote = argv[2];
    const char *local = (argc >= 4) ? argv[3] : argv[2];

    /* Resolve hostname */
    uint32_t ip = net_dns_resolve(host);
    if (ip == 0) {
        kprintf("ftpget: could not resolve '%s'\n", host);
        return 1;
    }

    /* Connect to FTP server on port 21 */
    int conn = net_tcp_connect(ip, 21);
    if (conn < 0) {
        kprintf("ftpget: failed to connect to %s:21\n", host);
        return 1;
    }

    char resp[256];
    int n = net_tcp_recv(conn, resp, sizeof(resp) - 1, 100);
    if (n > 0) {
        resp[n] = '\0';
        kprintf("ftpget: %s", resp);
    }

    /* Login */
    ftp_send_cmd(conn, "USER anonymous", resp, sizeof(resp));
    kprintf("ftpget: %s", resp);
    ftp_send_cmd(conn, "PASS ftp@", resp, sizeof(resp));
    kprintf("ftpget: %s", resp);

    /* Binary mode */
    ftp_send_cmd(conn, "TYPE I", resp, sizeof(resp));
    kprintf("ftpget: %s", resp);

    /* Passive mode */
    ftp_send_cmd(conn, "PASV", resp, sizeof(resp));
    kprintf("ftpget: %s", resp);

    /* Parse PASV response */
    uint32_t data_ip = 0;
    uint16_t data_port = 0;

    if (parse_pasv(resp, &data_ip, &data_port) == 0) {
        /* Connect to data port */
        int data_conn = net_tcp_connect(data_ip, data_port);
        if (data_conn < 0) {
            kprintf("ftpget: failed to connect data channel\n");
            net_tcp_close(conn);
            return 1;
        }

        /* Send RETR <remote-file> */
        char retr_cmd[512];
        snprintf(retr_cmd, sizeof(retr_cmd), "RETR %s", remote);
        ftp_send_cmd(conn, retr_cmd, resp, sizeof(resp));
        kprintf("ftpget: %s", resp);

        /* Read file data from data connection */
        static uint8_t buf[65536];
        uint32_t total = 0;
        while (1) {
            int r = net_tcp_recv(data_conn, buf + total,
                                  (uint16_t)(sizeof(buf) - total), 50);
            if (r <= 0) break;
            total += (uint32_t)r;
            if (total >= sizeof(buf)) break;
        }

        net_tcp_close(data_conn);

        if (total > 0) {
            /* Write to local file */
            if (vfs_write(local, buf, total) < 0) {
                kprintf("ftpget: failed to write local file '%s'\n", local);
            } else {
                kprintf("ftpget: downloaded %u bytes to '%s'\n",
                        (unsigned int)total, local);
            }
        } else {
            kprintf("ftpget: no data received\n");
        }
    } else {
        kprintf("ftpget: PASV parsing failed\n");
    }

    ftp_send_cmd(conn, "QUIT", resp, sizeof(resp));
    net_tcp_close(conn);

    return 0;
}

void ftpget_init(void)
{
    kprintf("[OK] cmd_ftpget: FTP download command ready\n");
}
