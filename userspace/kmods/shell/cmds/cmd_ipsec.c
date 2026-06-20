/* cmd_ipsec.c — IPsec SA/SP management */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "types.h"
#include "net.h"

/* ── IPsec structures (mirror of ipsec.c definitions) ────────── */
#define SADB_MAX_SAS 16

enum {
    SADB_MODE_TRANSPORT = 1,
    SADB_MODE_TUNNEL    = 2,
};

enum {
    SADB_PROTO_AH  = 1,
    SADB_PROTO_ESP = 2,
};

struct security_assoc {
    int      in_use;
    uint8_t  proto;
    uint8_t  mode;
    uint32_t spi;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  auth_key[32];
    int      auth_key_len;
    uint8_t  enc_key[32];
    int      enc_key_len;
    uint32_t replay_win;
    uint32_t last_seq;
};

/* ── External IPsec API (from net/ipsec.c) ───────────────────── */
int  ipsec_sa_add(uint32_t spi, uint32_t src_ip, uint32_t dst_ip,
                  uint8_t proto, uint8_t mode,
                  const uint8_t *auth_key, int auth_key_len,
                  const uint8_t *enc_key, int enc_key_len);
int  ipsec_sa_del(uint32_t spi, uint32_t dst_ip);
void ipsec_sa_flush(void);
int  ipsec_sa_list(struct security_assoc *buf, int max);

/* ── Helper: proto name ──────────────────────────────────────── */
static const char *sa_proto_name(uint8_t proto)
{
    switch (proto) {
        case SADB_PROTO_AH:  return "AH";
        case SADB_PROTO_ESP: return "ESP";
        default:             return "?";
    }
}

static const char *sa_mode_name(uint8_t mode)
{
    switch (mode) {
        case SADB_MODE_TRANSPORT: return "transport";
        case SADB_MODE_TUNNEL:    return "tunnel";
        default:                  return "?";
    }
}

/* ── Usage ───────────────────────────────────────────────────── */
static void ipsec_usage(void)
{
    kprintf("Usage: ipsec <subcommand> [options]\n");
    kprintf("Subcommands:\n");
    kprintf("  status       Show IPsec subsystem status\n");
    kprintf("  list         List all Security Associations\n");
    kprintf("  flush        Flush all SAs\n");
    kprintf("  auto add     Add an SA: auto add <src_ip> <dst_ip> <proto> <mode> <spi> [auth_key] [enc_key]\n");
    kprintf("  manual       Manual SA management\n");
}

