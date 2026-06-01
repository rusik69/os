#define KERNEL_INTERNAL
#include "types.h"
#include "ssh.h"
#include "net.h"
#include "sha256.h"
#include "aes.h"
#include "hmac.h"
#include "rng.h"
#include "string.h"
#include "printf.h"
#include "service.h"
#include "shell.h"
#include "users.h"
#include "editor.h"
#include "scheduler.h"

/* ── Forward declarations from ssh_crypto.c ─────────────────── */
extern void bn_from_bytes(bignum *r, const uint8_t *bytes, int len);
extern int bn_to_bytes(const bignum *a, uint8_t *bytes, int min_len);
extern int bn_compare(const bignum *a, const bignum *b);
extern void bn_copy(bignum *r, const bignum *a);
extern void bn_set_u32(bignum *r, uint32_t v);
extern int bn_is_zero(const bignum *a);
extern void bn_random(bignum *r, const bignum *max);
extern void bn_mod_exp(bignum *r, const bignum *base, const bignum *exp, const bignum *mod);
extern void bn_mod_mul(bignum *r, const bignum *a, const bignum *b, const bignum *m);
extern void bn_mod(bignum *r, const bignum *a, const bignum *m);

extern void dh_init(void);
extern const bignum *dh_get_p(void);
extern const bignum *dh_get_g(void);
extern void dh_generate_key(bignum *pub, bignum *priv);
extern void dh_compute_shared(bignum *shared, const bignum *their_pub, const bignum *my_priv);

extern int rsa_sign(const uint8_t *hash, uint8_t *sig_out);
extern int rsa_verify(const uint8_t *hash, const uint8_t *sig);
extern const uint8_t *rsa_get_pubkey_blob(int *len);

/* ── Pack/unpack helpers ────────────────────────────────────── */
static void p32(uint8_t *b, uint32_t v) {
    b[0] = (v >> 24) & 0xFF; b[1] = (v >> 16) & 0xFF;
    b[2] = (v >> 8) & 0xFF; b[3] = v & 0xFF;
}
static uint32_t g32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}
static int pstr(uint8_t *b, const uint8_t *d, int l) {
    p32(b, l); if (l) memcpy(b + 4, d, l); return 4 + l;
}
static int pmpint(uint8_t *b, const bignum *bn) {
    uint8_t tmp[257];
    int len = bn_to_bytes(bn, tmp + 1, 0);
    int start = 1;
    while (start <= len && tmp[start] == 0) start++;
    int dl = len - start + 1;
    if (dl <= 0) { p32(b, 0); return 4; }
    if (tmp[start] & 0x80) { tmp[start - 1] = 0; start--; dl++; }
    p32(b, dl); memcpy(b + 4, tmp + start, dl);
    return 4 + dl;
}

/* ── Per-connection SSH session ─────────────────────────────── */
#define SSH_SESS_BUF 16384

enum ssh_phase {
    SSH_PHASE_VERSION,
    SSH_PHASE_KEX_INIT,
    SSH_PHASE_KEX,
    SSH_PHASE_NEWKEYS,
    SSH_PHASE_SERVICE,
    SSH_PHASE_AUTH,
    SSH_PHASE_CHANNEL,
    SSH_PHASE_SESSION
};

struct ssh_session {
    int conn_id;
    int active;
    
    /* Protocol state */
    enum ssh_phase phase;
    uint32_t seq_nr_send;
    uint32_t seq_nr_recv;
    
    /* KEX state */
    uint8_t client_kex[400];
    int client_kex_len;
    uint8_t server_kex[400];
    int server_kex_len;
    bignum dh_priv;
    bignum dh_pub;
    bignum dh_shared;
    uint8_t exchange_hash[32];
    uint8_t session_id[32];
    int kex_done;
    
    /* Encryption */
    int encrypted;
    struct {
        uint8_t key[16], iv[16];
        struct aes_ctx ctx;
        uint32_t seq;
    } send_cipher, recv_cipher;
    
    /* MAC */
    uint8_t send_mac_key[32];
    uint8_t recv_mac_key[32];
    
    /* Receive buffer */
    uint8_t recv_buf[SSH_SESS_BUF];
    int recv_len;
    
    /* Channel state */
    int channel_open;
    int channel_id; /* our channel number (server: always 0) */
    uint32_t rwnd;   /* remote window */
    
    /* Shell state */
    int processing;   /* 1 while a command is executing */
    char cwd[64];     /* per-session working directory */
    char cmd_buf[1024];
    int cmd_len;
    char out_buf[32768];
    int out_len;
};

#define SSH_MAX_SESSIONS 4
static struct ssh_session sessions[SSH_MAX_SESSIONS];
static int sshd_running = 0;

