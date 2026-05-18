#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "types.h"

/* Initialize virtio-net (PCI 1AF4:1000).  Returns 0 if found, -1 if absent. */
int  virtio_net_init(void);
/* Send a raw frame.  Delegates to e1000 if virtio-net was not found. */
void virtio_net_send(const uint8_t *data, uint32_t len);
/* Returns 1 if a virtio-net device is present. */
int  virtio_net_present(void);

#endif