void cmd_ipsec(const char *args)
{
    if (!args || !*args) {
        ipsec_usage();
        return;
    }

    while (*args == ' ') args++;

    if (strncmp(args, "status", 6) == 0) {
        kprintf("IPsec subsystem: present\n");
        kprintf("SADB size: %d entries\n", SADB_MAX_SAS);

        /* Count active SAs */
        struct security_assoc buf[SADB_MAX_SAS];
        int count = ipsec_sa_list(buf, SADB_MAX_SAS);
        kprintf("Active SAs: %d\n", count);

    } else if (strncmp(args, "list", 4) == 0) {
        struct security_assoc buf[SADB_MAX_SAS];
        int count = ipsec_sa_list(buf, SADB_MAX_SAS);

        if (count == 0) {
            kprintf("No Security Associations.\n");
            return;
        }

        kprintf("Security Association Database (%d entries):\n", count);
        kprintf("SPI         Proto  Mode       SrcIP              DstIP\n");
        kprintf("----------  -----  ---------  -----------------  -----------------\n");
        for (int i = 0; i < count; i++) {
            struct security_assoc *sa = &buf[i];
            kprintf("0x%08x  %-5s  %-9s  %u.%u.%u.%u  %u.%u.%u.%u\n",
                    sa->spi,
                    sa_proto_name(sa->proto),
                    sa_mode_name(sa->mode),
                    (unsigned int)((sa->src_ip >> 24) & 0xFF),
                    (unsigned int)((sa->src_ip >> 16) & 0xFF),
                    (unsigned int)((sa->src_ip >> 8) & 0xFF),
                    (unsigned int)(sa->src_ip & 0xFF),
                    (unsigned int)((sa->dst_ip >> 24) & 0xFF),
                    (unsigned int)((sa->dst_ip >> 16) & 0xFF),
                    (unsigned int)((sa->dst_ip >> 8) & 0xFF),
                    (unsigned int)(sa->dst_ip & 0xFF));
        }

    } else if (strncmp(args, "flush", 5) == 0) {
        ipsec_sa_flush();
        kprintf("All SAs flushed.\n");

    } else if (strncmp(args, "auto add", 8) == 0) {
        /* Parse "auto add <src_ip> <dst_ip> <proto> <mode> <spi> [auth_key] [enc_key]" */
        const char *p = args + 8;
        while (*p == ' ') p++;

        /* Tokenize remaining args */
        const char *tokens[8];
        int ntokens = 0;
        const char *tok = p;
        while (*tok && ntokens < 8) {
            while (*tok == ' ') tok++;
            if (!*tok) break;
            tokens[ntokens++] = tok;
            while (*tok && *tok != ' ') tok++;
            if (*tok) { /* advance past space */ tok++; }
        }

        if (ntokens < 5) {
            kprintf("ipsec: Usage: ipsec auto add <src_ip> <dst_ip> <proto> <mode> <spi> [auth_key] [enc_key]\n");
            kprintf("  proto: ah=1, esp=2   mode: transport=1, tunnel=2\n");
            return;
        }

        /* Parse src_ip */
        uint32_t src_ip = 0;
        uint32_t dst_ip = 0;
        uint8_t proto = 0;
        uint8_t mode = 0;
        uint32_t spi = 0;

        /* Simple IP parser: a.b.c.d */
        {
            const char *ip = tokens[0];
            int shift = 24;
            while (*ip && shift >= 0) {
                int octet = 0;
                while (*ip >= '0' && *ip <= '9') {
                    octet = octet * 10 + (*ip - '0');
                    ip++;
                }
                src_ip |= ((uint32_t)(octet & 0xFF) << shift);
                shift -= 8;
                if (*ip == '.') ip++;
            }
        }
        {
            const char *ip = tokens[1];
            int shift = 24;
            while (*ip && shift >= 0) {
                int octet = 0;
                while (*ip >= '0' && *ip <= '9') {
                    octet = octet * 10 + (*ip - '0');
                    ip++;
                }
                dst_ip |= ((uint32_t)(octet & 0xFF) << shift);
                shift -= 8;
                if (*ip == '.') ip++;
            }
        }

        proto = (uint8_t)strtol(tokens[2], NULL, 10);
        mode = (uint8_t)strtol(tokens[3], NULL, 10);
        spi = (uint32_t)strtoul(tokens[4], NULL, 0);

        /* Optional keys */
        const uint8_t *auth_key = NULL;
        int auth_key_len = 0;
        const uint8_t *enc_key = NULL;
        int enc_key_len = 0;

        if (ntokens > 5) {
            auth_key = (const uint8_t *)tokens[5];
            auth_key_len = (int)strlen(tokens[5]);
        }
        if (ntokens > 6) {
            enc_key = (const uint8_t *)tokens[6];
            enc_key_len = (int)strlen(tokens[6]);
        }

        int ret = ipsec_sa_add(spi, src_ip, dst_ip, proto, mode,
                               auth_key, auth_key_len, enc_key, enc_key_len);
        if (ret == 0) {
            kprintf("ipsec: SA added: spi=0x%08x %s %s\n", spi,
                    sa_proto_name(proto), sa_mode_name(mode));
        } else {
            kprintf("ipsec: failed to add SA: err=%d\n", ret);
        }

    } else if (strncmp(args, "manual", 6) == 0) {
        kprintf("ipsec: manual SA management:\n");
        kprintf("  Use 'auto add' to add SAs programmatically\n");

    } else {
        kprintf("ipsec: unknown subcommand '%s'\n", args);
        ipsec_usage();
    }
}