static struct ssh_session *find_session(int conn_id) {
    for (int i = 0; i < SSH_MAX_SESSIONS; i++)
        if (sessions[i].active && sessions[i].conn_id == conn_id)
            return &sessions[i];
    return NULL;
}

static struct ssh_session *alloc_session(int conn_id) {
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
            memset(&sessions[i], 0, sizeof(sessions[i]));
            sessions[i].conn_id = conn_id;
            sessions[i].active = 1;
            sessions[i].phase = SSH_PHASE_VERSION;
            sessions[i].cwd[0] = '/';
            sessions[i].cwd[1] = '\0';
            return &sessions[i];
        }
    }
    return NULL;
}

/* ── Packet I/O ─────────────────────────────────────────────── */

/* Send raw data over TCP */
static int ses_send_raw(struct ssh_session *s, const uint8_t *data, int len) {
    return net_tcp_send(s->conn_id, data, len);
}

/* Send an encrypted SSH packet (after key exchange) */
static int ses_send_packet(struct ssh_session *s, uint8_t type,
                           const uint8_t *payload, int payload_len) {
    uint8_t pkt[SSH_SESS_BUF];
    int pkt_len;
    
    /* Build plaintext: uint32 length || byte pad_len || payload || padding */
    int pad_len = 16 - ((1 + payload_len) % 16);
    if (pad_len < 4) pad_len += 16;
    
    int total = 1 + payload_len + pad_len; /* padding_length + payload + padding */
    
    pkt[0] = (total >> 24) & 0xFF;
    pkt[1] = (total >> 16) & 0xFF;
    pkt[2] = (total >> 8) & 0xFF;
    pkt[3] = total & 0xFF;
    pkt[4] = pad_len;
    
    int off = 5;
    /* Message type byte */
    pkt[off++] = type;
    if (payload && payload_len > 0) {
        memcpy(pkt + off, payload, payload_len);
        off += payload_len;
    }
    /* Random padding */
    rng_fill_buf(pkt + off, pad_len);
    off += pad_len;
    
    int packet_len = off; /* from padding_length to end of padding */
    
    if (!s->encrypted) {
        /* Send in clear (before NEWKEYS) */
        ses_send_raw(s, pkt, 4 + packet_len);
        s->seq_nr_send++;
        return 0;
    }
    
    /* Encrypt the block (packet_length is not encrypted) */
    uint8_t encrypted[SSH_SESS_BUF];
    memcpy(encrypted, pkt, 4); /* packet_length in clear */
    
    /* Encrypt from padding_length onwards */
    aes_cbc_encrypt(&s->send_cipher.ctx, s->send_cipher.iv,
                    pkt + 4, encrypted + 4, packet_len);
    
    /* Compute MAC */
    uint8_t mac[32];
    uint8_t seq_buf[4];
    p32(seq_buf, s->seq_nr_send);
    uint8_t mac_input[4 + 4 + packet_len];
    memcpy(mac_input, seq_buf, 4);
    memcpy(mac_input + 4, pkt, 4 + packet_len);
    hmac_sha256(s->send_mac_key, 32, mac_input, 4 + 4 + packet_len, mac);
    
    ses_send_raw(s, encrypted, 4 + packet_len);
    ses_send_raw(s, mac, 32);
    s->seq_nr_send++;
    return 0;
}

/* Send a formatted SSH packet (type + string components) */
static int ses_send_packet_v(struct ssh_session *s, uint8_t type,
                             const uint8_t **parts, const int *part_lens, int nparts) {
    uint8_t payload[SSH_SESS_BUF];
    int off = 0;
    for (int i = 0; i < nparts; i++) {
        if (part_lens[i] > 0 && off + part_lens[i] <= SSH_SESS_BUF) {
            memcpy(payload + off, parts[i], part_lens[i]);
            off += part_lens[i];
        }
    }
    return ses_send_packet(s, type, payload, off);
}

/* ── SSH helpers ────────────────────────────────────────────── */

/* Read a string from packet data, returns pointer, length in *len */
static const uint8_t *ssh_read_string(const uint8_t *data, int data_len, int *off, int *len) {
    if (*off + 4 > data_len) { *len = 0; return NULL; }
    *len = g32(data + *off);
    *off += 4;
    if (*off + *len > data_len) { *len = data_len - *off + 4; if (*len < 0) *len = 0; }
    const uint8_t *result = data + *off;
    *off += *len;
    return result;
}

/* Read an mpint from packet */
static int ssh_read_mpint(const uint8_t *data, int data_len, int *off, bignum *bn) {
    int len;
    const uint8_t *mp = ssh_read_string(data, data_len, off, &len);
    if (!mp || len == 0) { memset(bn, 0, sizeof(*bn)); return -1; }
    bn_from_bytes(bn, mp, len);
    return 0;
}

