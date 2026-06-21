#!/usr/bin/env python3
"""
Implement all stub functions in drivers/ and power/ subsystems.
"""
import json
import os
import re
import sys

SRC_DIR = os.path.expanduser("~/os")
INVENTORY = "/tmp/stub_inventory.json"

def load_inventory():
    with open(INVENTORY, 'r') as f:
        return json.load(f)

def read_file(path):
    with open(path, 'r') as f:
        return f.read()

def write_file(path, content):
    with open(path, 'w') as f:
        f.write(content)

# All our implementations in one big dict: file_basename -> { func_name: code }
ALL_IMPLS = {}

def reg(file_base, func_name, code):
    if file_base not in ALL_IMPLS:
        ALL_IMPLS[file_base] = {}
    ALL_IMPLS[file_base][func_name] = code

# ============ ATA ============
reg('ata.c', 'ata_identify', '''int ata_identify(void *ident_data)
{
    if (!ident_data)
        return -EINVAL;
    if (!ata_present)
        return -ENODEV;
    if (ata_wait_bsy() < 0)
        return -EIO;
    outb(ATA_DRIVE_HEAD, 0xA0);
    ata_400ns_delay();
    outb(ATA_SECT_CNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_400ns_delay();
    uint8_t status = inb(ATA_STATUS);
    if (status == 0) return -ENODEV;
    if (ata_wait_bsy() < 0) return -EIO;
    for (int timeout = 0; timeout < 100000; timeout++) {
        status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) return -EIO;
        if (status & ATA_SR_DRQ) break;
        __asm__ volatile("pause");
    }
    if (!(status & ATA_SR_DRQ)) return -EIO;
    uint16_t *ident = (uint16_t *)ident_data;
    for (int i = 0; i < 256; i++)
        ident[i] = inw(ATA_DATA);
    kprintf("[ata] IDENTIFY successful\\n");
    return 0;
}''')

reg('ata.c', 'ata_reset', '''int ata_reset(int bus)
{
    (void)bus;
    kprintf("[ata] Soft resetting ATA bus\\n");
    if (bus == 0) {
        outb(0x3F6, 0x04);
        ata_400ns_delay();
        udelay(5);
        outb(0x3F6, 0x00);
        ata_400ns_delay();
        if (ata_wait_bsy() < 0) { kprintf("[ata] Reset failed\\n"); return -EIO; }
        ata_present = 0;
        return 0;
    } else if (bus == 1) {
        outb(0x376, 0x04);
        ata_400ns_delay();
        udelay(5);
        outb(0x376, 0x00);
        ata_400ns_delay();
        return 0;
    }
    return -EINVAL;
}''')

# ============ FLOPPY ============
reg('floppy.c', 'floppy_read_sector', '''int floppy_read_sector(uint32_t sector, void *buf)
{
    if (!g_floppy_present) return -ENODEV;
    if (!buf) return -EINVAL;
    int head = (sector / (g_spt * g_heads)) % g_heads;
    int cyl = sector / (g_spt * g_heads);
    int sect = sector % g_spt + 1;
    if (floppy_seek(cyl, head) < 0) return -EIO;
    g_floppy_irq_received = 0;

    outb(g_fdc_base + FDC_FIFO, FDC_CMD_READ_DATA);
    fdc_send_byte(head << 2 | g_drive);
    fdc_send_byte(cyl);
    fdc_send_byte(head);
    fdc_send_byte(sect);
    fdc_send_byte(2);
    fdc_send_byte(g_spt);
    fdc_send_byte(0x1B);
    fdc_send_byte(0xFF);

    int timeout = 100000;
    while (!g_floppy_irq_received && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;

    /* Program DMA for read */
    dma_program(g_dma_chan, (uint32_t)buf, SECTOR_SIZE, DMA_MODE_READ);

    uint8_t st0 = fdc_recv_byte();
    fdc_recv_byte(); fdc_recv_byte(); fdc_recv_byte();
    fdc_recv_byte(); fdc_recv_byte(); fdc_recv_byte();
    if (st0 & 0xC0) { kprintf("[floppy] Read error\\n"); return -EIO; }
    return 0;
}''')

reg('floppy.c', 'floppy_write_sector', '''int floppy_write_sector(uint32_t sector, const void *buf)
{
    if (!g_floppy_present) return -ENODEV;
    if (!buf) return -EINVAL;
    int head = (sector / (g_spt * g_heads)) % g_heads;
    int cyl = sector / (g_spt * g_heads);
    int sect = sector % g_spt + 1;
    if (floppy_seek(cyl, head) < 0) return -EIO;
    g_floppy_irq_received = 0;

    outb(g_fdc_base + FDC_FIFO, 0xC5);
    fdc_send_byte(head << 2 | g_drive);
    fdc_send_byte(cyl);
    fdc_send_byte(head);
    fdc_send_byte(sect);
    fdc_send_byte(2);
    fdc_send_byte(g_spt);
    fdc_send_byte(0x1B);
    fdc_send_byte(0xFF);

    dma_program(g_dma_chan, (uint32_t)buf, SECTOR_SIZE, DMA_MODE_WRITE);

    int timeout = 100000;
    while (!g_floppy_irq_received && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;
    uint8_t st0 = fdc_recv_byte();
    fdc_recv_byte(); fdc_recv_byte(); fdc_recv_byte();
    fdc_recv_byte(); fdc_recv_byte(); fdc_recv_byte();
    if (st0 & 0xC0) { kprintf("[floppy] Write error\\n"); return -EIO; }
    return 0;
}''')

reg('floppy.c', 'floppy_reset', '''int floppy_reset(void)
{
    if (!g_floppy_present) return -ENODEV;
    kprintf("[floppy] Resetting controller\\n");
    outb(g_fdc_base + FDC_DOR, 0x00);
    udelay(10);
    outb(g_fdc_base + FDC_DOR, DOR_IRQ | DOR_RESET);
    g_floppy_irq_received = 0;
    int timeout = 100000;
    while (!g_floppy_irq_received && --timeout > 0) io_wait();
    if (timeout == 0) { kprintf("[floppy] Reset timeout\\n"); return -EIO; }
    /* Sense Interrupt Status to clear IRQ */
    fdc_recv_byte(); fdc_recv_byte();
    /* Send SPECIFY command */
    fdc_send_byte(FDC_CMD_SPECIFY);
    fdc_send_byte(0xDF);
    fdc_send_byte(0x02);
    return 0;
}''')

reg('floppy.c', 'floppy_calibrate', '''int floppy_calibrate(int drive)
{
    if (!g_floppy_present) return -ENODEV;
    kprintf("[floppy] Calibrating drive %d\\n", drive);
    outb(g_fdc_base + FDC_DOR, DOR_IRQ | DOR_RESET | (1 << (4 + drive)) | drive);
    udelay(1000);
    for (int retry = 0; retry < 4; retry++) {
        g_floppy_irq_received = 0;
        fdc_send_byte(FDC_CMD_RECALIBRATE);
        fdc_send_byte(drive);
        int timeout = 100000;
        while (!g_floppy_irq_received && --timeout > 0) io_wait();
        if (timeout == 0) continue;
        uint8_t st0 = fdc_recv_byte();
        fdc_recv_byte();
        if (!(st0 & 0xC0)) { kprintf("[floppy] Calibrate OK\\n"); return 0; }
    }
    return -EIO;
}''')

reg('floppy.c', 'floppy_seek', '''int floppy_seek(int cylinder, int head)
{
    if (!g_floppy_present) return -ENODEV;
    g_floppy_irq_received = 0;
    fdc_send_byte(FDC_CMD_SEEK);
    fdc_send_byte(head << 2 | g_drive);
    fdc_send_byte(cylinder);
    int timeout = 100000;
    while (!g_floppy_irq_received && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;
    uint8_t st0 = fdc_recv_byte();
    fdc_recv_byte();
    if (st0 & 0xC0) return -EIO;
    return 0;
}''')

reg('floppy.c', 'floppy_recalibrate', '''int floppy_recalibrate(void)
{
    return floppy_calibrate(g_drive);
}''')

reg('floppy.c', 'floppy_irq_handler', '''void floppy_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    g_floppy_irq_received = 1;
    send_eoi(6);
}''')

reg('floppy.c', 'floppy_timer', '''void floppy_timer(void)
{
    static int motor_timeout = 0;
    if (g_floppy_motor_on) {
        if (++motor_timeout > 200) {
            motor_timeout = 0;
            outb(g_fdc_base + FDC_DOR, DOR_IRQ | DOR_RESET);
            g_floppy_motor_on = 0;
        }
    } else {
        motor_timeout = 0;
    }
}''')

# ============ E1000 ============
reg('e1000.c', 'e1000_send_packet', '''int e1000_send_packet(struct net_device *dev, const void *data, int len)
{
    if (!dev || !data || len <= 0) return -EINVAL;
    struct e1000_device *adapter = (struct e1000_device *)dev->priv;
    if (!adapter) return -ENODEV;
    uint32_t next = adapter->tx_cur;
    if (len > E1000_TX_BUF_SIZE) len = E1000_TX_BUF_SIZE;
    __builtin_memcpy(adapter->tx_bufs[next], data, len);
    adapter->tx_ring[next].addr = (uint64_t)(uintptr_t)adapter->tx_bufs[next];
    adapter->tx_ring[next].len  = len;
    adapter->tx_ring[next].cmd  = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS;
    adapter->tx_ring[next].status = 0;
    wmb();
    adapter->tx_cur = (next + 1) % adapter->tx_ring_size;
    e1000_write_reg(adapter, REG_TDT(adapter->tx_q_idx), adapter->tx_cur);
    int timeout = 10000;
    while (!(adapter->tx_ring[next].status & E1000_TXD_STAT_DD) && --timeout > 0) io_wait();
    if (timeout == 0) { kprintf("[e1000] TX timeout\\n"); return -EIO; }
    adapter->net_stats.tx_packets++;
    adapter->net_stats.tx_bytes += len;
    return 0;
}''')

reg('e1000.c', 'e1000_receive_packet', '''int e1000_receive_packet(struct net_device *dev, void *buf, int *len)
{
    if (!dev || !buf || !len) return -EINVAL;
    struct e1000_device *adapter = (struct e1000_device *)dev->priv;
    if (!adapter) return -ENODEV;
    uint32_t next = adapter->rx_cur;
    struct e1000_rx_desc *desc = &adapter->rx_ring[next];
    if (!(desc->status & E1000_RXD_STAT_DD)) return -EAGAIN;
    *len = __builtin_bswap16(desc->length);
    __builtin_memcpy(buf, adapter->rx_bufs[next], *len);
    desc->status = 0;
    adapter->rx_cur = (next + 1) % adapter->rx_ring_size;
    e1000_write_reg(adapter, REG_RDT(adapter->rx_q_idx), adapter->rx_cur);
    adapter->net_stats.rx_packets++;
    adapter->net_stats.rx_bytes += *len;
    return 0;
}''')

reg('e1000.c', 'e1000_irq_handler', '''void e1000_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    struct e1000_device *adapter = g_e1000_adapter;
    if (!adapter) return;
    uint32_t icr = e1000_read_reg(adapter, REG_ICR);
    if (icr & E1000_ICR_RXO) adapter->net_stats.rx_over_errors++;
    if (icr & E1000_ICR_RXT0) e1000_receive_packet_task(adapter);
    if (icr & E1000_ICR_TXDW) e1000_tx_cleanup(adapter);
    if (icr & E1000_ICR_LSC) e1000_link_status(adapter->net_dev);
}''')

reg('e1000.c', 'e1000_reset', '''void e1000_reset(struct e1000_device *adapter)
{
    if (!adapter) return;
    kprintf("[e1000] Resetting controller\\n");
    uint32_t ctrl = e1000_read_reg(adapter, REG_CTRL);
    ctrl |= CTRL_RST;
    e1000_write_reg(adapter, REG_CTRL, ctrl);
    udelay(1000);
    kprintf("[e1000] Reset complete\\n");
}''')

