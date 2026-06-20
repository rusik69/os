/*
 * cmd_ebtables.c — Ethernet bridge firewall
 *
 * Supports:
 *   ebtables -L              — List all rules in the bridge table
 *   ebtables -A <rule>       — Append a rule (simple MAC filter)
 *   ebtables -D <rule_nr>    — Delete a rule by number
 *
 * Rules are stored as simple MAC-address-based filter entries.
 */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "types.h"
#include "netdevice.h"

/* ── Bridge FDB entry (mirror of bridge.h) ───────────────────── */
#define BRIDGE_FDB_SIZE 64
#define BRIDGE_MAX_PORTS 32

struct bridge_fdb_entry {
    uint8_t  mac[6];
    int      port;
    uint64_t learn_tick;
    int      valid;
};

/* ── Simple ebtables rule table ───────────────────────────────── */
#define EBTABLES_MAX_RULES 32

struct ebtables_rule {
    int   in_use;
    char  src_mac[18];    /* "XX:XX:XX:XX:XX:XX" */
    char  dst_mac[18];
    int   in_port;
    int   out_port;
    char  action[16];     /* "ACCEPT" or "DROP" */
};

static struct ebtables_rule ebtables_rules[EBTABLES_MAX_RULES];
static int ebtables_rule_count = 0;

/* ── External bridge API (from net/bridge.c) ─────────────────── */
int  bridge_init(void);
int  bridge_add_port(int port_iface);
int  bridge_remove_port(int port_iface);
int  bridge_fdb_lookup(const uint8_t *mac);

static void ebtables_usage(void)
{
    kprintf("Usage: ebtables -L\n");
    kprintf("       ebtables -A <src_mac> <dst_mac> <in_port> <out_port> <ACCEPT|DROP>\n");
    kprintf("       ebtables -D <rule_number>\n");
    kprintf("  MAC format: XX:XX:XX:XX:XX:XX (use '*' for any)\n");
    kprintf("  Port: interface index (use -1 for any)\n");
}