/* Compute exchange hash H = hash(V_C || V_S || I_C || I_S || K_S || e || f || K) */
static int compute_exchange_hash(struct ssh_session *s,
                                  const uint8_t *host_key_blob, int host_key_blob_len,
                                  const bignum *e, const bignum *f,
                                  const bignum *K) {
    uint8_t buf[SSH_SESS_BUF];
    int off = 0;
    
    /* V_C: client version string (including \r\n) */
    const char *ver_c = "SSH-2.0-OSSSH\r\n";
    int ver_c_len = strlen(ver_c);
    memcpy(buf + off, ver_c, ver_c_len); off += ver_c_len;
    
    /* V_S: server version string (including \r\n) */
    const char *ver_s = "SSH-2.0-OSSSH\r\n";
    int ver_s_len = strlen(ver_s);
    memcpy(buf + off, ver_s, ver_s_len); off += ver_s_len;
    
    /* I_C: client KEXINIT payload (without length prefix) */
    /* We need to strip the 4-byte length from the stored packet */
    const uint8_t *client_kex_payload = s->client_kex;
    int client_kex_payload_len = s->client_kex_len;
    memcpy(buf + off, client_kex_payload, client_kex_payload_len); off += client_kex_payload_len;
    
    /* I_S: server KEXINIT payload */
    const uint8_t *server_kex_payload = s->server_kex;
    int server_kex_payload_len = s->server_kex_len;
    memcpy(buf + off, server_kex_payload, server_kex_payload_len); off += server_kex_payload_len;
    
    /* K_S: host key blob */
    memcpy(buf + off, host_key_blob, host_key_blob_len); off += host_key_blob_len;
    
    /* e: client DH public key (mpint) */
    off += pmpint(buf + off, e);
    
    /* f: server DH public key (mpint) */
    off += pmpint(buf + off, f);
    
    /* K: shared secret (mpint) */
    off += pmpint(buf + off, K);
    
    /* Hash it */
    sha256_hash(s->exchange_hash, buf, off);
    return 0;
}

/* Derive encryption keys after KEX */
static void derive_keys(struct ssh_session *s,
                         const bignum *K, const uint8_t *H, int H_len,
                         uint8_t session_id[32]) {
    uint8_t buf[64];
    int off;
    
    /* Initial IV client -> server: HASH(K || H || "A" || session_id) */
    /* We compute using SHA-256 in a sequence */
    
    /* For simplicity, derive using the standard SSH key derivation:
       Multiple rounds of HASH(K || H || X || session_id)
       where X = 'A', 'B', 'C', 'D', 'E', 'F' for each key */
    
    /* We need:
       IV_c2s = first 16 bytes of HASH(K || H || "A" || session_id)
       IV_s2c = first 16 bytes of HASH(K || H || "B" || session_id)
       Enc_c2s = first 16 bytes of HASH(K || H || "C" || session_id)
       Enc_s2c = first 16 bytes of HASH(K || H || "D" || session_id)
       MAC_c2s = HASH(K || H || "E" || session_id)
       MAC_s2c = HASH(K || H || "F" || session_id)
    */
    
    uint8_t K_bytes[256];
    int K_len = bn_to_bytes(K, K_bytes, 0);
    
    /* H is exchange_hash, H_len = 32 */
    char letters[] = {'A', 'B', 'C', 'D', 'E', 'F'};
    uint8_t output[6][32];
    
    for (int i = 0; i < 6; i++) {
        struct sha256_ctx ctx;
        sha256_init(&ctx);
        /* K (mpint format: length prefix + value) */
        uint8_t K_mpint[260];
        int mp_len = pmpint(K_mpint, K);
        /* Actually we need K as mpint without the int prefix - 
           let's do it simpler: K as bignum bytes with mpint format */
        uint8_t K_prefixed[260];
        int kp_len = 0;
        K_prefixed[kp_len++] = 0; K_prefixed[kp_len++] = 0; K_prefixed[kp_len++] = 0;
        K_prefixed[kp_len++] = K_len;
        memcpy(K_prefixed + kp_len, K_bytes, K_len);
        kp_len += K_len;
        
        sha256_update(&ctx, K_prefixed, kp_len);
        sha256_update(&ctx, H, H_len);
        sha256_update(&ctx, &letters[i], 1);
        sha256_update(&ctx, session_id, 32);
        sha256_final(output[i], &ctx);
    }
    
    /* Assign keys based on our role (server) */
    memcpy(s->send_cipher.iv, output[1], 16);    /* IV_s2c */
    memcpy(s->send_cipher.key, output[3], 16);   /* Enc_s2c */
    memcpy(s->send_mac_key, output[5], 32);      /* MAC_s2c */
    
    memcpy(s->recv_cipher.iv, output[0], 16);    /* IV_c2s */
    memcpy(s->recv_cipher.key, output[2], 16);   /* Enc_c2s */
    memcpy(s->recv_mac_key, output[4], 32);      /* MAC_c2s */
    
    /* Initialize AES contexts */
    aes_init(&s->send_cipher.ctx, s->send_cipher.key, 16);
    aes_init(&s->recv_cipher.ctx, s->recv_cipher.key, 16);
    
    s->seq_nr_send = 0;
    s->seq_nr_recv = 0;
    s->encrypted = 1;
}

