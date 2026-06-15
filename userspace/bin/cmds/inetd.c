/* inetd.c — Internet services daemon: read /etc/inetd.conf, bind TCP ports, fork+exec */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_LINE 256
#define MAX_ARGS 16
#define MAX_SERVICES 32

struct inetd_service {
    char service[32];
    unsigned short port;
    int type;          /* 0 = stream (tcp), 1 = dgram (udp) */
    int wait;
    char user[16];
    char program[64];
    char *args[MAX_ARGS];
    int arg_count;
};

static int parse_port(const char *service_name) {
    /* Simple service name to port mapping */
    if (strcmp(service_name, "echo") == 0) return 7;
    if (strcmp(service_name, "discard") == 0) return 9;
    if (strcmp(service_name, "daytime") == 0) return 13;
    if (strcmp(service_name, "chargen") == 0) return 19;
    if (strcmp(service_name, "ftp") == 0) return 21;
    if (strcmp(service_name, "ssh") == 0) return 22;
    if (strcmp(service_name, "telnet") == 0) return 23;
    if (strcmp(service_name, "smtp") == 0) return 25;
    if (strcmp(service_name, "time") == 0) return 37;
    if (strcmp(service_name, "whois") == 0) return 43;
    if (strcmp(service_name, "domain") == 0) return 53;
    if (strcmp(service_name, "http") == 0) return 80;
    if (strcmp(service_name, "pop3") == 0) return 110;
    if (strcmp(service_name, "imap") == 0) return 143;
    if (strcmp(service_name, "https") == 0) return 443;
    /* Try numeric */
    int port = 0;
    const char *p = service_name;
    while (*p >= '0' && *p <= '9') {
        port = port * 10 + (*p - '0');
        p++;
    }
    if (port > 0) return port;
    return -1;
}

/* Split line into tokens (destructive) */
static int split_line(char *line, char **tokens, int max_tokens) {
    int count = 0;
    char *p = line;
    while (*p && count < max_tokens) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') break;
        tokens[count++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return count;
}

static int read_inetd_conf(struct inetd_service *services, int max_services) {
    int fd = open("/etc/inetd.conf", O_RDONLY, 0);
    if (fd < 0) return -1;

    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    int count = 0;
    char *line = buf;
    while (line && *line && count < max_services) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';

        /* Skip comments and empty lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') {
            line = next;
            continue;
        }

        /* Parse: service socket_type protocol wait user server_program args */
        char *tokens[16];
        int tcount = split_line(p, tokens, 16);

        if (tcount < 6) {
            line = next;
            continue;
        }

        struct inetd_service *svc = &services[count];
        memset(svc, 0, sizeof(*svc));
        strncpy(svc->service, tokens[0], sizeof(svc->service) - 1);
        svc->port = parse_port(svc->service);
        if (svc->port <= 0) {
            line = next;
            continue;
        }

        if (strcmp(tokens[1], "stream") == 0) svc->type = 0;
        else if (strcmp(tokens[1], "dgram") == 0) svc->type = 1;
        else { line = next; continue; }

        svc->wait = (tokens[3][0] == 'w' || tokens[3][0] == 'W') ? 1 : 0;
        strncpy(svc->user, tokens[4], sizeof(svc->user) - 1);
        strncpy(svc->program, tokens[5], sizeof(svc->program) - 1);

        /* Args: program name + additional args */
        svc->args[0] = svc->program;
        svc->arg_count = 1;
        for (int i = 6; i < tcount && svc->arg_count < MAX_ARGS - 1; i++) {
            svc->args[svc->arg_count++] = tokens[i];
        }

        count++;
        line = next;
    }

    return count;
}