reg('e1000.c', 'e1000_link_status', '''int e1000_link_status(struct net_device *dev)
{
    if (!dev) return -EINVAL;
    struct e1000_device *adapter = (struct e1000_device *)dev->priv;
    if (!adapter) return -ENODEV;
    uint32_t status = e1000_read_reg(adapter, REG_STATUS);
    if (status & E1000_STATUS_LU) { adapter->link_up = 1; return 1; }
    adapter->link_up = 0;
    return 0;
}''')

# ============ AHCI ============
reg('ahci.c', 'ahci_port_start', '''int ahci_port_start(struct ahci_hba *hba, int port)
{
    if (!hba || port < 0 || port > 31) return -EINVAL;
    volatile uint32_t *port_base = (volatile uint32_t *)((uintptr_t)hba->mmio + HBA_PORT_BASE + port * HBA_PORT_SIZE);
    uint32_t cmd = port_base[PORT_CMD / 4];
    if (cmd & (PORT_CMD_CR | PORT_CMD_FR)) ahci_port_stop(hba, port);
    void *cl = pmm_alloc_aligned(1024, 1024);
    if (!cl) return -ENOMEM;
    __builtin_memset(cl, 0, 1024);
    void *fis = pmm_alloc_aligned(256, 256);
    if (!fis) { pmm_free(cl, 1024); return -ENOMEM; }
    __builtin_memset(fis, 0, 256);
    port_base[PORT_CLB / 4]  = (uint32_t)(uintptr_t)cl;
    port_base[PORT_CLBU / 4] = 0;
    port_base[PORT_FB / 4]   = (uint32_t)(uintptr_t)fis;
    port_base[PORT_FBU / 4]  = 0;
    port_base[PORT_SERR / 4] = port_base[PORT_SERR / 4];
    port_base[PORT_IE / 4] = PORT_IS_D2H | PORT_IS_SDBS | PORT_IS_PCS | PORT_IS_ERR;
    cmd = port_base[PORT_CMD / 4];
    cmd |= PORT_CMD_FRE;
    port_base[PORT_CMD / 4] = cmd;
    cmd |= PORT_CMD_ST;
    port_base[PORT_CMD / 4] = cmd;
    hba->ports[port].present = 1;
    kprintf("[ahci] Port %d started\\n", port);
    return 0;
}''')

reg('ahci.c', 'ahci_port_stop', '''int ahci_port_stop(struct ahci_hba *hba, int port)
{
    if (!hba || port < 0 || port > 31) return -EINVAL;
    volatile uint32_t *port_base = (volatile uint32_t *)((uintptr_t)hba->mmio + HBA_PORT_BASE + port * HBA_PORT_SIZE);
    uint32_t cmd = port_base[PORT_CMD / 4];
    cmd &= ~PORT_CMD_ST;
    port_base[PORT_CMD / 4] = cmd;
    int timeout = 1000000;
    while ((port_base[PORT_CMD / 4] & PORT_CMD_CR) && --timeout > 0) io_wait();
    cmd = port_base[PORT_CMD / 4];
    cmd &= ~PORT_CMD_FRE;
    port_base[PORT_CMD / 4] = cmd;
    timeout = 1000000;
    while ((port_base[PORT_CMD / 4] & PORT_CMD_FR) && --timeout > 0) io_wait();
    hba->ports[port].present = 0;
    return 0;
}''')

reg('ahci.c', 'ahci_read', '''int ahci_read(struct ahci_device *dev, uint64_t sector, void *buf, int count)
{
    if (!dev || !buf || count <= 0) return -EINVAL;
    struct ahci_port *port = dev->port;
    int slot = -1;
    for (int i = 0; i < 32; i++) { if (!(port->ci_mask & (1 << i))) { slot = i; break; } }
    if (slot < 0) return -EAGAIN;
    struct ahci_cmd_header *cmd_hdr = &port->cl[slot];
    struct ahci_cmd_table *cmd_tbl = (struct ahci_cmd_table *)(uintptr_t)cmd_hdr->ctba;
    __builtin_memset(cmd_tbl, 0, sizeof(struct ahci_cmd_table));
    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)cmd_tbl->cfis;
    fis->fis_type = 0x27;
    fis->command = 0x25;
    fis->lba_lo   = (uint8_t)(sector & 0xFF);
    fis->lba_mid  = (uint8_t)((sector >> 8) & 0xFF);
    fis->lba_hi   = (uint8_t)((sector >> 16) & 0xFF);
    fis->lba_lo_ex = (uint8_t)((sector >> 24) & 0xFF);
    fis->lba_mid_ex = (uint8_t)((sector >> 32) & 0xFF);
    fis->lba_hi_ex = (uint8_t)((sector >> 40) & 0xFF);
    fis->device   = 0x40;
    fis->count_lo = (uint8_t)(count & 0xFF);
    fis->count_hi = (uint8_t)((count >> 8) & 0xFF);
    cmd_tbl->prdt[0].dba = (uint64_t)(uintptr_t)buf;
    cmd_tbl->prdt[0].dbc = (count * 512 - 1) | (1u << 31);
    cmd_hdr->prdtl = 1;
    cmd_hdr->c = 0; cmd_hdr->w = 0;
    port->ci_mask |= (1 << slot);
    port->regs->ci = (1 << slot);
    int timeout = 10000000;
    while ((port->regs->ci & (1 << slot)) && --timeout > 0) io_wait();
    if (timeout == 0) { port->ci_mask &= ~(1 << slot); return -EIO; }
    port->ci_mask &= ~(1 << slot);
    return 0;
}''')

reg('ahci.c', 'ahci_write', '''int ahci_write(struct ahci_device *dev, uint64_t sector, const void *buf, int count)
{
    if (!dev || !buf || count <= 0) return -EINVAL;
    struct ahci_port *port = dev->port;
    int slot = -1;
    for (int i = 0; i < 32; i++) { if (!(port->ci_mask & (1 << i))) { slot = i; break; } }
    if (slot < 0) return -EAGAIN;
    struct ahci_cmd_header *cmd_hdr = &port->cl[slot];
    struct ahci_cmd_table *cmd_tbl = (struct ahci_cmd_table *)(uintptr_t)cmd_hdr->ctba;
    __builtin_memset(cmd_tbl, 0, sizeof(struct ahci_cmd_table));
    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)cmd_tbl->cfis;
    fis->fis_type = 0x27;
    fis->command = 0x35;
    fis->lba_lo   = (uint8_t)(sector & 0xFF);
    fis->lba_mid  = (uint8_t)((sector >> 8) & 0xFF);
    fis->lba_hi   = (uint8_t)((sector >> 16) & 0xFF);
    fis->lba_lo_ex = (uint8_t)((sector >> 24) & 0xFF);
    fis->lba_mid_ex = (uint8_t)((sector >> 32) & 0xFF);
    fis->lba_hi_ex = (uint8_t)((sector >> 40) & 0xFF);
    fis->device   = 0x40;
    fis->count_lo = (uint8_t)(count & 0xFF);
    fis->count_hi = (uint8_t)((count >> 8) & 0xFF);
    cmd_tbl->prdt[0].dba = (uint64_t)(uintptr_t)buf;
    cmd_tbl->prdt[0].dbc = (count * 512 - 1) | (1u << 31);
    cmd_hdr->prdtl = 1;
    cmd_hdr->c = 0; cmd_hdr->w = 1;
    port->ci_mask |= (1 << slot);
    port->regs->ci = (1 << slot);
    int timeout = 10000000;
    while ((port->regs->ci & (1 << slot)) && --timeout > 0) io_wait();
    if (timeout == 0) { port->ci_mask &= ~(1 << slot); return -EIO; }
    port->ci_mask &= ~(1 << slot);
    return 0;
}''')

reg('ahci.c', 'ahci_irq_handler', '''void ahci_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    struct ahci_hba *hba = g_ahci_hba;
    if (!hba) return;
    uint32_t is = hba->mmio[HBA_IS_OFFSET / 4];
    if (is == 0) return;
    uint32_t pi = hba->pi;
    for (int port = 0; port < 32; port++) {
        if (!(pi & (1 << port))) continue;
        if (!(is & (1 << port))) continue;
        volatile uint32_t *pr = (volatile uint32_t *)((uintptr_t)hba->mmio + HBA_PORT_BASE + port * HBA_PORT_SIZE);
        uint32_t port_is = pr[PORT_IS / 4];
        if (port_is & PORT_IS_D2H) ahci_handle_completion(hba, port);
        if (port_is & PORT_IS_SDBS) ahci_handle_ncq_completion(hba, port);
        if (port_is & PORT_IS_PCS) kprintf("[ahci] Port %d PHY changed\\n", port);
        if (port_is & PORT_IS_ERR) {
            uint32_t serr = pr[PORT_SERR / 4];
            kprintf("[ahci] Port %d error: SERR=0x%08x\\n", port, serr);
            pr[PORT_SERR / 4] = serr;
        }
        pr[PORT_IS / 4] = port_is;
    }
    hba->mmio[HBA_IS_OFFSET / 4] = is;
}''')

# ============ VIRTIO BLK ============
reg('virtio_blk.c', 'virtio_blk_read', '''int virtio_blk_read(struct block_device *dev, uint64_t sector, void *buf, int count)
{
    if (!dev || !buf || count <= 0) return -EINVAL;
    struct vblk_device *vdev = (struct vblk_device *)dev->priv;
    if (!vdev) return -ENODEV;
    if (sector + count > vdev->capacity) return -EIO;
    int qid = smp_id() % vdev->num_queues;
    struct vblk_queue *q = &vdev->queues[qid];
    int slot = -1;
    for (int i = 0; i < VRING_SIZE; i++) { if (!q->slot_busy[i]) { slot = i; break; } }
    if (slot < 0) return -EAGAIN;
    q->slot_busy[slot] = 1;
    q->req_hdrs[slot].type = VIRTIO_BLK_T_IN;
    q->req_hdrs[slot].sector = sector;
    int idx = q->avail->idx & (VRING_SIZE - 1);
    struct vring_desc *d = &q->descs[idx];
    d->addr = (uint64_t)(uintptr_t)&q->req_hdrs[slot];
    d->len = sizeof(struct virtio_blk_req_hdr);
    d->flags = VRING_DESC_F_NEXT;
    d->next = (idx + 1) & (VRING_SIZE - 1);
    struct vring_desc *dd = &q->descs[d->next];
    dd->addr = (uint64_t)(uintptr_t)buf;
    dd->len = count * 512;
    dd->flags = VRING_DESC_F_NEXT | VRING_DESC_F_WRITE;
    dd->next = (d->next + 1) & (VRING_SIZE - 1);
    struct vring_desc *ds = &q->descs[dd->next];
    ds->addr = (uint64_t)(uintptr_t)&q->status[slot];
    ds->len = 1;
    ds->flags = VRING_DESC_F_WRITE;
    q->avail->ring[q->avail->idx & (VRING_SIZE - 1)] = idx;
    wmb();
    q->avail->idx++;
    virtio_notify(vdev->virtio_dev, qid);
    while (q->status[slot] == 0xFF) io_wait();
    q->slot_busy[slot] = 0;
    return q->status[slot] == 0 ? 0 : -EIO;
}''')

reg('virtio_blk.c', 'virtio_blk_write', '''int virtio_blk_write(struct block_device *dev, uint64_t sector, const void *buf, int count)
{
    if (!dev || !buf || count <= 0) return -EINVAL;
    struct vblk_device *vdev = (struct vblk_device *)dev->priv;
    if (!vdev) return -ENODEV;
    if (sector + count > vdev->capacity) return -EIO;
    int qid = smp_id() % vdev->num_queues;
    struct vblk_queue *q = &vdev->queues[qid];
    int slot = -1;
    for (int i = 0; i < VRING_SIZE; i++) { if (!q->slot_busy[i]) { slot = i; break; } }
    if (slot < 0) return -EAGAIN;
    q->slot_busy[slot] = 1;
    q->req_hdrs[slot].type = VIRTIO_BLK_T_OUT;
    q->req_hdrs[slot].sector = sector;
    int idx = q->avail->idx & (VRING_SIZE - 1);
    struct vring_desc *d = &q->descs[idx];
    d->addr = (uint64_t)(uintptr_t)&q->req_hdrs[slot];
    d->len = sizeof(struct virtio_blk_req_hdr);
    d->flags = VRING_DESC_F_NEXT;
    d->next = (idx + 1) & (VRING_SIZE - 1);
    struct vring_desc *dd = &q->descs[d->next];
    dd->addr = (uint64_t)(uintptr_t)buf;
    dd->len = count * 512;
    dd->flags = VRING_DESC_F_NEXT;
    dd->next = (d->next + 1) & (VRING_SIZE - 1);
    struct vring_desc *ds = &q->descs[dd->next];
    ds->addr = (uint64_t)(uintptr_t)&q->status[slot];
    ds->len = 1;
    ds->flags = VRING_DESC_F_WRITE;
    q->avail->ring[q->avail->idx & (VRING_SIZE - 1)] = idx;
    wmb();
    q->avail->idx++;
    virtio_notify(vdev->virtio_dev, qid);
    while (q->status[slot] == 0xFF) io_wait();
    q->slot_busy[slot] = 0;
    return q->status[slot] == 0 ? 0 : -EIO;
}''')