/* ── Session I/O (shell output) ─────────────────────────────── */

static void ses_flush(struct ssh_session *s) {
    if (s->out_len > 0) {
        /* Send as SSH channel data */
        uint8_t payload[4 + 4 + s->out_len];
        int off = 0;
        /* recipient channel (4 bytes) */
        p32(payload + off, s->channel_id); off += 4;
        /* data string */
        p32(payload + off, s->out_len); off += 4;
        memcpy(payload + off, s->out_buf, s->out_len); off += s->out_len;
        ses_send_packet(s, SSH_MSG_CHANNEL_DATA, payload, off);
        s->out_len = 0;
    }
}

static void ses_write(struct ssh_session *s, const char *data, int len) {
    for (int i = 0; i < len; i++) {
        if (s->out_len >= 32256) ses_flush(s);
        if (s->out_len < 32768)
            s->out_buf[s->out_len++] = data[i];
    }
}

/* kprintf hook: redirects output to SSH session */
static void ssh_output_hook(char c, void *ctx) {
    struct ssh_session *s = (struct ssh_session *)ctx;
    if (c == '\n') {
        ses_write(s, "\r\n", 2);
    } else {
        ses_write(s, &c, 1);
    }
}

static void ssh_flush_hook(void *ctx) {
    ses_flush((struct ssh_session *)ctx);
}

/* ── Channel open handling ──────────────────────────────────── */

static void handle_channel_open(struct ssh_session *s, const uint8_t *data, int len) {
    int off = 0;
    int type_len;
    const uint8_t *type_str = ssh_read_string(data, len, &off, &type_len);
    if (!type_str) return;
    
    /* We only support "session" channels */
    if (type_len == 7 && memcmp(type_str, "session", 7) == 0) {
        /* Read sender channel and initial window */
        int sender_ch = g32(data + off); off += 4;
        uint32_t window = g32(data + off); off += 4;
        uint32_t max_pkt = g32(data + off); off += 4;
        (void)max_pkt;
        
        s->channel_id = 0; /* our channel number */
        s->rwnd = window;
        
        /* Send CHANNEL_OPEN_CONFIRMATION */
        uint8_t resp[20];
        int roff = 0;
        p32(resp + roff, sender_ch); roff += 4;  /* recipient channel */
        p32(resp + roff, s->channel_id); roff += 4;  /* sender channel */
        p32(resp + roff, 65536); roff += 4;  /* initial window */
        p32(resp + roff, 32768); roff += 4;  /* max packet */
        ses_send_packet(s, SSH_MSG_CHANNEL_OPEN_CONFIRMATION, resp, roff);
        
        s->channel_open = 1;
    } else {
        /* Reject unknown channel types */
        uint8_t resp[12];
        int roff = 0;
        int sender_ch = g32(data + off - 8); /* re-read */
        p32(resp + roff, sender_ch); roff += 4;
        p32(resp + roff, SSH_OPEN_UNKNOWN_CHANNEL_TYPE); roff += 4;
        p32(resp + roff, 0); roff += 4; /* language tag length */
        ses_send_packet(s, SSH_MSG_CHANNEL_OPEN_FAILURE, resp, roff);
    }
}

