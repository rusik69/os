#ifndef PPTP_H
#define PPTP_H

#include "types.h"

int pptp_call_create(uint16_t call_id);
int pptp_call_connect(int call_idx, uint16_t peer_call_id, uint32_t peer_ip);
int pptp_call_disconnect(int call_idx);
int pptp_call_delete(int call_idx);
int pptp_gre_encap(int call_idx, const void *payload, uint32_t payload_len,
                   void *out_buf, uint32_t out_len);
int pptp_gre_decap(const void *in_buf, uint32_t in_len,
                   void *payload, uint32_t *payload_len);
void pptp_init(void);

#endif /* PPTP_H */
