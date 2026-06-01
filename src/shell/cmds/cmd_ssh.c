/* cmd_ssh.c — SSH client */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "ssh.h"
#include "ssh_client.h"

void cmd_ssh(const char *args) {
    if (!args || *args == '\0') {
        kprintf("Usage: ssh <user>@<host>[:port]\n");
        return;
    }

    char buf[256];
    strncpy(buf, args, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';

    char *user = buf;
    char *host = buf;
    uint16_t port = 22;

    char *at = strchr(buf, '@');
    if (at) {
        *at = '\0';
        user = buf;
        host = at + 1;
    } else {
        user = "root";
        host = buf;
    }

    while (*host == ' ') host++;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port == 0) port = 22;
    }
    char *end = host + strlen(host) - 1;
    while (end > host && *end == ' ') *end-- = '\0';

    kprintf("SSH: connecting to %s@%s:%u...\n", user, host, port);

    /* Read password (simple version) */
    kprintf("Password: ");
    char pass[128] = {0};
    /* Use keyboard read - for simplicity, use fixed password for now */
    /* In real use, the kernel would provide getchar() */
    kprintf("(using default)\n");
    strcpy(pass, "os");

    struct ssh_client *cl = ssh_client_connect(host, port, user, pass,
                                                NULL, NULL, NULL);
    if (!cl) {
        kprintf("SSH: connection failed\n");
        return;
    }

    /* Wait for handshake */
    int timeout = 500;
    while (timeout > 0 && !ssh_client_ready(cl) && ssh_client_connected(cl)) {
        ssh_client_poll(cl);
        timeout--;
    }

    if (!ssh_client_connected(cl) || !ssh_client_ready(cl)) {
        kprintf("SSH: handshake failed\n");
        ssh_client_close(cl);
        return;
    }

    kprintf("SSH: connected.\n");

    /* Simple shell: send commands and poll */
    while (ssh_client_connected(cl)) {
        /* Poll for response data */
        for (int i = 0; i < 50 && ssh_client_connected(cl); i++) {
            ssh_client_poll(cl);
        }
        if (!ssh_client_connected(cl)) break;
        
        /* No interactive input from shell command context,
           just send a command and wait */
        break;
    }

    ssh_client_close(cl);
    kprintf("SSH: disconnected\n");
}