static void handle_channel_request(struct ssh_session *s, const uint8_t *data, int len) {
    int off = 0;
    uint32_t recip = g32(data + off); off += 4;
    int req_type_len;
    const uint8_t *req_type = ssh_read_string(data, len, &off, &req_type_len);
    if (!req_type) return;
    int want_reply = data[off++];
    
    if (req_type_len == 4 && memcmp(req_type, "shell", 4) == 0) {
        /* Start shell */
        if (want_reply)
            ses_send_packet(s, SSH_MSG_CHANNEL_SUCCESS, (uint8_t[]){ (recip >> 24) & 0xFF, (recip >> 16) & 0xFF, (recip >> 8) & 0xFF, recip & 0xFF }, 4);
        
        s->phase = SSH_PHASE_SESSION;
        
        /* Send welcome banner */
        kprintf_set_hook(ssh_output_hook, s);
        kprintf_set_flush(ssh_flush_hook, s);
        kprintf("\n=== OS SSH Remote Shell ===\n");
        kprintf("Type 'help' for commands, 'exit' to disconnect.\n\n");
        kprintf("os> ");
        kprintf_set_flush(0, 0);
        kprintf_set_hook(0, 0);
        ses_flush(s);
    } else if (req_type_len == 3 && memcmp(req_type, "pty", 3) == 0) {
        /* PTY allocation request - accept silently */
        if (want_reply)
            ses_send_packet(s, SSH_MSG_CHANNEL_SUCCESS, (uint8_t[]){ (recip >> 24) & 0xFF, (recip >> 16) & 0xFF, (recip >> 8) & 0xFF, recip & 0xFF }, 4);
    } else if (req_type_len == 7 && memcmp(req_type, "subsystem", 7) == 0) {
        /* Reject unknown subsystems */
        if (want_reply)
            ses_send_packet(s, SSH_MSG_CHANNEL_FAILURE, (uint8_t[]){ (recip >> 24) & 0xFF, (recip >> 16) & 0xFF, (recip >> 8) & 0xFF, recip & 0xFF }, 4);
    } else {
        /* Unknown request - fail if reply wanted */
        if (want_reply)
            ses_send_packet(s, SSH_MSG_CHANNEL_FAILURE, (uint8_t[]){ (recip >> 24) & 0xFF, (recip >> 16) & 0xFF, (recip >> 8) & 0xFF, recip & 0xFF }, 4);
    }
}

static void process_command(struct ssh_session *s) {
    char *cmd = s->cmd_buf;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;
    
    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        kprintf_set_hook(ssh_output_hook, s);
        kprintf("Goodbye!\n");
        kprintf_set_hook(0, 0);
        ses_flush(s);
        net_tcp_close(s->conn_id);
        s->active = 0;
        return;
    }
    
    shell_history_add(s->cmd_buf);
    s->processing = 1;
    
    kprintf_set_hook(ssh_output_hook, s);
    kprintf_set_flush(ssh_flush_hook, s);
    shell_process_line(cmd);
    kprintf_set_flush(0, 0);
    kprintf_set_hook(0, 0);
    
    s->processing = 0;
    
    kprintf_set_hook(ssh_output_hook, s);
    kprintf("os> ");
    kprintf_set_hook(0, 0);
    ses_flush(s);
}

/* ── Handle channel data ────────────────────────────────────── */
static void handle_channel_data(struct ssh_session *s, const uint8_t *data, int len) {
    int off = 0;
    uint32_t recip = g32(data + off); off += 4;
    (void)recip;
    int data_len;
    const uint8_t *chan_data = ssh_read_string(data, len, &off, &data_len);
    if (!chan_data || data_len == 0) return;
    
    /* Process each character */
    for (int i = 0; i < data_len; i++) {
        uint8_t c = chan_data[i];
        
        if (c == '\r') continue;
        if (c == '\n' || c == '\0') {
            s->cmd_buf[s->cmd_len] = '\0';
            ses_write(s, "\r\n", 2);
            ses_flush(s);
            process_command(s);
            s->cmd_len = 0;
            continue;
        }
        
        if (c == 127 || c == '\b') {
            if (s->cmd_len > 0) {
                s->cmd_len--;
                ses_write(s, "\b \b", 3);
                ses_flush(s);
            }
            continue;
        }
        
        if (c >= 32 && c < 127 && s->cmd_len < 1023) {
            s->cmd_buf[s->cmd_len++] = c;
            char echo = c;
            ses_write(s, &echo, 1);
            ses_flush(s);
        }
    }
}

/* ── Handle received SSH packets ────────────────────────────── */

static void handle_version(struct ssh_session *s) {
    /* Version exchange - respond with our version */
    const char *ver = "SSH-2.0-OSSSH\r\n";
    ses_send_raw(s, (const uint8_t *)ver, strlen(ver));
    s->phase = SSH_PHASE_KEX_INIT;
}

