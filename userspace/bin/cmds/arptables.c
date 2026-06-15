/* arptables.c — ARP table management: read /proc/net/arp and display */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Parse colon-separated hex MAC address */
static int parse_mac(const char *s, unsigned char *mac) {
    int count = 0;
    while (*s && count < 6) {
        unsigned long byte = 0;
        int nibbles = 0;
        while (*s && *s != ':') {
            byte = (byte << 4) + (*s >= 'a' ? *s - 'a' + 10 :
                                 *s >= 'A' ? *s - 'A' + 10 : *s - '0');
            nibbles++;
            s++;
        }
        if (nibbles > 0) {
            mac[count++] = (unsigned char)byte;
        }
        if (*s == ':') s++;
    }
    return count == 6;
}

/* Split a line into tokens (destructive, modifies line) */
static int split_line(char *line, char **tokens, int max_tokens) {
    int count = 0;
    char *p = line;
    while (*p && count < max_tokens) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        tokens[count++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    return count;
}

static void show_arp_table(void) {
    int fd = open("/proc/net/arp", O_RDONLY, 0);
    if (fd < 0) {
        /* Try net_arp_list syscall */
        int ret = net_arp_list();
        if (ret >= 0) return;
        printf("arptables: cannot read ARP table\n");
        return;
    }

    char buf[2048];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("arptables: empty ARP table\n");
        return;
    }
    buf[n] = '\0';

    printf("ARP table:\n");
    printf("%-20s %-7s %-18s %s\n", "IP address", "HW type", "HW address", "Interface");

    char *line = buf;
    int first = 1;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';

        if (first) {
            first = 0;
            line = next;
            continue;
        }

        /* Parse: IP address HW type Flags HW address Mask Device */
        char *tokens[8];
        int tcount = split_line(line, tokens, 8);

        if (tcount >= 4) {
            unsigned char mac[6];
            char mac_buf[18] = "(incomplete)";
            if (parse_mac(tokens[3], mac)) {
                snprintf(mac_buf, sizeof(mac_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
            printf("%-20s %-7s %-18s %s\n",
                   tokens[0], tokens[1], mac_buf,
                   tcount >= 6 ? tokens[5] : "");
        }

        line = next;
    }
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        if (strcmp(argv[1], "-L") == 0 || strcmp(argv[1], "--list") == 0) {
            show_arp_table();
            return 0;
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("Usage: arptables -L|--list  (list ARP table)\n");
            printf("       arptables -A|-D       (add/delete not supported in userspace)\n");
            return 0;
        }
        printf("arptables: unknown option '%s'\n", argv[1]);
        printf("Usage: arptables -L\n");
        return 1;
    }

    show_arp_table();
    return 0;
}