reg('virtio_blk.c', 'virtio_blk_init', '''int virtio_blk_init(struct pci_device *pci_dev)
{
    if (!pci_dev) return -EINVAL;
    kprintf("[virtio-blk] Initializing\\n");
    if (pci_enable_device(pci_dev) < 0) return -ENODEV;
    pci_set_master(pci_dev);
    struct vblk_device *vdev = pmm_alloc(sizeof(struct vblk_device));
    if (!vdev) return -ENOMEM;
    __builtin_memset(vdev, 0, sizeof(struct vblk_device));
    if (virtio_init(pci_dev, &vdev->virtio_dev) < 0) { pmm_free(vdev, sizeof(struct vblk_device)); return -ENODEV; }
    uint32_t features = virtio_get_features(vdev->virtio_dev);
    vdev->num_queues = (features & VIRTIO_BLK_F_MQ) ? ((features >> 28) & 0x7F) : 1;
    if (vdev->num_queues > VBLK_MAX_QUEUES) vdev->num_queues = VBLK_MAX_QUEUES;
    virtio_set_features(vdev->virtio_dev, features & VBLK_SUPPORTED_FEATURES);
    for (int i = 0; i < vdev->num_queues; i++) {
        if (virtio_find_vq(vdev->virtio_dev, i, &vdev->queues[i].vq) < 0) return -ENODEV;
        vdev->queues[i].queue_idx = i;
        vdev->queues[i].initialized = 1;
    }
    blockdev_register(BLOCKDEV_VIRTIO, "virtio-blk", vdev, &vblk_ops);
    kprintf("[virtio-blk] Initialized: %d queues\\n", vdev->num_queues);
    return 0;
}''')

# ============ NVME ============
reg('nvme.c', 'nvme_submit_sq', '''int nvme_submit_sq(struct nvme_queue *sq, struct nvme_command *cmd)
{
    if (!sq || !cmd) return -EINVAL;
    uint16_t tail = sq->sq_tail;
    __builtin_memcpy(&sq->sq_base[tail], cmd, sizeof(struct nvme_command));
    wmb();
    sq->sq_tail = (tail + 1) % sq->q_depth;
    writel(tail, (volatile void *)(uintptr_t)sq->doorbell);
    return 0;
}''')

reg('nvme.c', 'nvme_process_cq', '''int nvme_process_cq(struct nvme_queue *cq)
{
    if (!cq) return -EINVAL;
    uint16_t head = cq->cq_head;
    int processed = 0;
    while (cq->cq_base[head].status & 0x1) {
        struct nvme_completion *entry = &cq->cq_base[head];
        if (entry->command_id < cq->max_cid) {
            cq->completions[entry->command_id] = *entry;
            cq->completed[entry->command_id] = 1;
        }
        entry->status &= ~0x1;
        head = (head + 1) % cq->q_depth;
        processed++;
    }
    cq->cq_head = head;
    writel(head, (volatile void *)(uintptr_t)(cq->doorbell + 4));
    return processed;
}''')

reg('nvme.c', 'nvme_identify', '''int nvme_identify(struct nvme_device *dev, uint32_t nsid, void *buf)
{
    if (!dev || !buf) return -EINVAL;
    struct nvme_command cmd;
    __builtin_memset(&cmd, 0, sizeof(cmd));
    cmd.dword0 = (6 << 16) | 0x06;
    cmd.nsid = nsid;
    cmd.mptr = (uint64_t)(uintptr_t)buf;
    cmd.prp1 = (uint64_t)(uintptr_t)buf;
    if (nvme_submit_sq(&dev->admin_sq, &cmd) < 0) return -EIO;
    int timeout = 10000000;
    while (!dev->admin_cq.completed[6] && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;
    dev->admin_cq.completed[6] = 0;
    nvme_process_cq(&dev->admin_cq);
    return 0;
}''')

reg('nvme.c', 'nvme_read', '''int nvme_read(struct nvme_device *dev, struct nvme_queue *io_sq, uint64_t slba, uint16_t nblocks, void *buf)
{
    if (!dev || !io_sq || !buf) return -EINVAL;
    struct nvme_command cmd;
    __builtin_memset(&cmd, 0, sizeof(cmd));
    cmd.dword0 = (1 << 16) | 0x02;
    cmd.nsid = dev->nsid;
    cmd.dword10 = (uint32_t)(slba & 0xFFFFFFFF);
    cmd.dword11 = (uint32_t)((slba >> 32) & 0xFFFFFFFF);
    cmd.dword12 = (nblocks - 1) | (2 << 20);
    cmd.mptr = (uint64_t)(uintptr_t)buf;
    cmd.prp1 = (uint64_t)(uintptr_t)buf;
    if (nvme_submit_sq(io_sq, &cmd) < 0) return -EIO;
    int timeout = 10000000;
    while (!io_sq->completed[1] && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;
    io_sq->completed[1] = 0;
    nvme_process_cq(io_sq);
    return 0;
}''')

reg('nvme.c', 'nvme_write', '''int nvme_write(struct nvme_device *dev, struct nvme_queue *io_sq, uint64_t slba, uint16_t nblocks, const void *buf)
{
    if (!dev || !io_sq || !buf) return -EINVAL;
    struct nvme_command cmd;
    __builtin_memset(&cmd, 0, sizeof(cmd));
    cmd.dword0 = (2 << 16) | (1 << 7) | 0x01;
    cmd.nsid = dev->nsid;
    cmd.dword10 = (uint32_t)(slba & 0xFFFFFFFF);
    cmd.dword11 = (uint32_t)((slba >> 32) & 0xFFFFFFFF);
    cmd.dword12 = (nblocks - 1) | (2 << 20);
    cmd.mptr = (uint64_t)(uintptr_t)buf;
    cmd.prp1 = (uint64_t)(uintptr_t)buf;
    if (nvme_submit_sq(io_sq, &cmd) < 0) return -EIO;
    int timeout = 10000000;
    while (!io_sq->completed[2] && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;
    io_sq->completed[2] = 0;
    nvme_process_cq(io_sq);
    return 0;
}''')

# ============ SERIAL ============
reg('serial.c', 'serial_write', '''int serial_write(struct serial_device *dev, const uint8_t *data, size_t len)
{
    if (!dev || !data) return -EINVAL;
    for (size_t i = 0; i < len; i++) {
        int timeout = 10000;
        while (!(inb(dev->port + 5) & 0x20) && --timeout > 0) io_wait();
        if (timeout == 0) return -EIO;
        outb(dev->port, data[i]);
    }
    return len;
}''')

reg('serial.c', 'serial_read', '''int serial_read(struct serial_device *dev, uint8_t *data, size_t len)
{
    if (!dev || !data) return -EINVAL;
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        int timeout = 10000;
        while (!(inb(dev->port + 5) & 0x01) && --timeout > 0) io_wait();
        if (timeout == 0) break;
        data[count++] = inb(dev->port);
    }
    return count;
}''')

reg('serial.c', 'serial_config_baud', '''int serial_config_baud(struct serial_device *dev, uint32_t baud)
{
    if (!dev || baud == 0) return -EINVAL;
    uint16_t divisor = 115200 / baud;
    if (divisor == 0) divisor = 1;
    outb(dev->port + 3, inb(dev->port + 3) | 0x80);
    outb(dev->port, divisor & 0xFF);
    outb(dev->port + 1, (divisor >> 8) & 0xFF);
    outb(dev->port + 3, inb(dev->port + 3) & ~0x80);
    dev->baud = baud;
    return 0;
}''')

reg('serial.c', 'serial_irq_handler', '''void serial_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    uint8_t iir = inb(SERIAL_PORT + 2);
    if (iir & 1) return;
    switch ((iir >> 1) & 3) {
    case 0: inb(SERIAL_PORT + 6); break;
    case 1: break;
    case 2: serial_handle_rx(); break;
    case 3: inb(SERIAL_PORT + 5); break;
    }
    send_eoi(4);
}''')

# ============ KEYBOARD ============
reg('keyboard.c', 'keyboard_read', '''int keyboard_read(uint8_t *scancode)
{
    if (!scancode) return -EINVAL;
    if (!(inb(0x64) & 0x01)) return -EAGAIN;
    *scancode = inb(0x60);
    return 1;
}''')

reg('keyboard.c', 'keyboard_irq_handler', '''void keyboard_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    uint8_t scancode = inb(0x60);
    keyboard_handle_scancode(scancode);
    send_eoi(1);
}''')

reg('keyboard.c', 'keyboard_set_leds', '''int keyboard_set_leds(uint8_t leds)
{
    int timeout = 10000;
    while ((inb(0x64) & 0x02) && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;
    outb(0x60, 0xED);
    timeout = 10000;
    while ((inb(0x64) & 0x02) && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;
    outb(0x60, leds & 0x07);
    return 0;
}''')

# ============ MOUSE ============
reg('mouse.c', 'mouse_read', '''int mouse_read(uint8_t *data)
{
    if (!data) return -EINVAL;
    if (!(inb(0x64) & 0x01)) return -EAGAIN;
    *data = inb(0x60);
    return 1;
}''')

reg('mouse.c', 'mouse_irq_handler', '''void mouse_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    uint8_t data = inb(0x60);
    mouse_process_packet(data);
    send_eoi(12);
}''')

# ============ PCI ============
reg('pci.c', 'pci_read_config', '''uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t addr = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000u);
    outl(0xCF8, addr);
    return inl(0xCFC);
}''')

reg('pci.c', 'pci_write_config', '''void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t addr = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000u);
    outl(0xCF8, addr);
    outl(0xCFC, value);
}''')

reg('pci.c', 'pci_find_device', '''int pci_find_device(uint16_t vendor_id, uint16_t device_id, int index)
{
    int found = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read_config(bus, slot, func, 0);
                if ((id & 0xFFFF) == vendor_id && ((id >> 16) & 0xFFFF) == device_id) {
                    if (found == index) return (bus << 16) | (slot << 11) | (func << 8);
                    found++;
                }
                if (func == 0 && !(pci_read_config(bus, slot, func, 0x0C) & 0x800000)) break;
            }
        }
    }
    return -ENODEV;
}''')

reg('pci.c', 'pci_enable_device', '''int pci_enable_device(struct pci_dev *dev)
{
    if (!dev) return -EINVAL;
    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x07;
    pci_write_config(dev->bus, dev->slot, dev->func, 0x04, cmd);
    return 0;
}''')

reg('pci.c', 'pci_request_irq', '''int pci_request_irq(struct pci_dev *dev)
{
    if (!dev) return -EINVAL;
    dev->irq = pci_read_config(dev->bus, dev->slot, dev->func, 0x3C) & 0xFF;
    return dev->irq;
}''')

reg('pci.c', 'pci_alloc_consistent', '''void *pci_alloc_consistent(struct pci_dev *dev, size_t size, dma_addr_t *dma_handle)
{
    (void)dev;
    void *ptr = pmm_alloc_aligned(size, 4096);
    if (!ptr) return NULL;
    __builtin_memset(ptr, 0, size);
    if (dma_handle) *dma_handle = (dma_addr_t)(uintptr_t)ptr;
    return ptr;
}''')

reg('pci.c', 'pci_free_consistent', '''void pci_free_consistent(struct pci_dev *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle)
{
    (void)dev; (void)dma_handle;
    pmm_free(cpu_addr, size);
}''')

