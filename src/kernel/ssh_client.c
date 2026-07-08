#define KERNEL_INTERNAL
#include "types.h"
#include "sha256.h"
#include "aes.h"
#include "hmac.h"
#include "rng.h"
#include "heap.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "ssh.h"
#include "ssh_client.h"

/* ── Crypto forward declarations ────────────────────────────── */
extern void bn_from_bytes(bignum *r, const uint8_t *bytes, int len);
extern int bn_to_bytes(const bignum *a, uint8_t *bytes, int min_len);
extern int bn_compare(const bignum *a, const bignum *b);
extern void bn_copy(bignum *r, const bignum *a);
extern void bn_set_u32(bignum *r, uint32_t v);
extern void bn_mod_exp(bignum *r, const bignum *base, const bignum *exp, const bignum *mod);
extern void bn_mod_mul(bignum *r, const bignum *a, const bignum *b, const bignum *m);
extern void bn_mod(bignum *r, const bignum *a, const bignum *m);
extern void bn_random(bignum *r, const bignum *max);
extern void dh_init(void);
extern const bignum *dh_get_p(void);
extern const bignum *dh_get_g(void);
extern void dh_generate_key(bignum *pub, bignum *priv);
extern void dh_compute_shared(bignum *shared, const bignum *their_pub, const bignum *my_priv);

/* ── Pack helpers ───────────────────────────────────────────── */
static void pc32(uint8_t *b, uint32_t v) {
    b[0]=(uint8_t)((v>>24)&0xFF);b[1]=(uint8_t)((v>>16)&0xFF);b[2]=(uint8_t)((v>>8)&0xFF);b[3]=(uint8_t)(v&0xFF);
}
static uint32_t gc32(const uint8_t *b) {
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
}
static int pstr32(uint8_t *b, const uint8_t *d, int l) {
    pc32(b,l);if(l)memcpy(b+4,d,l);return 4+l;
}
static int pmpint32(uint8_t *b, const bignum *bn) {
    uint8_t tmp[257];
    int len=bn_to_bytes(bn,tmp+1,0);
    int start=1;
    while(start<=len&&tmp[start]==0)start++;
    int dl=len-start+1;
    if(dl<=0){pc32(b,0);return 4;}
    if(tmp[start]&0x80){tmp[start-1]=0;start--;dl++;}
    pc32(b,dl);memcpy(b+4,tmp+start,dl);
    return 4+dl;
}

/* ── SSH client session struct ──────────────────────────────── */
#define CLI_BUF 32768

struct ssh_client {
    int conn_id;
    int connected;
    int ready;

    uint32_t seq_send, seq_recv;
    int encrypted;
    struct aes_ctx send_ctx, recv_ctx;
    uint8_t send_iv[16], recv_iv[16];
    uint8_t send_mac_key[32], recv_mac_key[32];

    bignum dh_priv;
    bignum dh_shared;
    uint8_t exchange_hash[32];
    uint8_t session_id[32];

    int phase;
    uint8_t rbuf[CLI_BUF];
    int rlen;

    char user[64];
    char pass[128];

    uint32_t channel_id;
    int channel_open;

    ssh_output_fn on_output;
    ssh_close_fn on_close;
    void *ctx;
};

/* Low-level send */
static int cl_send(struct ssh_client *c, const void *d, int l) {
    return net_tcp_send(c->conn_id, d, (uint16_t)l);
}

/* Send SSH packet */
static int cl_pkt(struct ssh_client *c, uint8_t type, const uint8_t *pl, int plen) {
    uint8_t pkt[CLI_BUF];
    int pad = 16 - ((1+plen)%16);
    if(pad<4)pad+=16;
    int total = 1+plen+pad;

    pkt[0]=(uint8_t)((total>>24)&0xFF);pkt[1]=(uint8_t)((total>>16)&0xFF);
    pkt[2]=(uint8_t)((total>>8)&0xFF);pkt[3]=(uint8_t)(total&0xFF);
    pkt[4]=(uint8_t)pad;int off=5;
    pkt[off++]=type;
    if(pl&&plen){memcpy(pkt+off,pl,plen);off+=plen;}
    rng_fill_buf(pkt+off,pad);off+=pad;

    if(!c->encrypted) {
        cl_send(c,pkt,4+total);
        c->seq_send++;
        return 0;
    }

    uint8_t enc[CLI_BUF];
    memcpy(enc,pkt,4);
    aes_cbc_encrypt(&c->send_ctx,c->send_iv,pkt+4,enc+4,total);

    /* Compute MAC (use pkt as scratch: shift right by 4 to prepend seq_nr) */
    uint8_t mac[32], sb[4];
    pc32(sb, c->seq_send);
    memmove(pkt + 4, pkt, 4 + total);
    memcpy(pkt, sb, 4);
    hmac_sha256(c->send_mac_key, 32, pkt, 4 + 4 + total, mac);

    cl_send(c,enc,4+total);
    cl_send(c,mac,32);
    c->seq_send++;
    return 0;
}