/* Parse a MAC address string "XX:XX:XX:XX:XX:XX" into bytes */
static int parse_mac(const char *s, uint8_t mac[6])
{
    if (!s || !mac) return -1;
    if (strcmp(s, "*") == 0) {
        memset(mac, 0, 6);
        return 0; /* wildcard */
    }
    for (int i = 0; i < 6; i++) {
        int hi = 0, lo = 0;
        if (s[0] >= '0' && s[0] <= '9') hi = s[0] - '0';
        else if (s[0] >= 'A' && s[0] <= 'F') hi = s[0] - 'A' + 10;
        else if (s[0] >= 'a' && s[0] <= 'f') hi = s[0] - 'a' + 10;
        else return -1;
        if (s[1] >= '0' && s[1] <= '9') lo = s[1] - '0';
        else if (s[1] >= 'A' && s[1] <= 'F') lo = s[1] - 'A' + 10;
        else if (s[1] >= 'a' && s[1] <= 'f') lo = s[1] - 'a' + 10;
        else return -1;
        mac[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
        if (i < 5 && *s == ':') s++;
    }
    return 0;
}

/* Format a MAC address to string */
static void format_mac(const uint8_t mac[6], char *buf, int buf_size)
{
    if (!mac || !buf) return;
    /* Check if wildcard (all zeros from parse) */
    int all_zero = 1;
    for (int i = 0; i < 6; i++) { if (mac[i] != 0) { all_zero = 0; break; } }
    if (all_zero) {
        snprintf(buf, (size_t)buf_size, "*");
    } else {
        snprintf(buf, (size_t)buf_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

static void ebtables_list(void)
{
    kprintf("Bridge table: mac filter table\n");
    kprintf("No.  SRC MAC             DST MAC             IN PORT  OUT PORT  ACTION\n");
    kprintf("==== =================== =================== ======== ========= ========\n");

    int count = 0;
    for (int i = 0; i < EBTABLES_MAX_RULES; i++) {
        if (!ebtables_rules[i].in_use) continue;
        char src[20], dst[20];
        format_mac((uint8_t*)ebtables_rules[i].src_mac, src, sizeof(src));
        format_mac((uint8_t*)ebtables_rules[i].dst_mac, dst, sizeof(dst));
        kprintf("%-4d %-19s %-19s %-8d %-9d %s\n",
                i + 1, src, dst,
                ebtables_rules[i].in_port,
                ebtables_rules[i].out_port,
                ebtables_rules[i].action);
        count++;
    }

    if (count == 0)
        kprintf("  (no rules defined)\n");

    kprintf("\n%d rules, %d bridge interface(s) total\n", count, 0);
}

void cmd_ebtables(const char *args)
{
    if (!args || !*args) {
        ebtables_usage();
        return;
    }

    while (*args == ' ') args++;

    if (strncmp(args, "-L", 2) == 0) {
        ebtables_list();

    } else if (strncmp(args, "-A ", 3) == 0) {
        /* Append a rule: -A <src_mac> <dst_mac> <in_port> <out_port> <ACCEPT|DROP> */
        const char *p = args + 3;
        while (*p == ' ') p++;

        /* Tokenize */
        const char *tokens[8];
        int ntokens = 0;
        const char *tok = p;
        while (*tok && ntokens < 8) {
            while (*tok == ' ') tok++;
            if (!*tok) break;
            tokens[ntokens++] = tok;
            while (*tok && *tok != ' ') tok++;
            if (*tok) { tok++; }
        }

        if (ntokens < 5) {
            kprintf("ebtables: Usage: ebtables -A <src_mac> <dst_mac> <in_port> <out_port> <ACCEPT|DROP>\n");
            return;
        }

        /* Find free slot */
        int slot = -1;
        for (int i = 0; i < EBTABLES_MAX_RULES; i++) {
            if (!ebtables_rules[i].in_use) { slot = i; break; }
        }
        if (slot < 0) {
            kprintf("ebtables: rule table full (max %d)\n", EBTABLES_MAX_RULES);
            return;
        }

        struct ebtables_rule *r = &ebtables_rules[slot];

        /* Parse src_mac */
        uint8_t mac[6];
        if (parse_mac(tokens[0], mac) == 0) {
            snprintf(r->src_mac, sizeof(r->src_mac), "%s", tokens[0]);
        } else {
            kprintf("ebtables: invalid src MAC '%s'\n", tokens[0]);
            return;
        }

        /* Parse dst_mac */
        if (parse_mac(tokens[1], mac) == 0) {
            snprintf(r->dst_mac, sizeof(r->dst_mac), "%s", tokens[1]);
        } else {
            kprintf("ebtables: invalid dst MAC '%s'\n", tokens[1]);
            return;
        }

        r->in_port = (int)strtol(tokens[2], NULL, 10);
        r->out_port = (int)strtol(tokens[3], NULL, 10);

        /* Parse action */
        if (strcmp(tokens[4], "ACCEPT") == 0 || strcmp(tokens[4], "DROP") == 0) {
            snprintf(r->action, sizeof(r->action), "%s", tokens[4]);
        } else {
            kprintf("ebtables: invalid action '%s' (use ACCEPT or DROP)\n", tokens[4]);
            return;
        }

        r->in_use = 1;
        ebtables_rule_count++;
        kprintf("ebtables: rule %d added: %s -> %s (in=%d out=%d action=%s)\n",
                slot + 1, r->src_mac, r->dst_mac, r->in_port, r->out_port, r->action);

    } else if (strncmp(args, "-D ", 3) == 0) {
        /* Delete a rule by number: -D <rule_number> */
        const char *p = args + 3;
        while (*p == ' ') p++;
        int rule_nr = (int)strtol(p, NULL, 10);
        if (rule_nr < 1 || rule_nr > EBTABLES_MAX_RULES) {
            kprintf("ebtables: invalid rule number '%d' (must be 1..%d)\n", rule_nr, EBTABLES_MAX_RULES);
            return;
        }
        int idx = rule_nr - 1;
        if (!ebtables_rules[idx].in_use) {
            kprintf("ebtables: rule %d is not active\n", rule_nr);
            return;
        }
        ebtables_rules[idx].in_use = 0;
        ebtables_rule_count--;
        kprintf("ebtables: rule %d deleted\n", rule_nr);

    } else {
        kprintf("ebtables: unknown option '%s'\n", args);
        ebtables_usage();
    }
}