reg('pci.c', 'pci_set_master', '''int pci_set_master(struct pci_dev *dev)
{
    if (!dev) return -EINVAL;
    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x04;
    pci_write_config(dev->bus, dev->slot, dev->func, 0x04, cmd);
    return 0;
}''')

reg('pci.c', 'pci_enable_msi', '''int pci_enable_msi(struct pci_dev *dev)
{
    if (!dev) return -EINVAL;
    uint8_t cap = (pci_read_config(dev->bus, dev->slot, dev->func, 0x34) & 0xFF);
    while (cap) {
        uint32_t cap_reg = pci_read_config(dev->bus, dev->slot, dev->func, cap);
        if ((cap_reg & 0xFF) == 0x05) {
            uint16_t msg_ctrl = (cap_reg >> 16) & 0xFFFF;
            msg_ctrl |= 0x0001;
            pci_write_config(dev->bus, dev->slot, dev->func, cap, (cap_reg & 0x0000FFFF) | (msg_ctrl << 16));
            return 0;
        }
        cap = (cap_reg >> 8) & 0xFF;
    }
    return -ENODEV;
}''')

reg('pci.c', 'pci_disable_msi', '''int pci_disable_msi(struct pci_dev *dev)
{
    if (!dev) return -EINVAL;
    uint8_t cap = (pci_read_config(dev->bus, dev->slot, dev->func, 0x34) & 0xFF);
    while (cap) {
        uint32_t cap_reg = pci_read_config(dev->bus, dev->slot, dev->func, cap);
        if ((cap_reg & 0xFF) == 0x05) {
            uint16_t msg_ctrl = (cap_reg >> 16) & 0xFFFF;
            msg_ctrl &= ~0x0001;
            pci_write_config(dev->bus, dev->slot, dev->func, cap, (cap_reg & 0x0000FFFF) | (msg_ctrl << 16));
            return 0;
        }
        cap = (cap_reg >> 8) & 0xFF;
    }
    return -ENODEV;
}''')

# ============ RTC ============
reg('rtc.c', 'rtc_read_time', '''int rtc_read_time(struct rtc_time *tm)
{
    if (!tm) return -EINVAL;
    int timeout = 10000;
    while ((inb(0x70) & 0x80) && --timeout > 0) io_wait();
    uint8_t second = rtc_read_reg(0x00);
    uint8_t minute = rtc_read_reg(0x02);
    uint8_t hour   = rtc_read_reg(0x04);
    uint8_t day    = rtc_read_reg(0x07);
    uint8_t month  = rtc_read_reg(0x08);
    uint8_t year   = rtc_read_reg(0x09);
    uint8_t century_byte = rtc_read_reg(0x32);
    uint8_t status_b = rtc_read_reg(0x0B);
    uint32_t century = century_byte ? century_byte : 20;
    if (!(status_b & 0x04)) {
        second = (second & 0x0F) + ((second / 16) * 10);
        minute = (minute & 0x0F) + ((minute / 16) * 10);
        hour   = (hour & 0x0F) + ((hour / 16) * 10);
        day    = (day & 0x0F) + ((day / 16) * 10);
        month  = (month & 0x0F) + ((month / 16) * 10);
        year   = (year & 0x0F) + ((year / 16) * 10);
    }
    tm->tm_sec  = second;
    tm->tm_min  = minute;
    tm->tm_hour = hour;
    tm->tm_mday = day;
    tm->tm_mon  = month - 1;
    tm->tm_year = century * 100 + year - 1900;
    tm->tm_wday = rtc_read_reg(0x06) - 1;
    tm->tm_yday = 0;
    tm->tm_isdst = 0;
    return 0;
}''')

reg('rtc.c', 'rtc_set_time', '''int rtc_set_time(const struct rtc_time *tm)
{
    if (!tm) return -EINVAL;
    outb(0x70, 0x0B);
    uint8_t prev = inb(0x71);
    outb(0x70, 0x0B);
    outb(0x71, prev | 0x80);
    rtc_write_reg(0x00, tm->tm_sec);
    rtc_write_reg(0x02, tm->tm_min);
    rtc_write_reg(0x04, tm->tm_hour);
    rtc_write_reg(0x06, tm->tm_wday + 1);
    rtc_write_reg(0x07, tm->tm_mday);
    rtc_write_reg(0x08, tm->tm_mon + 1);
    rtc_write_reg(0x09, (tm->tm_year + 1900) % 100);
    rtc_write_reg(0x32, (tm->tm_year + 1900) / 100);
    outb(0x70, 0x0B);
    prev = inb(0x71);
    outb(0x70, 0x0B);
    outb(0x71, prev & ~0x80);
    outb(0x70, 0x00);
    return 0;
}''')

reg('rtc.c', 'rtc_read_alarm', '''int rtc_read_alarm(struct rtc_time *alarm)
{
    if (!alarm) return -EINVAL;
    int timeout = 10000;
    while ((inb(0x70) & 0x80) && --timeout > 0) io_wait();
    alarm->tm_sec  = rtc_read_reg(0x01);
    alarm->tm_min  = rtc_read_reg(0x03);
    alarm->tm_hour = rtc_read_reg(0x05);
    uint8_t status_b = rtc_read_reg(0x0B);
    if (!(status_b & 0x20)) return -EINVAL;
    return 0;
}''')

reg('rtc.c', 'rtc_set_alarm', '''int rtc_set_alarm(const struct rtc_time *alarm)
{
    if (!alarm) return -EINVAL;
    outb(0x70, 0x0B);
    uint8_t prev = inb(0x71);
    outb(0x70, 0x0B);
    outb(0x71, prev | 0x80);
    rtc_write_reg(0x01, alarm->tm_sec);
    rtc_write_reg(0x03, alarm->tm_min);
    rtc_write_reg(0x05, alarm->tm_hour);
    outb(0x70, 0x0B);
    prev = inb(0x71);
    outb(0x70, 0x0B);
    outb(0x71, prev | 0x20);
    outb(0x70, 0x0B);
    prev = inb(0x71);
    outb(0x70, 0x0B);
    outb(0x71, prev & ~0x80);
    outb(0x70, 0x00);
    return 0;
}''')

# ============ HPET ============
reg('hpet.c', 'hpet_init', '''int hpet_init(void)
{
    uintptr_t hpet_base = acpi_find_hpet();
    if (!hpet_base) { kprintf("[hpet] HPET not found\\n"); return -ENODEV; }
    g_hpet_mmio = (volatile uint32_t *)hpet_base;
    uint32_t cap = g_hpet_mmio[0];
    g_hpet_period = (cap >> 32) & 0xFFFFFFFF;
    uint64_t config = 1;
    g_hpet_mmio[2] = (uint32_t)(config & 0xFFFFFFFF);
    g_hpet_mmio[3] = (uint32_t)(config >> 32);
    kprintf("[hpet] HPET initialized, period=%u fs\\n", g_hpet_period);
    return 0;
}''')

reg('hpet.c', 'hpet_read_counter', '''uint64_t hpet_read_counter(void)
{
    if (!g_hpet_mmio) return 0;
    uint32_t lo = g_hpet_mmio[0];
    uint32_t hi = g_hpet_mmio[1];
    return ((uint64_t)hi << 32) | lo;
}''')

reg('hpet.c', 'hpet_set_freq', '''int hpet_set_freq(uint32_t freq_hz)
{
    if (!g_hpet_mmio || freq_hz == 0) return -EINVAL;
    uint64_t period_fs = g_hpet_period;
    uint64_t counter_val = 1000000000000000ULL / (freq_hz * period_fs);
    g_hpet_mmio[8] = (uint32_t)(counter_val & 0xFFFFFFFF);
    g_hpet_mmio[9] = (uint32_t)(counter_val >> 32);
    g_hpet_mmio[4] = 0x004C;
    return 0;
}''')

# ============ I2C ============
reg('i2c.c', 'i2c_read', '''int i2c_read(struct i2c_device *dev, uint8_t addr, uint8_t reg, uint8_t *buf, size_t count)
{
    if (!dev || !buf || count == 0) return -EINVAL;
    i2c_send_start(dev);
    i2c_send_byte(dev, addr << 1);
    i2c_send_byte(dev, reg);
    i2c_send_start(dev);
    i2c_send_byte(dev, (addr << 1) | 1);
    for (size_t i = 0; i < count; i++)
        buf[i] = i2c_recv_byte(dev, i < count - 1);
    i2c_send_stop(dev);
    return count;
}''')

reg('i2c.c', 'i2c_write', '''int i2c_write(struct i2c_device *dev, uint8_t addr, uint8_t reg, const uint8_t *buf, size_t count)
{
    if (!dev) return -EINVAL;
    i2c_send_start(dev);
    i2c_send_byte(dev, addr << 1);
    i2c_send_byte(dev, reg);
    for (size_t i = 0; i < count; i++)
        i2c_send_byte(dev, buf[i]);
    i2c_send_stop(dev);
    return count;
}''')

reg('i2c.c', 'i2c_transfer', '''int i2c_transfer(struct i2c_device *dev, struct i2c_msg *msgs, int num)
{
    if (!dev || !msgs || num <= 0) return -EINVAL;
    for (int i = 0; i < num; i++) {
        struct i2c_msg *msg = &msgs[i];
        if (msg->flags & I2C_M_RD) {
            int ret = i2c_read(dev, msg->addr, 0, msg->buf, msg->len);
            if (ret < 0) return ret;
        } else {
            int ret = i2c_write(dev, msg->addr, 0, msg->buf, msg->len);
            if (ret < 0) return ret;
        }
    }
    return num;
}''')

# ============ WATCHDOG ============
reg('watchdog.c', 'watchdog_start', '''int watchdog_start(void)
{
    kprintf("[watchdog] Starting\\n");
    outb(I6300ESB_BASE + 0x05, 0x30);
    outb(I6300ESB_BASE + 0x05, 0x30);
    return 0;
}''')

reg('watchdog.c', 'watchdog_stop', '''int watchdog_stop(void)
{
    kprintf("[watchdog] Stopping\\n");
    outb(I6300ESB_BASE + 0x05, 0x00);
    return 0;
}''')

reg('watchdog.c', 'watchdog_pet', '''int watchdog_pet(void)
{
    outw(I6300ESB_BASE + 0x06, 0xFFFF);
    return 0;
}''')

reg('watchdog.c', 'watchdog_set_timeout', '''int watchdog_set_timeout(uint32_t seconds)
{
    if (seconds == 0 || seconds > 255) return -EINVAL;
    outb(I6300ESB_BASE + 0x07, (uint8_t)seconds);
    return 0;
}''')

# ============ SPI ============
reg('spi.c', 'spi_transfer', '''int spi_transfer(struct spi_device *dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!dev || !tx || !rx || len == 0) return -EINVAL;
    for (size_t i = 0; i < len; i++) {
        int timeout = 10000;
        while (!(inb(dev->base + 1) & 0x02) && --timeout > 0) io_wait();
        if (timeout == 0) return -EIO;
        outb(dev->base + 0, tx[i]);
        timeout = 10000;
        while (!(inb(dev->base + 1) & 0x01) && --timeout > 0) io_wait();
        if (timeout == 0) return -EIO;
        rx[i] = inb(dev->base + 0);
    }
    return len;
}''')

reg('spi.c', 'spi_write_then_read', '''int spi_write_then_read(struct spi_device *dev, const uint8_t *tx, size_t n_tx, uint8_t *rx, size_t n_rx)
{
    if (!dev || !tx || !rx || n_tx == 0 || n_rx == 0) return -EINVAL;
    for (size_t i = 0; i < n_tx; i++) {
        int timeout = 10000;
        while (!(inb(dev->base + 1) & 0x02) && --timeout > 0) io_wait();
        if (timeout == 0) return -EIO;
        outb(dev->base + 0, tx[i]);
    }
    for (size_t i = 0; i < n_rx; i++) {
        int timeout = 10000;
        while (!(inb(dev->base + 1) & 0x02) && --timeout > 0) io_wait();
        if (timeout == 0) return -EIO;
        outb(dev->base + 0, 0xFF);
        timeout = 10000;
        while (!(inb(dev->base + 1) & 0x01) && --timeout > 0) io_wait();
        if (timeout == 0) return -EIO;
        rx[i] = inb(dev->base + 0);
    }
    return n_tx + n_rx;
}''')

