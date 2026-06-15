/* tftpd.c — TFTP server: listen on UDP 69, serve files from directory */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define TFTP_PORT 69
#define BLOCK_SIZE 512

int main(int argc, char *argv[]) {
    const char *dir = ".";
    if (argc > 1) dir = argv[1];

    printf("tftpd: TFTP server starting on port %d, directory: %s\n", TFTP_PORT, dir);

    int ret = net_udp_listen(TFTP_PORT);
    if (ret < 0) {
        printf("tftpd: failed to listen on UDP port %d\n", TFTP_PORT);
        return 1;
    }

    unsigned char buf[516]; /* TFTP RRQ max size */
    unsigned int src_ip;
    unsigned short src_port;

    while (1) {
        int n = net_udp_recv(TFTP_PORT, buf, sizeof(buf), &src_ip, &src_port);
        if (n < 2) continue;

        int opcode = (buf[0] << 8) | buf[1];

        if (opcode != 1) continue; /* Only handle RRQ */

        /* Extract filename (null-terminated after opcode) */
        char filename[256];
        int i;
        for (i = 0; i < 255 && (2 + i) < n && buf[2 + i] != 0; i++) {
            filename[i] = (char)buf[2 + i];
        }
        filename[i] = '\0';

        printf("tftpd: RRQ from %d.%d.%d.%d:%d file=%s\n",
               ((src_ip) >> 24) & 0xFF, ((src_ip) >> 16) & 0xFF,
               ((src_ip) >> 8) & 0xFF, (src_ip) & 0xFF,
               src_port, filename);

        /* Prevent path traversal */
        if (strstr(filename, "..") != NULL) {
            printf("tftpd: path traversal denied: %s\n", filename);
            unsigned char err[] = {0, 5, 0, 2, 'A', 'c', 'c', 'e', 's', 's',
                                   ' ', 'd', 'e', 'n', 'i', 'e', 'd', 0};
            net_udp_send(src_ip, TFTP_PORT, src_port, err, sizeof(err));
            continue;
        }

        /* Build full path */
        char fullpath[512];
        if (dir[0] == '.' && dir[1] == '\0') {
            snprintf(fullpath, sizeof(fullpath), "%s", filename);
        } else {
            snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, filename);
        }

        int fd = open(fullpath, O_RDONLY, 0);
        if (fd < 0) {
            printf("tftpd: file not found: %s\n", fullpath);
            unsigned char err[] = {0, 5, 0, 1, 'F', 'i', 'l', 'e', ' ',
                                   'n', 'o', 't', ' ', 'f', 'o', 'u', 'n', 'd', 0};
            net_udp_send(src_ip, TFTP_PORT, src_port, err, sizeof(err));
            continue;
        }

        /* Send data blocks */
        unsigned short block = 1;
        unsigned char data_pkt[4 + BLOCK_SIZE];
        unsigned char ack[4];
        int transfer_ok = 1;

        while (1) {
            int rd = read(fd, data_pkt + 4, BLOCK_SIZE);
            if (rd < 0) {
                close(fd);
                transfer_ok = 0;
                break;
            }

            /* Build DATA packet */
            data_pkt[0] = 0;
            data_pkt[1] = 3; /* DATA opcode */
            data_pkt[2] = (unsigned char)((block >> 8) & 0xFF);
            data_pkt[3] = (unsigned char)(block & 0xFF);

            /* Send to client */
            ret = net_udp_send(src_ip, TFTP_PORT, src_port, data_pkt, 4 + rd);
            if (ret < 0) {
                close(fd);
                transfer_ok = 0;
                break;
            }

            /* Wait for ACK (blocking recv) */
            int ack_n = net_udp_recv(TFTP_PORT, ack, sizeof(ack), &src_ip, &src_port);
            if (ack_n < 4) {
                close(fd);
                transfer_ok = 0;
                break;
            }

            int ack_op = (ack[0] << 8) | ack[1];
            unsigned short ack_block = (unsigned short)((ack[2] << 8) | ack[3]);

            if (ack_op != 4 || ack_block != block) {
                close(fd);
                transfer_ok = 0;
                break;
            }

            if (rd < BLOCK_SIZE) break; /* Last packet */

            block++;
        }

        close(fd);

        if (transfer_ok) {
            printf("tftpd: transfer complete for %s (%d blocks)\n",
                   filename, block);
        } else {
            printf("tftpd: transfer failed for %s\n", filename);
        }
    }

    /* Should not reach here */
    net_udp_unlisten(TFTP_PORT);
    return 0;
}