/* String/mpint readers */
static const uint8_t *cstr(const uint8_t *d, int dl, int *off, int *l) {
    if(*off+4>dl){*l=0;return NULL;}
    *l=gc32(d+*off);*off+=4;
    if(*off+*l>dl){*l=0;return NULL;}
    const uint8_t *r=d+*off;*off+=*l;
    return r;
}
static int cmint(const uint8_t *d, int dl, int *off, bignum *bn) {
    int l;const uint8_t *mp=cstr(d,dl,off,&l);
    if(!mp||!l){memset(bn,0,sizeof(*bn));return -1;}
    bn_from_bytes(bn,mp,l);
    return 0;
}

/* Compute exchange hash (client side) */
static void cl_hash(struct ssh_client *c, const uint8_t *hk, int hkl,
                     const bignum *e, const bignum *f, const bignum *K,
                     const uint8_t *ck, int ckl, const uint8_t *sk, int skl) {
    uint8_t *buf = kmalloc(4096);
    if (!buf) return;
    int off=0;
    const char *vc="SSH-2.0-OSSSH\r\n";
    const char *vs="SSH-2.0-OSSSH\r\n";
    memcpy(buf+off,vc,strlen(vc));off+=(int)strlen(vc);
    memcpy(buf+off,vs,strlen(vs));off+=(int)strlen(vs);
    if(ck&&ckl){memcpy(buf+off,ck,ckl);off+=ckl;}
    if(sk&&skl){memcpy(buf+off,sk,skl);off+=skl;}
    if(hk&&hkl){memcpy(buf+off,hk,hkl);off+=hkl;}
    off+=pmpint32(buf+off,e);
    off+=pmpint32(buf+off,f);
    off+=pmpint32(buf+off,K);
    sha256_hash(c->exchange_hash,buf,off);
    kfree(buf);
}

/* Derive keys (client side) */
static void cl_keys(struct ssh_client *c, const bignum *K,
                     const uint8_t *H, int Hl, uint8_t *sid) {
    uint8_t Kb[256];int Kl=bn_to_bytes(K,Kb,0);
    char ltrs[]={'A','B','C','D','E','F'};
    uint8_t out[6][32];
    for(int i=0;i<6;i++){
        struct sha256_ctx ctx;sha256_init(&ctx);
        uint8_t kp[260];int kl=0;
        kp[kl++]=(uint8_t)0;kp[kl++]=(uint8_t)0;kp[kl++]=(uint8_t)0;kp[kl++]=(uint8_t)Kl;
        memcpy(kp+kl,Kb,Kl);kl+=Kl;
        sha256_update(&ctx,kp,kl);
        sha256_update(&ctx,H,Hl);
        sha256_update(&ctx,&ltrs[i],1);
        sha256_update(&ctx,sid,32);
        sha256_final(out[i],&ctx);
    }
    memcpy(c->send_iv,out[0],16);memcpy(c->send_mac_key,out[4],32);
    aes_init(&c->send_ctx,out[2],16);
    memcpy(c->recv_iv,out[1],16);memcpy(c->recv_mac_key,out[5],32);
    aes_init(&c->recv_ctx,out[3],16);
    c->seq_send=0;c->seq_recv=0;c->encrypted=1;
}