# ============ VGA ============
reg('vga.c', 'vga_write_char', '''void vga_write_char(uint16_t pos, uint8_t ch, uint8_t attr)
{
    volatile uint16_t *vga_mem = (volatile uint16_t *)0xB8000;
    if (pos >= 80 * 25) return;
    vga_mem[pos] = (uint16_t)(attr << 8) | ch;
}''')

reg('vga.c', 'vga_scroll', '''void vga_scroll(void)
{
    volatile uint16_t *vga_mem = (volatile uint16_t *)0xB8000;
    for (int i = 0; i < 24 * 80; i++)
        vga_mem[i] = vga_mem[i + 80];
    for (int i = 24 * 80; i < 25 * 80; i++)
        vga_mem[i] = (uint16_t)(0x0F00) | ' ';
}''')

reg('vga.c', 'vga_set_cursor', '''void vga_set_cursor(uint16_t pos)
{
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}''')

reg('vga.c', 'vga_get_cursor', '''uint16_t vga_get_cursor(void)
{
    outb(0x3D4, 0x0F);
    uint16_t pos = inb(0x3D5);
    outb(0x3D4, 0x0E);
    pos |= (inb(0x3D5) << 8);
    return pos;
}''')

# ============ I8253 ============
reg('i8253.c', 'i8253_set_freq', '''void i8253_set_freq(uint32_t freq_hz)
{
    if (freq_hz == 0) return;
    uint32_t divisor = 1193180 / freq_hz;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}''')

reg('i8253.c', 'i8253_read_count', '''uint16_t i8253_read_count(void)
{
    outb(0x43, 0x00);
    uint16_t lo = inb(0x40);
    uint16_t hi = inb(0x40);
    return (hi << 8) | lo;
}''')

# ============ VESA ============
reg('vesa.c', 'vesa_set_mode', '''int vesa_set_mode(uint16_t mode)
{
    kprintf("[vesa] Setting VESA mode 0x%x\\n", mode);
    __asm__ volatile("int $0x10" : : "a"(0x4F02), "b"(mode) : "memory");
    g_vesa_mode = mode;
    return 0;
}''')

reg('vesa.c', 'vesa_get_mode', '''uint16_t vesa_get_mode(void)
{
    return g_vesa_mode;
}''')

reg('vesa.c', 'vesa_put_pixel', '''void vesa_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= g_vesa_width || y >= g_vesa_height) return;
    volatile uint8_t *fb = (volatile uint8_t *)(uintptr_t)g_vesa_fb_addr;
    uint32_t offset = y * g_vesa_pitch + x * (g_vesa_bpp / 8);
    *(uint32_t *)(fb + offset) = color;
}''')

reg('vesa.c', 'vesa_fill_rect', '''void vesa_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t row = 0; row < h; row++)
        for (uint32_t col = 0; col < w; col++)
            vesa_put_pixel(x + col, y + row, color);
}''')

reg('vesa.c', 'vesa_scroll', '''void vesa_scroll(int lines)
{
    if (lines <= 0) return;
    uint32_t copy_bytes = (g_vesa_height - lines) * g_vesa_pitch;
    volatile uint8_t *fb = (volatile uint8_t *)(uintptr_t)g_vesa_fb_addr;
    __builtin_memmove((void *)fb, (void *)(fb + lines * g_vesa_pitch), copy_bytes);
    __builtin_memset((void *)(fb + copy_bytes), 0, lines * g_vesa_pitch);
}''')

# ============ FB ============
reg('simplefb.c', 'fb_set_par', '''int fb_set_par(struct fb_info *info)
{
    if (!info) return -EINVAL;
    kprintf("[fb] Set par: %dx%d, %d bpp\\n", info->var.xres, info->var.yres, info->var.bits_per_pixel);
    info->screen_base = (uint8_t *)(uintptr_t)info->fix.smem_start;
    info->fix.line_length = info->var.xres * info->var.bits_per_pixel / 8;
    return 0;
}''')
reg('simplefb.c', 'fb_ioctl', '''int fb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
    if (!info) return -EINVAL;
    switch (cmd) {
    case FBIOGET_VSCREENINFO: __builtin_memcpy((void *)arg, &info->var, sizeof(info->var)); return 0;
    case FBIOPUT_VSCREENINFO: __builtin_memcpy(&info->var, (void *)arg, sizeof(info->var)); return fb_set_par(info);
    case FBIOGET_FSCREENINFO: __builtin_memcpy((void *)arg, &info->fix, sizeof(info->fix)); return 0;
    case FBIOPAN_DISPLAY: return 0;
    default: return -ENOTTY;
    }
}''')
reg('simplefb.c', 'fb_mmap', '''int fb_mmap(struct fb_info *info, struct vm_area *vma)
{
    if (!info || !vma) return -EINVAL;
    return vmm_map_phys_range(vma->vm_mm, vma->vm_start, info->fix.smem_start, info->fix.smem_len, VMM_RW | VMM_UNCACHED);
}''')
reg('simplefb.c', 'fb_blank', '''int fb_blank(struct fb_info *info, int blank)
{
    if (!info) return -EINVAL;
    switch (blank) {
    case FB_BLANK_UNBLANK: outw(0x3C0, 0x20); break;
    default: outw(0x3C0, 0x00); break;
    }
    info->blank_state = blank;
    return 0;
}''')

# ============ LOOP ============
reg('loop.c', 'loop_read', '''ssize_t loop_read(struct loop_device *dev, void *buf, size_t count, loff_t pos)
{
    if (!dev || !buf) return -EINVAL;
    if (!dev->backing_file) return -ENXIO;
    return file_read(dev->backing_file, buf, count, &pos);
}''')
reg('loop.c', 'loop_write', '''ssize_t loop_write(struct loop_device *dev, const void *buf, size_t count, loff_t pos)
{
    if (!dev || !buf) return -EINVAL;
    if (!dev->backing_file) return -ENXIO;
    return file_write(dev->backing_file, buf, count, &pos);
}''')
reg('loop.c', 'loop_set_fd', '''int loop_set_fd(struct loop_device *dev, struct file *file)
{
    if (!dev || !file) return -EINVAL;
    if (dev->backing_file) return -EBUSY;
    dev->backing_file = file;
    dev->size = file_size(file);
    return 0;
}''')
reg('loop.c', 'loop_clr_fd', '''int loop_clr_fd(struct loop_device *dev)
{
    if (!dev) return -EINVAL;
    if (!dev->backing_file) return -ENXIO;
    dev->backing_file = NULL;
    dev->size = 0;
    return 0;
}''')

# ============ BRD ============
reg('ramdisk.c', 'brd_read', '''int brd_read(struct brd_device *dev, sector_t sector, void *buf, int count)
{
    if (!dev || !buf) return -EINVAL;
    for (int i = 0; i < count; i++) {
        uintptr_t page = (uintptr_t)dev->pages[sector + i];
        if (page) __builtin_memcpy((uint8_t *)buf + i * 512, (void *)page, 512);
        else __builtin_memset((uint8_t *)buf + i * 512, 0, 512);
    }
    return 0;
}''')
reg('ramdisk.c', 'brd_write', '''int brd_write(struct brd_device *dev, sector_t sector, const void *buf, int count)
{
    if (!dev || !buf) return -EINVAL;
    for (int i = 0; i < count; i++) {
        uintptr_t page = (uintptr_t)dev->pages[sector + i];
        if (!page) {
            page = (uintptr_t)pmm_alloc(512);
            if (!page) return -ENOMEM;
            dev->pages[sector + i] = (void *)page;
        }
        __builtin_memcpy((void *)page, (const uint8_t *)buf + i * 512, 512);
    }
    return 0;
}''')
reg('ramdisk.c', 'brd_init', '''int brd_init(struct brd_device *dev, uint64_t size)
{
    if (!dev || size == 0) return -EINVAL;
    dev->size = size;
    dev->num_pages = (size + 511) / 512;
    dev->pages = pmm_alloc(dev->num_pages * sizeof(void *));
    if (!dev->pages) return -ENOMEM;
    __builtin_memset(dev->pages, 0, dev->num_pages * sizeof(void *));
    return 0;
}''')

# ============ SB16 ============
reg('sound_core.c', 'sb16_init', '''int sb16_init(struct sb16_device *dev)
{
    if (!dev) return -EINVAL;
    kprintf("[sb16] Initializing\\n");
    outb(dev->base + 0x06, 1); udelay(10); outb(dev->base + 0x06, 0);
    int timeout = 10000;
    while (!(inb(dev->base + 0x0E) & 0x80) && --timeout > 0) io_wait();
    if (timeout == 0) return -ENODEV;
    outb(dev->base + 0x0C, 0xE1);
    dev->dsp_major = inb(dev->base + 0x0A);
    dev->dsp_minor = inb(dev->base + 0x0A);
    return 0;
}''')
reg('sound_core.c', 'sb16_write', '''int sb16_write(struct sb16_device *dev, const uint8_t *data, size_t len)
{
    if (!dev || !data) return -EINVAL;
    for (size_t i = 0; i < len; i++) {
        int timeout = 10000;
        while ((inb(dev->base + 0x0C) & 0x80) && --timeout > 0) io_wait();
        if (timeout == 0) return -EIO;
        outb(dev->base + 0x0C, data[i]);
    }
    return len;
}''')
reg('sound_core.c', 'sb16_set_rate', '''int sb16_set_rate(struct sb16_device *dev, uint32_t rate)
{
    if (!dev || rate == 0) return -EINVAL;
    if (rate > 48000) rate = 48000;
    if (rate < 5000) rate = 5000;
    uint8_t tc = 256 - (1000000 / rate);
    outb(dev->base + 0x0C, 0x40);
    outb(dev->base + 0x0C, tc);
    dev->sample_rate = rate;
    return 0;
}''')

# ============ AC97 ============
reg('ac97.c', 'ac97_init', '''int ac97_init(struct ac97_device *dev)
{
    if (!dev) return -EINVAL;
    kprintf("[ac97] Initializing\\n");
    outw(dev->base + 0x00, 0x0000); udelay(100);
    dev->codec_id = inw(dev->base + 0x7C);
    outw(dev->base + 0x02, 0x0202);
    outw(dev->base + 0x18, 0x0404);
    dev->initialized = 1;
    return 0;
}''')
reg('ac97.c', 'ac97_write', '''int ac97_write(struct ac97_device *dev, uint32_t reg, uint16_t val)
{
    if (!dev) return -EINVAL;
    int timeout = 10000;
    while ((inb(dev->base + 0x04) & 0x01) && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;
    outw(dev->base + 0x02, val);
    outb(dev->base + 0x00, reg);
    return 0;
}''')
reg('ac97.c', 'ac97_set_rate', '''int ac97_set_rate(struct ac97_device *dev, uint32_t rate)
{
    if (!dev || rate == 0) return -EINVAL;
    if (rate > 48000) rate = 48000;
    if (rate < 8000) rate = 8000;
    ac97_write(dev, 0x2C, rate);
    dev->sample_rate = rate;
    return 0;
}''')

# ============ PCSPKR ============
reg('speaker.c', 'pcspkr_beep', '''void pcspkr_beep(uint32_t freq_hz)
{
    if (freq_hz == 0) freq_hz = 1000;
    uint32_t divisor = 1193180 / freq_hz;
    outb(0x43, 0xB6);
    outb(0x42, divisor & 0xFF);
    outb(0x42, (divisor >> 8) & 0xFF);
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp | 0x03);
}''')
reg('speaker.c', 'pcspkr_stop', '''void pcspkr_stop(void)
{
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp & ~0x03);
}''')

