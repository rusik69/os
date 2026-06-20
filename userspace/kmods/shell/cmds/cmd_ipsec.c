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
    kprintf("  auto add     Add an SA (not implemented via shell)\n");
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

    } else {
        kprintf("ipsec: unknown subcommand '%s'\n", args);
        ipsec_usage();
    }
}