static void handle_kexinit(struct ssh_session *s, const uint8_t *data, int len) {
    /* Store client KEXINIT payload (skip the 4-byte packet length) */
    if (len > SSH_SESS_BUF) len = SSH_SESS_BUF;
    s->client_kex_len = (len > 4) ? len - 4 : 0;
    if (s->client_kex_len > 0)
        memcpy(s->client_kex, data + 4, s->client_kex_len);
    
    /* Build server KEXINIT */
    uint8_t kex[256];
    int off = 0;
    /* Cookie (16 random bytes) */
    rng_fill_buf(kex + off, 16); off += 16;
    
    /* Algorithm lists (as strings) */
    const char *kex_algos = "diffie-hellman-group14-sha256";
    const char *hostkey_algos = "ssh-rsa";
    const char *enc_algos = "aes128-cbc";
    const char *mac_algos = "hmac-sha2-256";
    const char *comp_algos = "none";
    const char *lang = "";
    
    off += pstr(kex + off, (const uint8_t *)kex_algos, strlen(kex_algos));
    off += pstr(kex + off, (const uint8_t *)hostkey_algos, strlen(hostkey_algos));
    off += pstr(kex + off, (const uint8_t *)enc_algos, strlen(enc_algos));
    off += pstr(kex + off, (const uint8_t *)enc_algos, strlen(enc_algos));
    off += pstr(kex + off, (const uint8_t *)mac_algos, strlen(mac_algos));
    off += pstr(kex + off, (const uint8_t *)mac_algos, strlen(mac_algos));
    off += pstr(kex + off, (const uint8_t *)comp_algos, strlen(comp_algos));
    off += pstr(kex + off, (const uint8_t *)comp_algos, strlen(comp_algos));
    off += pstr(kex + off, (const uint8_t *)lang, 0);
    off += pstr(kex + off, (const uint8_t *)lang, 0);
    /* First KEX packet follows flag */
    kex[off++] = 0;
    /* Reserved */
    kex[off++] = 0;
    /* Padding */
    int pad = 0;
    while ((off + pad + 1) % 8 != 0) pad++;
    rng_fill_buf(kex + off, pad); off += pad;
    
    /* Store server KEXINIT payload */
    s->server_kex_len = off;
    memcpy(s->server_kex, kex, off);
    
    /* Send KEXINIT as a packet */
    ses_send_raw(s, (const uint8_t[]){0,0,0,0}, 4); /* dummy - we'll send properly */
    /* Actually, send as proper packet */
    /* Build the full KEXINIT packet: packet_length || padding_length || SSH_MSG_KEXINIT || kex_data || padding */
    int kex_pkt_len = 1 + off + 16; /* enough for padding */
    uint8_t kex_pkt[512];
    kex_pkt[0] = (kex_pkt_len >> 24) & 0xFF;
    kex_pkt[1] = (kex_pkt_len >> 16) & 0xFF;
    kex_pkt[2] = (kex_pkt_len >> 8) & 0xFF;
    kex_pkt[3] = kex_pkt_len & 0xFF;
    
    int pad_len = 16 - ((1 + off) % 16);
    if (pad_len < 4) pad_len += 16;
    int actual_len = 1 + off + pad_len;
    kex_pkt[0] = (actual_len >> 24) & 0xFF;
    kex_pkt[1] = (actual_len >> 16) & 0xFF;
    kex_pkt[2] = (actual_len >> 8) & 0xFF;
    kex_pkt[3] = actual_len & 0xFF;
    kex_pkt[4] = pad_len;
    kex_pkt[5] = SSH_MSG_KEXINIT;
    memcpy(kex_pkt + 6, kex, off);
    rng_fill_buf(kex_pkt + 6 + off, pad_len);
    
    ses_send_raw(s, kex_pkt, 4 + actual_len);
    
    /* Now we wait for KEXDH_INIT */
}

static void handle_kexdh_init(struct ssh_session *s, const uint8_t *data, int len) {
    int off = 0;
    /* Skip packet length (4) and padding_length (1) and message type (1) */
    off = 0;
    int pkt_len = g32(data); off += 4;
    int pad_len = data[off++];
    uint8_t msg_type = data[off++]; (void)msg_type;
    
    /* Skip padding to get to the payload */
    /* Actually, the mpint e is right after the msg type byte */
    bignum e;
    if (ssh_read_mpint(data + off, len - off, &off, &e) < 0) return;
    
    /* Generate server's DH key pair */
    bignum f;
    dh_generate_key(&f, &s->dh_priv);
    bn_copy(&s->dh_pub, &f);
    
    /* Compute shared secret K = e^priv mod p */
    dh_compute_shared(&s->dh_shared, &e, &s->dh_priv);
    
    /* Get host key blob */
    int hk_len;
    const uint8_t *hk = rsa_get_pubkey_blob(&hk_len);
    
    /* Compute exchange hash */
    compute_exchange_hash(s, hk, hk_len, &e, &f, &s->dh_shared);
    memcpy(s->session_id, s->exchange_hash, 32);
    
    /* Sign the exchange hash with host key */
    uint8_t sig[256];
    rsa_sign(s->exchange_hash, sig);
    
    /* Build KEXDH_REPLY */
    uint8_t reply[4096];
    int roff = 0;
    
    /* Host key blob */
    memcpy(reply + roff, hk, hk_len); roff += hk_len;
    
    /* f (server's DH public key) */
    roff += pmpint(reply + roff, &f);
    
    /* Signature of H */
    /* Signature blob = string "ssh-rsa" || string(sig) */
    uint8_t sig_blob[512];
    int sblen = 0;
    sblen += pstr(sig_blob + sblen, (const uint8_t *)"ssh-rsa", 7);
    sblen += pstr(sig_blob + sblen, sig, 256);
    
    roff += pstr(reply + roff, sig_blob, sblen);
    
    ses_send_packet(s, SSH_MSG_KEXDH_REPLY, reply, roff);
}