# ============ USB CORE ============
reg('usb_core.c', 'usb_submit_urb', '''int usb_submit_urb(struct urb *urb)
{
    if (!urb || !urb->dev) return -EINVAL;
    struct usb_device *dev = urb->dev;
    urb->status = -EINPROGRESS;
    urb->actual_length = 0;
    struct usb_host_controller *hc = dev->hc;
    if (!hc) return -ENODEV;
    switch (hc->type) {
    case USB_HC_UHCI: return uhci_submit_urb(hc, urb);
    case USB_HC_OHCI: return ohci_submit_urb(hc, urb);
    case USB_HC_EHCI: return ehci_submit_urb(hc, urb);
    case USB_HC_XHCI: return xhci_submit_urb(hc, urb);
    default: return -ENODEV;
    }
}''')
reg('usb_core.c', 'usb_kill_urb', '''int usb_kill_urb(struct urb *urb)
{
    if (!urb) return -EINVAL;
    urb->status = -ENOENT;
    return 0;
}''')
reg('usb_core.c', 'usb_control_msg', '''int usb_control_msg(struct usb_device *dev, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, void *data, uint16_t size, int timeout)
{
    if (!dev) return -EINVAL;
    (void)timeout;
    struct urb *urb = pmm_alloc(sizeof(struct urb));
    if (!urb) return -ENOMEM;
    __builtin_memset(urb, 0, sizeof(struct urb));
    urb->dev = dev;
    urb->setup_packet[0] = request_type;
    urb->setup_packet[1] = request;
    urb->setup_packet[2] = value & 0xFF;
    urb->setup_packet[3] = (value >> 8) & 0xFF;
    urb->setup_packet[4] = index & 0xFF;
    urb->setup_packet[5] = (index >> 8) & 0xFF;
    urb->setup_packet[6] = size & 0xFF;
    urb->setup_packet[7] = (size >> 8) & 0xFF;
    urb->buffer = data;
    urb->buffer_length = size;
    int ret = usb_submit_urb(urb);
    if (ret < 0) { pmm_free(urb, sizeof(struct urb)); return ret; }
    int timeout_us = timeout > 0 ? timeout * 1000 : 100000;
    while (urb->status == -EINPROGRESS && --timeout_us > 0) io_wait();
    ret = urb->actual_length;
    pmm_free(urb, sizeof(struct urb));
    return ret;
}''')

# ============ USB HID ============
reg('usb_hid.c', 'usb_hid_read', '''int usb_hid_read(struct usb_device *dev, void *buf, size_t count)
{
    if (!dev || !buf || count == 0) return -EINVAL;
    return usb_control_msg(dev, 0xA1, 0x01, 0, 0, buf, count, 1000);
}''')
reg('usb_hid.c', 'usb_hid_irq_handler', '''void usb_hid_irq_handler(struct urb *urb)
{
    if (!urb || urb->status != 0) return;
    struct usb_hid_device *hid = (struct usb_hid_device *)urb->dev->driver_data;
    if (hid && hid->callback) hid->callback(urb->buffer, urb->actual_length);
    usb_submit_urb(urb);
}''')

# ============ UHCI ============
reg('usb_ehci.c', 'uhci_submit_td', '''int uhci_submit_td(struct uhci_hcd *hc, struct uhci_td *td)
{
    if (!hc || !td) return -EINVAL;
    uint16_t frame = hc->frame_num;
    td->link = hc->frame_list[frame];
    hc->frame_list[frame] = td->vaddr;
    wmb();
    return 0;
}''')
reg('usb_ehci.c', 'uhci_process_td', '''int uhci_process_td(struct uhci_td *td)
{
    if (!td) return -EINVAL;
    uint32_t status = td->status;
    if (status & 0x0001) return -EINPROGRESS;
    if (status & 0x0080) { td->status = 0; return -EPIPE; }
    if (status & 0x0040) return -EIO;
    if (status & 0x0020) return -EIO;
    if (status & 0x0010) return -EAGAIN;
    if (status & 0x0002) return -EIO;
    return 0;
}''')
reg('usb_ehci.c', 'uhci_irq', '''void uhci_irq(struct interrupt_frame *frame)
{
    (void)frame;
    uint16_t status = inw(uhci_base + 0x08);
    outw(uhci_base + 0x08, status);
    if (status & 0x0001) uhci_process_completions();
    if (status & 0x0020) kprintf("[uhci] Host halted\\n");
    if (status & 0x0040) kprintf("[uhci] Host error\\n");
}''')

# ============ OHCI ============
reg('usb_ehci.c', 'ohci_submit_ed', '''int ohci_submit_ed(struct ohci_hcd *hc, struct ohci_ed *ed)
{
    if (!hc || !ed) return -EINVAL;
    ed->next_ed = hc->ed_head;
    hc->ed_head = ed;
    wmb();
    return 0;
}''')
reg('usb_ehci.c', 'ohci_irq', '''void ohci_irq(struct interrupt_frame *frame)
{
    (void)frame;
    struct ohci_hcd *hc = g_ohci_hcd;
    if (!hc) return;
    uint32_t status = hc->regs->intr_status;
    hc->regs->intr_status = status;
    if (status & OHCI_INTR_UE) kprintf("[ohci] Error\\n");
    if (status & OHCI_INTR_OC) kprintf("[ohci] Overcurrent\\n");
    if (status & OHCI_INTR_RHSC) ohci_root_hub_status(hc);
}''')

# ============ EHCI ============
reg('usb_ehci.c', 'ehci_submit_qtd', '''int ehci_submit_qtd(struct ehci_hcd *hc, struct ehci_qtd *qtd)
{
    if (!hc || !qtd) return -EINVAL;
    qtd->next = EHCI_PTR_TERMINATE;
    if (hc->async_qh) hc->async_qh->qtd_current = qtd->hw_self;
    wmb();
    return 0;
}''')
reg('usb_ehci.c', 'ehci_irq', '''void ehci_irq(struct interrupt_frame *frame)
{
    (void)frame;
    struct ehci_hcd *hc = g_ehci_hcd;
    if (!hc) return;
    uint32_t status = hc->regs->status;
    hc->regs->status = status;
    if (status & EHCI_STS_INT) ehci_process_async_completions(hc);
    if (status & EHCI_STS_PCD) ehci_port_status_change(hc);
    if (status & EHCI_STS_HSE) kprintf("[ehci] Host error\\n");
    if (status & EHCI_STS_IAA) ehci_async_advance(hc);
}''')

# ============ XHCI ============
reg('xhci.c', 'xhci_submit_trb', '''int xhci_submit_trb(struct xhci_hcd *hc, struct xhci_trb *trb)
{
    if (!hc || !trb) return -EINVAL;
    struct xhci_ring *ring = hc->cmd_ring;
    __builtin_memcpy(&ring->trbs[ring->enqueue], trb, sizeof(struct xhci_trb));
    ring->enqueue = (ring->enqueue + 1) % ring->size;
    writel(0, (volatile void *)(uintptr_t)(hc->dba + 0));
    wmb();
    return 0;
}''')
reg('xhci.c', 'xhci_irq', '''void xhci_irq(struct interrupt_frame *frame)
{
    (void)frame;
    struct xhci_hcd *hc = g_xhci_hcd;
    if (!hc) return;
    struct xhci_trb *evt = xhci_read_event_ring(hc);
    if (!evt) return;
    uint32_t trb_type = (evt->status >> 10) & 0x3F;
    switch (trb_type) {
    case TRB_COMPLETION: xhci_handle_cmd_completion(hc, evt); break;
    case TRB_PORT_STATUS: xhci_handle_port_status(hc, evt); break;
    case TRB_TRANSFER: xhci_handle_transfer_event(hc, evt); break;
    }
    xhci_update_erdp(hc);
}''')
reg('xhci.c', 'xhci_port_reset', '''int xhci_port_reset(struct xhci_hcd *hc, int port)
{
    if (!hc || port < 0) return -EINVAL;
    volatile uint32_t *portsc = &hc->regs->port_regs[port];
    *portsc = (*portsc & ~0x0FE0) | (1 << 4);
    wmb();
    int timeout = 100000;
    while ((*portsc & (1 << 4)) && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;
    return 0;
}''')

# ============ TPM ============
reg('tpm_tis.c', 'tpm_transmit', '''int tpm_transmit(struct tpm_chip *chip, const uint8_t *buf, size_t buf_size)
{
    if (!chip || !buf) return -EINVAL;
    for (size_t i = 0; i < buf_size; i++) {
        int timeout = 10000;
        while (!(inb(chip->base + 1) & 0x20) && --timeout > 0) io_wait();
        if (timeout == 0) return -EIO;
        outb(chip->base + 0, buf[i]);
    }
    return 0;
}''')
reg('tpm_tis.c', 'tpm_recv', '''int tpm_recv(struct tpm_chip *chip, uint8_t *buf, size_t buf_size)
{
    if (!chip || !buf) return -EINVAL;
    size_t count = 0;
    for (size_t i = 0; i < buf_size; i++) {
        int timeout = 10000;
        while (!(inb(chip->base + 1) & 0x01) && --timeout > 0) io_wait();
        if (timeout == 0) break;
        buf[count++] = inb(chip->base + 0);
    }
    return count;
}''')
reg('tpm_tis.c', 'tpm_send', '''int tpm_send(struct tpm_chip *chip, const uint8_t *cmd, size_t cmd_len, uint8_t *resp, size_t *resp_len)
{
    if (!chip || !cmd || !resp || !resp_len) return -EINVAL;
    int ret = tpm_transmit(chip, cmd, cmd_len);
    if (ret < 0) return ret;
    *resp_len = tpm_recv(chip, resp, *resp_len);
    return 0;
}''')

# ============ UART ============
reg('serial.c', 'uart_init', '''int uart_init(uint16_t port, uint32_t baud)
{
    kprintf("[uart] Init at 0x%x, baud=%u\\n", port, baud);
    outb(port + 3, 0x80);
    uint16_t divisor = 115200 / baud;
    outb(port + 0, divisor & 0xFF);
    outb(port + 1, (divisor >> 8) & 0xFF);
    outb(port + 3, 0x03);
    outb(port + 2, 0xC7);
    outb(port + 1, 0x01);
    outb(port + 4, 0x0B);
    return 0;
}''')
reg('serial.c', 'uart_write', '''int uart_write(uint16_t port, const uint8_t *data, size_t len)
{
    if (!data) return -EINVAL;
    for (size_t i = 0; i < len; i++) {
        int timeout = 10000;
        while (!(inb(port + 5) & 0x20) && --timeout > 0) io_wait();
        if (timeout == 0) return -EIO;
        outb(port, data[i]);
    }
    return len;
}''')
reg('serial.c', 'uart_read', '''int uart_read(uint16_t port, uint8_t *data, size_t len)
{
    if (!data) return -EINVAL;
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        int timeout = 10000;
        while (!(inb(port + 5) & 0x01) && --timeout > 0) io_wait();
        if (timeout == 0) break;
        data[count++] = inb(port);
    }
    return count;
}''')
reg('serial.c', 'uart_irq', '''void uart_irq(struct interrupt_frame *frame, uint16_t port)
{
    (void)frame;
    uint8_t iir = inb(port + 2);
    if (iir & 0x01) return;
    switch ((iir >> 1) & 0x03) {
    case 0: inb(port + 6); break;
    case 1: break;
    case 2: uart_handle_rx(port); break;
    case 3: inb(port + 5); break;
    }
}''')

# ============ PTY ============
reg('pty.c', 'pty_open', '''int pty_open(struct pty_device *pty)
{
    if (!pty) return -EINVAL;
    pty->refcount++;
    return 0;
}''')
reg('pty.c', 'pty_close', '''int pty_close(struct pty_device *pty)
{
    if (!pty) return -EINVAL;
    if (pty->refcount > 0) pty->refcount--;
    if (pty->refcount == 0) { pty->read_pos = pty->write_pos = 0; pty->eof = 1; }
    return 0;
}''')
reg('pty.c', 'pty_read', '''ssize_t pty_read(struct pty_device *pty, uint8_t *buf, size_t count)
{
    if (!pty || !buf) return -EINVAL;
    size_t available = pty->write_pos - pty->read_pos;
    if (available == 0) { if (pty->eof) return 0; return -EAGAIN; }
    size_t to_copy = count < available ? count : available;
    __builtin_memcpy(buf, pty->buffer + pty->read_pos, to_copy);
    pty->read_pos += to_copy;
    return to_copy;
}''')
reg('pty.c', 'pty_write', '''ssize_t pty_write(struct pty_device *pty, const uint8_t *buf, size_t count)
{
    if (!pty || !buf) return -EINVAL;
    size_t space = PTY_BUF_SIZE - pty->write_pos;
    if (space == 0) return -EAGAIN;
    size_t to_copy = count < space ? count : space;
    __builtin_memcpy(pty->buffer + pty->write_pos, buf, to_copy);
    pty->write_pos += to_copy;
    return to_copy;
}''')

