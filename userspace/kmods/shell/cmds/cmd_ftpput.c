/* cmd_ftpput.c — FTP upload */
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

    /* Find opening paren */
    while (*p && *p != '(') p++;
    if (*p != '(') return -1;
    p++; /* skip '(' */

    /* Parse up to 6 comma-separated numbers */
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

int cmd_ftpput(int argc, char **argv)
{
    if (argc < 4) {
        kprintf("usage: ftpput <host> <remote-file> <local-file>\n");
        return 1;
    }

    const char *host = argv[1];
    const char *remote = argv[2];
    const char *local = argv[3];

    /* Resolve hostname */
    uint32_t ip = net_dns_resolve(host);
    if (ip == 0) {
        kprintf("ftpput: could not resolve '%s'\n", host);
        return 1;
    }

    /* Read local file */
    static uint8_t filebuf[16384];
    uint32_t file_size = 0;
    if (vfs_read(local, filebuf, sizeof(filebuf), &file_size) != 0) {
        kprintf("ftpput: cannot read local file '%s'\n", local);
        return 1;
    }
    if (file_size == 0) {
        kprintf("ftpput: local file '%s' is empty\n", local);
        return 1;
    }

    /* Connect to FTP server on port 21 */
    int conn = net_tcp_connect(ip, 21);
    if (conn < 0) {
        kprintf("ftpput: failed to connect to %s:21\n", host);
        return 1;
    }

    /* Read server banner */
    char resp[256];
    int n = net_tcp_recv(conn, resp, sizeof(resp) - 1, 100);
    if (n > 0) {
        resp[n] = '\0';
        kprintf("ftpput: %s", resp);
    }

    /* Send USER anonymous */
    ftp_send_cmd(conn, "USER anonymous", resp, sizeof(resp));
    kprintf("ftpput: %s", resp);

    /* Send PASS ftp@ */
    ftp_send_cmd(conn, "PASS ftp@", resp, sizeof(resp));
    kprintf("ftpput: %s", resp);

    /* Send TYPE I (binary) */
    ftp_send_cmd(conn, "TYPE I", resp, sizeof(resp));
    kprintf("ftpput: %s", resp);

    /* Send PASV to get passive mode port */
    ftp_send_cmd(conn, "PASV", resp, sizeof(resp));
    kprintf("ftpput: %s", resp);

    /* Parse PASV response */
    uint32_t data_ip = 0;
    uint16_t data_port = 0;

    if (parse_pasv(resp, &data_ip, &data_port) == 0) {
        /* Connect to data port */
        int data_conn = net_tcp_connect(data_ip, data_port);
        if (data_conn < 0) {
            kprintf("ftpput: failed to connect data channel\n");
            net_tcp_close(conn);
            return 1;
        }

        /* Send STOR <remote-file> */
        char stor_cmd[512];
        snprintf(stor_cmd, sizeof(stor_cmd), "STOR %s", remote);
        ftp_send_cmd(conn, stor_cmd, resp, sizeof(resp));
        kprintf("ftpput: %s", resp);

        /* Send file data */
        int sent = net_tcp_send(data_conn, filebuf, (uint16_t)file_size);
        if (sent < 0) {
            kprintf("ftpput: data send failed\n");
        } else {
            kprintf("ftpput: sent %d bytes\n", sent);
        }

        /* Close data connection */
        net_tcp_close(data_conn);
    } else {
        kprintf("ftpput: PASV failed\n");
    }

    /* Send QUIT */
    ftp_send_cmd(conn, "QUIT", resp, sizeof(resp));
    net_tcp_close(conn);

    kprintf("ftpput: upload of '%s' to %s as '%s' complete\n",
            local, host, remote);
    return 0;
}

void ftpput_init(void)
{
    kprintf("[OK] cmd_ftpput: FTP upload command ready\n");
}
