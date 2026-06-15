#ifndef L2TP_H
#define L2TP_H

#include "types.h"

int l2tp_session_create(uint32_t session_id, uint32_t tunnel_id);
int l2tp_session_delete(int session_id);
int l2tp_session_set_peer(int session_id, uint32_t peer_sid,
                          uint32_t peer_tid, uint32_t peer_ip);
int l2tp_session_set_cookie(int session_id, uint64_t cookie, int len);
int l2tp_encap(int session_id, const void *payload, uint32_t payload_len,
               void *out_buf, uint32_t out_len);
int l2tp_decap(const void *in_buf, uint32_t in_len,
               void *payload, uint32_t *payload_len);
void l2tp_init(void);

#endif /* L2TP_H */