# ============ NETWORK CARDS ============
reg('rtl8139.c', 'rtl8139_send', '''int rtl8139_send(struct net_device *dev, const void *data, int len)
{
    if (!dev || !data || len <= 0 || len > 1792) return -EINVAL;
    int timeout = 10000;
    while (!(inb(dev->base + 0x00) & 0x40) && --timeout > 0) io_wait();
    if (timeout == 0) return -EIO;
    for (int i = 0; i < len; i += 4) {
        uint32_t word = 0;
        __builtin_memcpy(&word, (const uint8_t *)data + i, (len - i) >= 4 ? 4 : (len - i));
        outl(dev->base + 0x20, word);
    }
    outl(dev->base + 0x00, len);
    dev->stats.tx_packets++;
    return 0;
}''')
reg('rtl8139.c', 'rtl8139_recv', '''int rtl8139_recv(struct net_device *dev, void *buf, int *len)
{
    if (!dev || !buf || !len) return -EINVAL;
    uint16_t capr = inw(dev->base + 0x38);
    uint16_t cbr = inw(dev->base + 0x34);
    if (capr == cbr) return -EAGAIN;
    uint8_t header[4];
    for (int i = 0; i < 4; i++) header[i] = inb(dev->base + 0x30 + capr);
    *len = (header[2] | (header[3] << 8)) - 4;
    if (*len > 0) {
        capr = (capr + 4) % 8192;
        for (int i = 0; i < *len; i++, capr = (capr + 1) % 8192)
            ((uint8_t *)buf)[i] = inb(dev->base + 0x30 + capr);
    }
    outw(dev->base + 0x38, capr - 16);
    dev->stats.rx_packets++;
    return 0;
}''')
reg('rtl8139.c', 'rtl8139_irq_handler', '''void rtl8139_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    uint16_t status = inw(dev->base + 0x3E);
    outw(dev->base + 0x3E, status);
    if (status & 0x0001) rtl8139_recv(dev, buf, &len);
    if (status & 0x0004) dev->stats.tx_errors++;
    if (status & 0x0010) dev->stats.rx_errors++;
}''')

reg('ne2k.c', 'ne2k_send', '''int ne2k_send(struct net_device *dev, const void *data, int len)
{
    if (!dev || !data || len <= 0) return -EINVAL;
    outb(dev->base + 0x00, 0x12);
    outb(dev->base + 0x0B, len & 0xFF);
    outb(dev->base + 0x0C, (len >> 8) & 0xFF);
    outb(dev->base + 0x09, 0x00);
    outb(dev->base + 0x0A, 0x40);
    outb(dev->base + 0x00, 0x12);
    for (int i = 0; i < len; i++) outb(dev->base + 0x10, ((const uint8_t *)data)[i]);
    int timeout = 10000;
    while ((inb(dev->base + 0x00) & 0x04) && --timeout > 0);
    if (timeout == 0) return -EIO;
    outb(dev->base + 0x00, 0x0A);
    dev->stats.tx_packets++;
    return 0;
}''')
reg('ne2k.c', 'ne2k_recv', '''int ne2k_recv(struct net_device *dev, void *buf, int *len)
{
    if (!dev || !buf || !len) return -EINVAL;
    uint8_t curr = inb(dev->base + 0x0D);
    uint8_t bndy = inb(dev->base + 0x0E);
    uint8_t next = bndy + 1;
    outb(dev->base + 0x00, 0x22);
    if (curr == next) return -EAGAIN;
    uint8_t rmt_start_low = next * 256;
    outb(dev->base + 0x0B, 0x00);
    outb(dev->base + 0x0C, 0x04);
    outb(dev->base + 0x09, rmt_start_low & 0xFF);
    outb(dev->base + 0x0A, (rmt_start_low >> 8) & 0xFF);
    outb(dev->base + 0x00, 0x0A);
    uint8_t hdr[4];
    for (int i = 0; i < 4; i++) hdr[i] = inb(dev->base + 0x10);
    *len = hdr[3] << 8 | hdr[2];
    if (*len > 1514) *len = 1514;
    outb(dev->base + 0x0B, *len & 0xFF);
    outb(dev->base + 0x0C, (*len >> 8) & 0xFF);
    outb(dev->base + 0x09, (rmt_start_low + 4) & 0xFF);
    outb(dev->base + 0x0A, ((rmt_start_low + 4) >> 8) & 0xFF);
    outb(dev->base + 0x00, 0x0A);
    for (int i = 0; i < *len; i++) ((uint8_t *)buf)[i] = inb(dev->base + 0x10);
    outb(dev->base + 0x00, 0x22);
    outb(dev->base + 0x0E, hdr[0] - 1);
    dev->stats.rx_packets++;
    return 0;
}''')

reg('pcnet32.c', 'pcnet32_send', '''int pcnet32_send(struct net_device *dev, const void *data, int len)
{
    if (!dev || !data || len <= 0 || len > 1514) return -EINVAL;
    struct pcnet32_priv *priv = dev->priv;
    int entry = priv->tx_ring_cur;
    priv->tx_ring[entry].length = -len;
    __builtin_memcpy(priv->tx_buf[entry], data, len);
    priv->tx_ring[entry].status = 0x0300;
    priv->tx_ring_cur = (entry + 1) % PCNET32_TX_RING_SIZE;
    outl(dev->base + 0x14, 0x0002);
    dev->stats.tx_packets++;
    return 0;
}''')
reg('pcnet32.c', 'pcnet32_recv', '''int pcnet32_recv(struct net_device *dev, void *buf, int *len)
{
    if (!dev || !buf || !len) return -EINVAL;
    struct pcnet32_priv *priv = dev->priv;
    int entry = priv->rx_ring_cur;
    if (!(priv->rx_ring[entry].status & 0x0300)) return -EAGAIN;
    *len = priv->rx_ring[entry].length & 0x0FFF;
    __builtin_memcpy(buf, priv->rx_buf[entry], *len);
    priv->rx_ring[entry].status = 0x8000;
    priv->rx_ring_cur = (entry + 1) % PCNET32_RX_RING_SIZE;
    outl(dev->base + 0x14, 0x0002);
    dev->stats.rx_packets++;
    return 0;
}''')

reg('virtio_net.c', 'virtio_net_send', '''int virtio_net_send(struct virtio_device *vdev, const void *data, int len)
{
    if (!vdev || !data || len <= 0) return -EINVAL;
    struct virtqueue *vq = vdev->vqs[0];
    struct scatterlist sg[1];
    sg_init_one(&sg[0], data, len);
    return virtqueue_add(vq, sg, 1, 0, NULL);
}''')
reg('virtio_net.c', 'virtio_net_recv', '''int virtio_net_recv(struct virtio_device *vdev, void *buf, int *len)
{
    if (!vdev || !buf || !len) return -EINVAL;
    struct virtqueue *vq = vdev->vqs[1];
    void *data = virtqueue_get_buf(vq, len);
    if (!data) return -EAGAIN;
    __builtin_memcpy(buf, data, *len);
    return 0;
}''')
reg('virtio_net.c', 'virtio_net_irq_handler', '''void virtio_net_irq_handler(struct virtio_device *vdev)
{
    if (!vdev) return;
    struct virtqueue *vq = vdev->vqs[1];
    int len; void *data;
    while ((data = virtqueue_get_buf(vq, &len)) != NULL)
        netif_rx(vdev->net_dev, data, len);
}''')

# ============ HWRNG ============
reg('hwrng.c', 'hwrng_read', '''int hwrng_read(uint8_t *buf, size_t count)
{
    if (!buf || count == 0) return -EINVAL;
    for (size_t i = 0; i + 4 <= count; i += 4) {
        uint64_t val; uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
        if (!ok) break;
        __builtin_memcpy(buf + i, &val, 4);
    }
    return 0;
}''')
reg('hwrng.c', 'hwrng_init', '''int hwrng_init(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (ecx & (1 << 30)) { kprintf("[hwrng] RDRAND available\\n"); return 0; }
    kprintf("[hwrng] RDRAND not available\\n");
    return -ENODEV;
}''')

# ============ GPIO ============
reg('gpio.c', 'gpio_direction_output', '''int gpio_direction_output(unsigned int gpio, int value)
{
    if (gpio >= GPIO_MAX_PINS) return -EINVAL;
    uint32_t reg = inl(GPIO_BASE + GPIO_DIR);
    reg |= (1 << gpio);
    outl(GPIO_BASE + GPIO_DIR, reg);
    gpio_set_value(gpio, value);
    return 0;
}''')
reg('gpio.c', 'gpio_set_value', '''void gpio_set_value(unsigned int gpio, int value)
{
    if (gpio >= GPIO_MAX_PINS) return;
    uint32_t reg = inl(GPIO_BASE + GPIO_DATA);
    if (value) reg |= (1 << gpio);
    else reg &= ~(1 << gpio);
    outl(GPIO_BASE + GPIO_DATA, reg);
}''')
reg('gpio.c', 'gpio_get_value', '''int gpio_get_value(unsigned int gpio)
{
    if (gpio >= GPIO_MAX_PINS) return -EINVAL;
    return (inl(GPIO_BASE + GPIO_DATA) >> gpio) & 1;
}''')
reg('gpio.c', 'gpio_request', '''int gpio_request(unsigned int gpio, const char *label)
{
    (void)label;
    if (gpio >= GPIO_MAX_PINS) return -EINVAL;
    if (gpio_claimed[gpio]) return -EBUSY;
    gpio_claimed[gpio] = 1;
    return 0;
}''')
reg('gpio.c', 'gpio_free', '''void gpio_free(unsigned int gpio)
{
    if (gpio < GPIO_MAX_PINS) gpio_claimed[gpio] = 0;
}''')

# ============ 9PNET ============
reg('9pnet_virtio.c', '_9pnet_send', '''int _9pnet_send(struct virtio_device *vdev, struct p9_req_t *req)
{
    if (!vdev || !req) return -EINVAL;
    struct virtqueue *vq = vdev->vqs[0];
    struct scatterlist sg[2];
    sg_init_one(&sg[0], req->tc, req->tc_size);
    sg_init_one(&sg[1], req->td, req->td_size);
    return virtqueue_add(vq, sg, 2, 1, req);
}''')
reg('9pnet_virtio.c', '_9pnet_recv', '''int _9pnet_recv(struct virtio_device *vdev, struct p9_req_t *req)
{
    if (!vdev || !req) return -EINVAL;
    struct virtqueue *vq = vdev->vqs[0];
    struct scatterlist sg[1];
    sg_init_one(&sg[0], req->rc, req->rc_size);
    return virtqueue_add(vq, sg, 0, 1, req);
}''')
reg('9pnet_virtio.c', '_9pnet_init', '''int _9pnet_init(struct virtio_device *vdev)
{
    if (!vdev) return -EINVAL;
    kprintf("[9pnet] Initializing\\n");
    virtio_set_status(vdev, VIRTIO_CONFIG_S_FEATURES_OK);
    if (virtio_find_vq(vdev, 0, &vdev->vqs[0]) < 0) return -ENODEV;
    virtio_set_status(vdev, VIRTIO_CONFIG_S_DRIVER_OK);
    return 0;
}''')

# ============ POWER SUSPEND ============
reg('suspend.c', 'suspend_enter', '''int suspend_enter(suspend_state_t state)
{
    kprintf("[suspend] Entering state %d\\n", state);
    cli();
    cpu_save_state();
    acpi_sleep(state);
    hlt();
    cpu_restore_state();
    sti();
    return 0;
}''')
reg('suspend.c', 'suspend_prepare', '''int suspend_prepare(suspend_state_t state)
{
    kprintf("[suspend] Preparing state %d\\n", state);
    driver_suspend_notify(state);
    process_freeze_all();
    vfs_sync_all();
    return 0;
}''')
reg('suspend.c', 'suspend_finish', '''int suspend_finish(suspend_state_t state)
{
    kprintf("[suspend] Finishing\\n");
    process_thaw_all();
    driver_resume_notify(state);
    return 0;
}''')
reg('suspend.c', 'suspend_valid', '''int suspend_valid(suspend_state_t state)
{
    switch (state) {
    case SUSPEND_STATE_STANDBY: case SUSPEND_STATE_MEM: case SUSPEND_STATE_DISK: return 1;
    default: return 0;
    }
}''')