static void handle_newkeys(struct ssh_session *s) {
    /* Derive all keys from shared secret */
    derive_keys(s, &s->dh_shared, s->exchange_hash, 32, s->session_id);
    
    s->phase = SSH_PHASE_NEWKEYS;
    s->kex_done = 1;
}

/* Handle SSH_MSG_SERVICE_REQUEST */
static void handle_service_request(struct ssh_session *s, const uint8_t *data, int len) {
    int off = 6; /* skip packet_length(4)+pad_len(1)+msg_type(1) */
    int name_len;
    const uint8_t *name = ssh_read_string(data, len, &off, &name_len);
    if (!name) return;
    
    if (name_len == 12 && memcmp(name, "ssh-userauth", 12) == 0) {
        /* Send SERVICE_ACCEPT */
        uint8_t resp[4];
        p32(resp, 12); /* ssh-userauth string len to echo */
        /* Actually SERVICE_ACCEPT just contains the service name */
        uint8_t accept[12 + 4];
        int aoff = 0;
        aoff += pstr(accept + aoff, name, name_len);
        ses_send_packet(s, SSH_MSG_SERVICE_ACCEPT, accept, aoff);
        s->phase = SSH_PHASE_AUTH;
    }
}

/* Handle SSH_MSG_USERAUTH_REQUEST */
static void handle_userauth_request(struct ssh_session *s, const uint8_t *data, int len) {
    int off = 6; /* skip header */
    
    int user_len;
    const uint8_t *user = ssh_read_string(data, len, &off, &user_len);
    int svc_len;
    const uint8_t *svc = ssh_read_string(data, len, &off, &svc_len);
    int method_len;
    const uint8_t *method = ssh_read_string(data, len, &off, &method_len);
    
    if (!user || !svc || !method) return;
    (void)svc;
    
    if (method_len == 8 && memcmp(method, "password", 8) == 0) {
        /* Read password */
        int has_changed = data[off++]; (void)has_changed;
        int pass_len;
        const uint8_t *pass = ssh_read_string(data, len, &off, &pass_len);
        
        if (pass) {
            char pass_buf[128];
            int pl = pass_len < 127 ? pass_len : 127;
            memcpy(pass_buf, pass, pl);
            pass_buf[pl] = '\0';
            
            char user_buf[64];
            int ul = user_len < 63 ? user_len : 63;
            memcpy(user_buf, user, ul);
            user_buf[ul] = '\0';
            
            /* Authenticate against system users */
            if (session_login(user_buf, pass_buf) == 0) {
                /* Success */
                ses_send_packet(s, SSH_MSG_USERAUTH_SUCCESS, NULL, 0);
                s->phase = SSH_PHASE_CHANNEL;
                return;
            }
        }
    }
    
    /* Authentication failed */
    uint8_t fail[64];
    int foff = 0;
    foff += pstr(fail + foff, (const uint8_t *)"password", 8);
    fail[foff++] = 0; /* partial success = false */
    ses_send_packet(s, SSH_MSG_USERAUTH_FAILURE, fail, foff);
}

/* ── Protocol dispatch ──────────────────────────────────────── */