/* Process an SSH packet */
static void cl_dispatch(struct ssh_client *c, uint8_t type, uint8_t *pl, int plen) {
    int off;

    switch(type) {
    case SSH_MSG_KEXINIT:
        if(c->phase==0||c->phase==1) {
            c->phase=1;
            bignum e;dh_init();dh_generate_key(&e,&c->dh_priv);
            uint8_t kx[512];int ko=0;ko+=pmpint32(kx+ko,&e);
            cl_pkt(c,SSH_MSG_KEXDH_INIT,kx,ko);
            c->phase=2;
        }
        break;

    case SSH_MSG_KEXDH_REPLY: {
        off=0;
        int hkl;const uint8_t *hk=cstr(pl,plen,&off,&hkl);
        bignum f;cmint(pl,plen,&off,&f);
        int sgl;cstr(pl,plen,&off,&sgl);(void)sgl;
        dh_compute_shared(&c->dh_shared,&f,&c->dh_priv);
        cl_hash(c,hk,hkl,&c->dh_priv,&f,&c->dh_shared,NULL,0,NULL,0);
        memcpy(c->session_id,c->exchange_hash,32);
        cl_pkt(c,SSH_MSG_NEWKEYS,NULL,0);
        cl_keys(c,&c->dh_shared,c->exchange_hash,32,c->session_id);
        c->phase=3;
        break;
    }

    case SSH_MSG_NEWKEYS:
        c->phase=4;
        {
            uint8_t svc[16];int so=pstr32(svc,(const uint8_t*)"ssh-userauth",12);
            cl_pkt(c,SSH_MSG_SERVICE_REQUEST,svc,so);
        }
        break;

    case SSH_MSG_SERVICE_ACCEPT:
        {
            uint8_t auth[512];int ao=0;
            ao+=pstr32(auth+ao,(const uint8_t*)c->user,(int)strlen(c->user));
            ao+=pstr32(auth+ao,(const uint8_t*)"ssh-connection",14);
            ao+=pstr32(auth+ao,(const uint8_t*)"password",8);
            auth[ao++]=0;
            ao+=pstr32(auth+ao,(const uint8_t*)c->pass,(int)strlen(c->pass));
            cl_pkt(c,SSH_MSG_USERAUTH_REQUEST,auth,ao);
            c->phase=5;
        }
        break;

    case SSH_MSG_USERAUTH_SUCCESS:
        c->phase=6;
        {
            uint8_t ch[32];int co=0;
            co+=pstr32(ch+co,(const uint8_t*)"session",7);
            pc32(ch+co,0);co+=4;
            pc32(ch+co,65536);co+=4;
            pc32(ch+co,32768);co+=4;
            cl_pkt(c,SSH_MSG_CHANNEL_OPEN,ch,co);
        }
        break;

    case SSH_MSG_USERAUTH_FAILURE:
        if(c->on_close) c->on_close(c->ctx);
        net_tcp_close(c->conn_id);
        c->connected=0;
        break;

    case SSH_MSG_CHANNEL_OPEN_CONFIRMATION:
        off=0;
        gc32(pl+off);off+=4;
        c->channel_id=gc32(pl+off);off+=4;
        c->channel_open=1;
        c->phase=6;
        c->ready=1;
        if(c->on_output) c->on_output("\n",1,c->ctx);
        break;

    case SSH_MSG_CHANNEL_OPEN_FAILURE:
        if(c->on_close) c->on_close(c->ctx);
        net_tcp_close(c->conn_id);
        c->connected=0;
        break;

    case SSH_MSG_CHANNEL_DATA: {
        off=0;gc32(pl+off);off+=4;
        int dl;const uint8_t *d=cstr(pl,plen,&off,&dl);
        if(d&&dl>0&&c->on_output) c->on_output((const char*)d,dl,c->ctx);
        break;
    }

    case SSH_MSG_CHANNEL_CLOSE:
        if(c->on_close) c->on_close(c->ctx);
        net_tcp_close(c->conn_id);
        c->connected=0;
        break;

    case SSH_MSG_CHANNEL_EOF: break;
    case SSH_MSG_DEBUG: case SSH_MSG_IGNORE: break;

    default:
        {uint8_t ui[4];pc32(ui,type);cl_pkt(c,SSH_MSG_UNIMPLEMENTED,ui,4);}
        break;
    }
}

/* Process raw data from network */
static void cl_feed(struct ssh_client *c, const uint8_t *data, int len) {
    if(c->rlen+len>CLI_BUF)len=CLI_BUF-c->rlen;
    memcpy(c->rbuf+c->rlen,data,len);
    c->rlen+=len;

    int consumed=0;
    while(consumed<c->rlen) {
        int rem=c->rlen-consumed;
        if(rem<4)break;
        int pktlen=gc32(c->rbuf+consumed);
        if(pktlen<1||pktlen>35000)break;
        int total=4+pktlen+(c->encrypted?32:0);
        if(rem<total)break;

        uint8_t *pkt=c->rbuf+consumed;
        uint8_t type;uint8_t *pl;int plen;

        if(!c->encrypted) {
            int pad=pkt[4];type=pkt[5];pl=pkt+6;
            plen=pktlen-pad-1-1;if(plen<0)plen=0;
        } else {
            /* Decrypt in-place in the rbuf (overwriting encrypted data) */
            int dl=pktlen; /* decrypt from byte 4 onwards */
            aes_cbc_decrypt(&c->recv_ctx,c->recv_iv,pkt+4,pkt+4,dl);
            int pad=pkt[4];type=pkt[5];pl=pkt+6;
            plen=pktlen-pad-1-1;if(plen<0)plen=0;
            c->seq_recv++;
        }

        cl_dispatch(c,type,pl,plen);
        consumed+=total;
    }

    if(consumed>0&&consumed<c->rlen)
        memmove(c->rbuf,c->rbuf+consumed,c->rlen-consumed),c->rlen-=consumed;
    else if(consumed>=c->rlen)
        c->rlen=0;
}