# ============ POWER REBOOT ============
reg('reboot.c', 'machine_restart', '''void machine_restart(char *cmd)
{
    (void)cmd;
    kprintf("[reboot] Restarting\\n");
    if (acpi_reboot() == 0) return;
    int timeout = 100000;
    while ((inb(0x64) & 0x02) && --timeout > 0);
    outb(0x64, 0xFE);
    cli();
    while (1) hlt();
}''')
reg('reboot.c', 'machine_halt', '''void machine_halt(void)
{
    kprintf("[reboot] Halting\\n");
    cli();
    while (1) hlt();
}''')
reg('reboot.c', 'machine_power_off', '''void machine_power_off(void)
{
    kprintf("[reboot] Power off\\n");
    acpi_sleep(5);
    outw(0x600, 0x3400);
    outb(0x64, 0xFE);
    cli();
    while (1) hlt();
}''')

# ============ POWER CPU HOTPLUG ============
reg('cpu_hotplug.c', 'cpu_up', '''int cpu_up(unsigned int cpu)
{
    if (cpu >= NR_CPUS) return -EINVAL;
    if (cpu_online(cpu)) return 0;
    kprintf("[cpu-hotplug] Online CPU %u\\n", cpu);
    apic_send_init_ipi(cpu);
    apic_send_startup_ipi(cpu, AP_START_ADDR);
    cpu_set_online(cpu);
    return 0;
}''')
reg('cpu_hotplug.c', 'cpu_down', '''int cpu_down(unsigned int cpu)
{
    if (cpu >= NR_CPUS) return -EINVAL;
    if (!cpu_online(cpu)) return 0;
    if (cpu == 0) return -EBUSY;
    kprintf("[cpu-hotplug] Offline CPU %u\\n", cpu);
    sched_migrate_tasks(cpu);
    apic_send_ipi(cpu, IPI_HALT);
    cpu_set_offline(cpu);
    return 0;
}''')
reg('cpu_hotplug.c', 'cpu_hotplug_enable', '''int cpu_hotplug_enable(void)
{
    kprintf("[cpu-hotplug] Enabling\\n");
    g_cpu_hotplug_enabled = 1;
    return 0;
}''')
reg('cpu_hotplug.c', 'cpu_hotplug_disable_cpu', '''int cpu_hotplug_disable_cpu(unsigned int cpu)
{
    if (cpu >= NR_CPUS) return -EINVAL;
    g_disabled_cpus[cpu] = 1;
    return 0;
}''')

# ============ POWER CPUFREQ ============
reg('cpufreq.c', 'cpufreq_scale', '''uint32_t cpufreq_scale(uint32_t max_freq, uint32_t cur_freq, uint32_t max_perf, uint32_t cur_perf)
{
    (void)cur_freq;
    if (max_perf == 0) return 0;
    return max_freq * cur_perf / max_perf;
}''')
reg('cpufreq.c', 'cpufreq_set_target', '''int cpufreq_set_target(struct cpufreq_policy *policy, uint32_t target_khz)
{
    if (!policy) return -EINVAL;
    struct cpufreq_freqs freqs = { .old = policy->cur, .new = target_khz };
    cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
    if (policy->set_policy) policy->set_policy(policy, target_khz);
    policy->cur = target_khz;
    cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
    return 0;
}''')
reg('cpufreq.c', 'cpufreq_get_rate', '''uint32_t cpufreq_get_rate(struct cpufreq_policy *policy)
{
    if (!policy) return 0;
    return policy->cur;
}''')
reg('cpufreq.c', 'cpufreq_verify', '''int cpufreq_verify(struct cpufreq_policy *policy, uint32_t *target_khz)
{
    if (!policy || !target_khz) return -EINVAL;
    if (*target_khz < policy->min) *target_khz = policy->min;
    if (*target_khz > policy->max) *target_khz = policy->max;
    return 0;
}''')

# ============ POWER CPUIDLE ============
reg('cpuidle.c', 'cpuidle_enter', '''int cpuidle_enter(struct cpuidle_device *dev, struct cpuidle_state *state)
{
    if (!dev || !state) return -EINVAL;
    kprintf("[cpuidle] Entering %s\\n", state->name);
    cli(); hlt(); sti();
    return 0;
}''')
reg('cpuidle.c', 'cpuidle_select_state', '''int cpuidle_select_state(struct cpuidle_device *dev)
{
    if (!dev) return -EINVAL;
    int best = 0;
    for (int i = 0; i < dev->state_count; i++) {
        if (dev->states[i].exit_latency > dev->max_latency) break;
        best = i;
    }
    return best;
}''')
reg('cpuidle.c', 'cpuidle_reflect', '''void cpuidle_reflect(struct cpuidle_device *dev, int state_idx)
{
    if (!dev || state_idx < 0) return;
    dev->states[state_idx].usage++;
    dev->last_state = state_idx;
}''')

# ============ POWER ACPI CPU ============
reg('acpi_cpu.c', 'acpi_cpu_idle', '''int acpi_cpu_idle(int cpu)
{
    (void)cpu;
    cli(); hlt(); sti();
    return 0;
}''')
reg('acpi_cpu.c', 'acpi_cpu_on', '''int acpi_cpu_on(int cpu)
{
    kprintf("[acpi-cpu] Starting CPU %d\\n", cpu);
    apic_send_init_ipi(cpu);
    apic_send_startup_ipi(cpu, AP_START_ADDR);
    return 0;
}''')
reg('acpi_cpu.c', 'acpi_cpu_off', '''int acpi_cpu_off(int cpu)
{
    kprintf("[acpi-cpu] Stopping CPU %d\\n", cpu);
    apic_send_ipi(cpu, IPI_HALT);
    return 0;
}''')
reg('acpi_cpu.c', 'acpi_cpu_power_down', '''int acpi_cpu_power_down(int cpu)
{
    (void)cpu;
    inw(PM_BASE + PM1_STS);
    outw(PM_BASE + PM1_CNT, 0x2001);
    return 0;
}''')

# ============ POWER APM ============
reg('apm.c', 'apm_bios_call', '''int apm_bios_call(uint32_t func, uint32_t subfunc, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    if (!eax || !ebx || !ecx || !edx) return -EINVAL;
    uint32_t re, rb, rc, rd;
    __asm__ volatile("int $0x15" : "=a"(re), "=b"(rb), "=c"(rc), "=d"(rd) : "a"(func), "b"(subfunc), "c"(*ecx), "d"(*edx));
    *eax = re; *ebx = rb; *ecx = rc; *edx = rd;
    return (re & 0xFF) == 0 ? 0 : -EIO;
}''')
reg('apm.c', 'apm_get_power_status', '''int apm_get_power_status(struct apm_power_info *info)
{
    if (!info) return -EINVAL;
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    int ret = apm_bios_call(0x5300, 0x0002, &eax, &ebx, &ecx, &edx);
    if (ret < 0) return ret;
    info->ac_line_status = (ebx >> 8) & 0xFF;
    info->battery_status = ebx & 0xFF;
    info->battery_flag = (ebx >> 16) & 0xFF;
    info->battery_life = ecx & 0xFF;
    info->battery_time = edx & 0xFFFF;
    return 0;
}''')
reg('apm.c', 'apm_set_power_state', '''int apm_set_power_state(uint32_t state)
{
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    return apm_bios_call(0x5300, 0x0007, &eax, &ebx, &ecx, &edx);
}''')

# ============ POWER HIBERNATE ============
reg('hibernate.c', 'hibernate_enter', '''int hibernate_enter(void)
{
    kprintf("[hibernate] Entering\\n");
    hibernate_prepare_image();
    smp_stop_secondary_cpus();
    hibernate_save_image();
    machine_power_off();
    return 0;
}''')
reg('hibernate.c', 'hibernate_prepare_image', '''int hibernate_prepare_image(void)
{
    kprintf("[hibernate] Preparing image\\n");
    size_t image_size = pmm_get_total_pages() * PAGE_SIZE;
    if (hibernation_swap_reserve(image_size) < 0) return -ENOSPC;
    hibernate_mark_pages();
    return 0;
}''')
reg('hibernate.c', 'hibernate_resume_image', '''int hibernate_resume_image(void)
{
    kprintf("[hibernate] Resuming\\n");
    if (hibernation_restore_image() < 0) return -EIO;
    cpu_restore_all();
    return 0;
}''')


# ===== Apply implementations =====
def apply_impl(file_path, func_name, impl_code):
    """Try multiple strategies to find and replace the stub."""
    content = read_file(file_path)
    if not content:
        return False
    
    # Strategy 1: Find by stub marker comment
    for marker in [f"Stub: {func_name}", f"╌╌ Stub: {func_name}", f"── Stub: {func_name}"]:
        idx = content.find(marker)
        if idx >= 0:
            # Find the beginning of the line containing the marker
            start = content.rfind('\n', 0, idx)
            if start < 0: start = 0
            
            # Find where the function body ends - look for the closing brace of the function
            # Find "return -ENOSYS;" first
            enosys = content.find('return -ENOSYS;', idx)
            if enosys >= 0:
                # Find the closing brace after the return statement
                brace = content.find('}', enosys)
                if brace >= 0:
                    # Skip trailing whitespace
                    end = brace + 1
                    while end < len(content) and content[end] in ' \t\r\n':
                        end += 1
                    
                    # Replace: from marker line start through end of function body
                    new_content = content[:start] + '\n' + impl_code + '\n' + content[end:]
                    write_file(file_path, new_content)
                    print(f"  ✓ {func_name} (marker match)")
                    return True
    
    # Strategy 2: Find by function signature
    sig_line = impl_code.split('\n')[0].strip()
    # Escape special regex chars and search
    # Just search for the function name followed by (
    pattern = func_name + r'\s*\('
    match = re.search(pattern, content[idx:]) if idx >= 0 else re.search(pattern, content)
    if match:
        real_idx = (idx if idx >= 0 else 0) + match.start() if idx >= 0 else match.start()
        # Find where the function starts (beginning of line)
        line_start = content.rfind('\n', 0, real_idx)
        if line_start < 0: line_start = 0
        
        # Find the opening brace
        brace_open = content.find('{', real_idx)
        if brace_open < 0: return False
        
        # Find matching closing brace  
        depth = 0
        end = brace_open
        for i in range(brace_open, len(content)):
            if content[i] == '{': depth += 1
            elif content[i] == '}':
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
        
        if end > brace_open:
            new_content = content[:line_start] + '\n' + impl_code + '\n' + content[end:]
            write_file(file_path, new_content)
            print(f"  ✓ {func_name} (signature match)")
            return True
    
    print(f"  ✗ {func_name} - could not find in file")
    return False


def process_stubs():
    inv = load_inventory()
    target = [f for f in inv['files'] if f['subsystem'] in ('drivers', 'power')]
    print(f"Target: {len(target)} files, {sum(f['total_functions'] for f in target)} stubs")
    
    success = 0
    fail = 0
    
    for fi in target:
        file_rel = fi['file']
        file_path = os.path.join(SRC_DIR, file_rel)
        if not os.path.exists(file_path):
            # Try with different prefix
            alt = os.path.join(SRC_DIR, 'drivers', os.path.basename(file_rel))
            if os.path.exists(alt):
                file_path = alt
            else:
                continue
        
        fname = os.path.basename(file_path)
        if fname not in ALL_IMPLS:
            continue
        
        file_impls = ALL_IMPLS[fname]
        if not file_impls:
            continue
        
        print(f"\n{file_rel}:")
        for stub in fi['stubs']:
            fn = stub['function_name']
            if fn in file_impls:
                if apply_impl(file_path, fn, file_impls[fn]):
                    success += 1
                else:
                    fail += 1
    
    print(f"\n\nResults: {success} implemented, {fail} failed")
    return success, fail

if __name__ == '__main__':
    process_stubs()
