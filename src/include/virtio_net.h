#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "types.h"

/* Initialize virtio-net (PCI 1AF4:1000).  Returns 0 if found, -1 if absent. */
int  virtio_net_init(void);
int  virtio_net_send(const uint8_t *data, uint32_t len);
int  virtio_net_receive(void *buf, uint16_t max_len);
void virtio_net_get_mac(uint8_t *mac);
int virtio_net_present(void);
void virtio_net_irq_rearm(void);

#endif