/* ── Public API ──────────────────────────────────────────────── */

struct ssh_client *ssh_client_connect(const char *host, uint16_t port,
                                       const char *user, const char *pass,
                                       ssh_output_fn on_output,
                                       ssh_close_fn on_close,
                                       void *ctx) {
    /* Resolve hostname */
    uint32_t ip = net_dns_resolve(host);
    if(ip==0) {
        /* Manual IP parse: "a.b.c.d" */
        const char *p = host;
        unsigned int oct[4] = {0,0,0,0};
        int oi = 0, val = 0, dots = 0;
        while (*p && oi < 4) {
            if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); }
            else if (*p == '.') { oct[oi++] = val; val = 0; dots++; }
            else break;
            p++;
        }
        if (dots == 3 && oi == 3) { oct[oi] = val; oi++; }
        if (oi == 4)
            ip = (oct[0]<<24)|(oct[1]<<16)|(oct[2]<<8)|oct[3];
        else return NULL;
    }

    int conn_id=net_tcp_connect(ip,port);
    if(conn_id<0) return NULL;

    struct ssh_client *cl = (struct ssh_client*)kmalloc(sizeof(struct ssh_client));
    if(!cl) { net_tcp_close(conn_id); return NULL; }

    memset(cl,0,sizeof(*cl));
    cl->conn_id=conn_id;
    cl->connected=1;
    cl->ready=0;
    cl->phase=0;
    strncpy(cl->user,user,sizeof(cl->user)-1);
    strncpy(cl->pass,pass,sizeof(cl->pass)-1);
    cl->on_output=on_output;
    cl->on_close=on_close;
    cl->ctx=ctx;

    cl_send(cl,"SSH-2.0-OSSSH\r\n",17);

    return cl;
}

void ssh_client_poll(struct ssh_client *cl) {
    if(!cl||!cl->connected) return;
    net_poll();
    /* Read any received data from TCP buffer */
    uint8_t *buf = kmalloc(4096);
    if (!buf) return;
    int n = net_tcp_recv(cl->conn_id, buf, 4096, 0);
    if(n > 0) cl_feed(cl, buf, n);
    kfree(buf);
}

int ssh_client_send(struct ssh_client *cl, const char *data, int len) {
    if(!cl||!cl->channel_open||!cl->connected) return -1;
    uint8_t pkt[CLI_BUF];int po=0;
    pc32(pkt+po,cl->channel_id);po+=4;
    po+=pstr32(pkt+po,(const uint8_t*)data,len);
    cl_pkt(cl,SSH_MSG_CHANNEL_DATA,pkt,po);
    return 0;
}

void ssh_client_close(struct ssh_client *cl) {
    if(!cl) return;
    if(cl->channel_open&&cl->connected) {
        uint8_t cp[4];pc32(cp,0);
        cl_pkt(cl,SSH_MSG_CHANNEL_EOF,cp,4);
        cl_pkt(cl,SSH_MSG_CHANNEL_CLOSE,cp,4);
    }
    net_tcp_close(cl->conn_id);
    cl->connected=0;
    kfree(cl);
}

int ssh_client_connected(struct ssh_client *cl) {
    return cl&&cl->connected;
}

int ssh_client_ready(struct ssh_client *cl) {
    return cl&&cl->ready;
}

/* ── Stub: ssh_client_disconnect ─────────────────────────────── */
static int ssh_client_disconnect(void *session)
{
    (void)session;
    kprintf("[ssh] ssh_client_disconnect: not yet implemented\n");
    return 0;
}
/* ── Stub: ssh_client_exec ─────────────────────────────── */
static int ssh_client_exec(void *session, const char *cmd, void *output)
{
    (void)session;
    (void)cmd;
    (void)output;
    kprintf("[ssh] ssh_client_exec: not yet implemented\n");
    return 0;
}