static void process_ssh_data(struct ssh_session *s, const uint8_t *data, int len) {
    s->recv_len += len;
    if (s->recv_len > SSH_SESS_BUF) s->recv_len = SSH_SESS_BUF;
    memcpy(s->recv_buf, data, len < SSH_SESS_BUF ? len : SSH_SESS_BUF);
    
    if (s->phase == SSH_PHASE_VERSION) {
        /* Check for newline */
        for (int i = 0; i < s->recv_len; i++) {
            if (s->recv_buf[i] == '\n') {
                handle_version(s);
                s->recv_len = 0;
                return;
            }
        }
        return;
    }
    
    /* Process packets once we have at least length field */
    int consumed = 0;
    
    while (consumed < s->recv_len) {
        int remaining = s->recv_len - consumed;
        if (remaining < 4) break;
        
        int pkt_len = g32(s->recv_buf + consumed);
        if (pkt_len < 1 || pkt_len > 35000) break;
        
        int total_pkt = 4 + pkt_len;
        if (!s->encrypted)
            total_pkt += 0; /* no MAC before encryption */
        else
            total_pkt += 32; /* MAC length */
        
        if (remaining < total_pkt) break;
        
        uint8_t *pkt = s->recv_buf + consumed;
        int type;
        uint8_t *payload;
        int payload_len;
        
        if (!s->encrypted) {
            int pad = pkt[4];
            type = pkt[5];
            payload = pkt + 6;
            payload_len = pkt_len - pad - 1 - 1;
            if (payload_len < 0) payload_len = 0;
        } else {
            /* Decrypt the packet (from byte 4 onwards) */
            uint8_t decrypted[32768];
            int dec_len = pkt_len;
            if (dec_len > 32768) dec_len = 32768;
            memcpy(decrypted, pkt + 4, dec_len);
            aes_cbc_decrypt(&s->recv_cipher.ctx, s->recv_cipher.iv,
                           decrypted, decrypted, dec_len);
            
            int pad = decrypted[0];
            type = decrypted[1];
            payload = decrypted + 2;
            payload_len = pkt_len - pad - 1 - 1;
            if (payload_len < 0) payload_len = 0;
            s->seq_nr_recv++;
        }
        
        /* Dispatch */
        switch (s->phase) {
            case SSH_PHASE_KEX_INIT:
                if (type == SSH_MSG_KEXINIT) {
                    handle_kexinit(s, pkt, total_pkt);
                    s->phase = SSH_PHASE_KEX;
                }
                break;
            
            case SSH_PHASE_KEX:
                if (type == SSH_MSG_KEXDH_INIT) {
                    handle_kexdh_init(s, pkt, total_pkt);
                } else if (type == SSH_MSG_NEWKEYS) {
                    handle_newkeys(s);
                }
                break;
            
            case SSH_PHASE_NEWKEYS:
                if (type == SSH_MSG_SERVICE_REQUEST)
                    handle_service_request(s, pkt, total_pkt);
                break;
            
            case SSH_PHASE_AUTH:
                if (type == SSH_MSG_USERAUTH_REQUEST)
                    handle_userauth_request(s, pkt, total_pkt);
                break;
            
            case SSH_PHASE_CHANNEL:
                if (type == SSH_MSG_CHANNEL_OPEN)
                    handle_channel_open(s, payload, payload_len);
                else if (type == SSH_MSG_CHANNEL_REQUEST)
                    handle_channel_request(s, payload, payload_len);
                break;
            
            case SSH_PHASE_SESSION:
                if (type == SSH_MSG_CHANNEL_DATA)
                    handle_channel_data(s, payload, payload_len);
                else if (type == SSH_MSG_CHANNEL_CLOSE) {
                    net_tcp_close(s->conn_id);
                    s->active = 0;
                } else if (type == SSH_MSG_CHANNEL_REQUEST)
                    handle_channel_request(s, payload, payload_len);
                break;
            
            default:
                break;
        }
        
        consumed += total_pkt;
    }
    
    /* Remove consumed data */
    if (consumed > 0 && consumed < s->recv_len) {
        memmove(s->recv_buf, s->recv_buf + consumed, s->recv_len - consumed);
        s->recv_len -= consumed;
    } else if (consumed >= s->recv_len) {
        s->recv_len = 0;
    }
}

/* ── Connection callbacks ────────────────────────────────────── */

static void on_connect(int conn_id) {
    struct ssh_session *s = alloc_session(conn_id);
    if (!s) { net_tcp_close(conn_id); return; }
    service_log("sshd", "client connected");
    
    /* Initialize DH */
    dh_init();
    
    /* Send SSH version string - the version exchange starts from server */
    const char *ver = "SSH-2.0-OSSSH\r\n";
    ses_send_raw(s, (const uint8_t *)ver, strlen(ver));
}

static void on_data(int conn_id, const void *data, uint16_t len) {
    struct ssh_session *s = find_session(conn_id);
    if (!s) return;
    if (s->processing) return;
    
    process_ssh_data(s, (const uint8_t *)data, len);
}

static void on_close(int conn_id) {
    struct ssh_session *s = find_session(conn_id);
    if (s) s->active = 0;
    service_log("sshd", "client disconnected");
}

/* ── Public API ──────────────────────────────────────────────── */

int sshd_start(void) {
    if (sshd_running) return 0;
    memset(sessions, 0, sizeof(sessions));
    net_tcp_listen(22, on_connect, on_data, on_close);
    sshd_running = 1;
    return 0;
}

void sshd_stop(void) {
    if (!sshd_running) return;
    net_tcp_unlisten(22);
    memset(sessions, 0, sizeof(sessions));
    sshd_running = 0;
}

void sshd_init(void) {
    dh_init();
    kprintf("[OK] SSH daemon crypto initialized\n");
    sshd_start();
}

void sshd_task(void) {
    for (;;) {
        net_wait_for_packet();
        net_poll();
    }
}