static void run_service(struct inetd_service *svc) {
    int ret;

    if (svc->type == 0) {
        /* TCP stream */
        ret = net_tcp_listen(svc->port);
        if (ret < 0) {
            printf("inetd: cannot listen on port %d (%s)\n", svc->port, svc->service);
            return;
        }
        printf("inetd: listening on TCP port %d (%s)\n", svc->port, svc->service);

        while (1) {
            int conn = net_tcp_accept(svc->port, 0xFFFFFFFF);
            if (conn < 0) {
                yield();
                continue;
            }

            int pid = fork();
            if (pid == 0) {
                /* Child: try to exec the service program */
                char conn_str[16];
                snprintf(conn_str, sizeof(conn_str), "%d", conn);

                char *envp[] = { NULL };
                char *argv[MAX_ARGS + 2];
                int i;
                for (i = 0; i < svc->arg_count; i++)
                    argv[i] = svc->args[i];
                argv[i] = conn_str;
                argv[i + 1] = NULL;

                execve(svc->program, argv, envp);
                /* If exec fails, handle internally */
                printf("inetd: exec %s failed, handling internally\n", svc->program);
                exit(1);
            } else if (pid > 0) {
                net_tcp_close_conn(conn);
            }
        }
    } else {
        /* UDP dgram */
        ret = net_udp_listen(svc->port);
        if (ret < 0) {
            printf("inetd: cannot listen on UDP port %d (%s)\n", svc->port, svc->service);
            return;
        }
        printf("inetd: listening on UDP port %d (%s)\n", svc->port, svc->service);

        unsigned char buf[1024];
        unsigned int src_ip;
        unsigned short src_port;
        while (1) {
            int n = net_udp_recv(svc->port, buf, sizeof(buf), &src_ip, &src_port);
            if (n >= 0) {
                int pid = fork();
                if (pid == 0) {
                    printf("inetd: UDP packet from %d.%d.%d.%d:%d\n",
                           (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                           (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port);
                    exit(0);
                }
            } else {
                yield();
            }
        }
    }
}

int main(int argc, char *argv[]) {
    int run_daemon = 0;
    int list_only = 0;

    if (argc > 1) {
        if (strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[1], "-d") == 0 || strcmp(argv[1], "--daemon") == 0) {
            run_daemon = 1;
        } else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("Usage: inetd [-l|--list]   (list configured services)\n");
            printf("       inetd [-d|--daemon] (run as daemon)\n");
            return 0;
        } else {
            printf("inetd: unknown option '%s'\n", argv[1]);
            printf("Usage: inetd [-l] [-d]\n");
            return 1;
        }
    }

    struct inetd_service services[MAX_SERVICES];
    int count = read_inetd_conf(services, MAX_SERVICES);

    if (count <= 0) {
        printf("inetd: no services configured in /etc/inetd.conf\n");
        printf("inetd: kernel provides built-in services (TCP echo, discard, etc.)\n");
        if (run_daemon) {
            printf("inetd: running daemon without configuration\n");
        }
        return 0;
    }

    if (list_only) {
        printf("Service    Port  Type   Program\n");
        printf("---------  ----  -----  -------------------------\n");
        for (int i = 0; i < count; i++) {
            printf("%-10s %-5d %-6s %s\n",
                   services[i].service, services[i].port,
                   services[i].type == 0 ? "tcp" : "udp",
                   services[i].program);
        }
        return 0;
    }

    if (run_daemon) {
        printf("inetd: starting daemon with %d configured services\n", count);
        for (int i = 0; i < count; i++) {
            int pid = fork();
            if (pid == 0) {
                run_service(&services[i]);
                exit(0);
            }
        }
        while (1) {
            yield();
        }
    } else {
        printf("inetd: configured services (%d):\n", count);
        for (int i = 0; i < count; i++) {
            printf("  %-10s port %-5d %-4s -> %s\n",
                   services[i].service, services[i].port,
                   services[i].type == 0 ? "tcp" : "udp",
                   services[i].program);
        }
        printf("Run 'inetd -d' to start the daemon.\n");
    }

    return 0;
}
