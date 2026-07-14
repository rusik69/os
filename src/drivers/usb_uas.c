// SPDX-License-Identifier: GPL-2.0-only
/*
 * usb_uas.c — USB Attached SCSI (UAS) transport driver
 *
 * Implements the USB Attached SCSI Protocol (UASP) for USB mass storage
 * devices operating in UAS mode (bInterfaceProtocol = 0x62).
 *
 * UAS provides SCSI command queuing using Information Units (IUs) sent
 * over bulk endpoints, as an alternative to the Bulk-Only Transport (BBB).
 *
 * References:
 *   USB Mass Storage Class — UAS, Revision 1.0 (T10/2099-D)
 *   SCSI Primary Commands (SPC-4), READ(10), WRITE(10), READ CAPACITY(10)
 *   EHCI Specification, Revision 1.0
 */

#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "usb.h"
#include "usb_msc.h"
#include "blockdev.h"
#include "pmm.h"
#include "module.h"

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("USB Attached SCSI (UAS) transport driver");
MODULE_AUTHOR("Rusik69 OS Kernel Team");

/* ── UAS Information Unit IDs ────────────────────────────────────────────── */
#define UAS_IU_COMMAND      0x01	/* Command IU: SCSI CDB + tag */
#define UAS_IU_SENSE        0x03	/* Sense IU: status + sense data */
#define UAS_IU_RESPONSE     0x04	/* Response IU: task mgmt response */
#define UAS_IU_READY        0x06	/* Ready IU: device ready indicator */

/* MSC protocol codes */
#define USB_MSC_PROTO_BBB   0x50	/* Bulk-Only Transport */
#define USB_MSC_PROTO_UAS   0x62	/* USB Attached SCSI */

/* ── UAS COMMAND IU (32 bytes) ──────────────────────────────────────────── */
struct uas_command_iu {
	uint8_t  iu_id;			/* 0x01 */
	uint8_t  reserved1;		/* reserved */
	uint16_t tag;			/* command tag (little-endian) */
	uint8_t  reserved2;		/* reserved */
	uint8_t  lun;			/* logical unit number */
	uint8_t  reserved3[2];		/* reserved */
	uint32_t data_transfer_len;	/* expected data length (LE) */
	uint8_t  cmd_len;		/* CDB length (6–32) */
	uint8_t  flags;			/* bit 0: 0=write, 1=read */
	uint8_t  reserved4[2];		/* reserved */
	uint8_t  cdb[16];		/* SCSI CDB (max 16 bytes) */
} __attribute__((packed));

/* ── UAS SENSE IU (minimum 8 bytes, up to 26+ bytes with sense data) ────── */
struct uas_sense_iu {
	uint8_t  iu_id;			/* 0x03 */
	uint8_t  reserved1;		/* reserved */
	uint16_t tag;			/* associated command tag (LE) */
	uint8_t  status;		/* SCSI status byte */
	uint8_t  reserved2[3];		/* reserved */
	uint8_t  sense_data[18];	/* optional sense data */
} __attribute__((packed));

/* ── UAS RESPONSE IU (8 bytes) ──────────────────────────────────────────── */
struct uas_response_iu {
	uint8_t  iu_id;			/* 0x04 */
	uint8_t  reserved1;		/* reserved */
	uint16_t tag;			/* associated command tag (LE) */
	uint8_t  response_code;		/* 0x00 = TASK_COMPLETE, 0x01 = FAILED */
	uint8_t  reserved2[3];
} __attribute__((packed));

/* ── EHCI TD / QH structures (same layout as usb_msc.c) ────────────────── */

/* Queue Element Transfer Descriptor (qTD) — EHCI spec §3.5 */
struct ehci_qtd {
	uint32_t next;		/* next qTD pointer (or 1 = terminate) */
	uint32_t alt_next;	/* alternate next (or 1) */
	uint32_t token;		/* status, PID, length, etc. */
	uint32_t buf[5];	/* up to 5×4 kB buffer pages */
	uint32_t _pad[3];	/* padding to 32-byte alignment */
} __attribute__((packed, aligned(32)));

/* Queue Head (qH) — EHCI spec §3.6 */
struct ehci_qh {
	uint32_t next_qh;	/* horizontal link (T=1 for end) */
	uint32_t ep_char;	/* endpoint characteristics */
	uint32_t ep_cap;	/* endpoint capabilities */
	uint32_t cur_qtd;	/* current qTD pointer */
	/* overlay — mirrors qTD */
	uint32_t next_qtd;
	uint32_t alt_qtd;
	uint32_t token;
	uint32_t buf[5];
	uint32_t _pad[3];
} __attribute__((packed, aligned(32)));

/* qTD token field bits */
#define QTD_STATUS_ACTIVE   (1u << 7)
#define QTD_STATUS_HALTED   (1u << 6)
#define QTD_STATUS_MASK     0xFFu
#define QTD_PID_OUT         (0u << 8)
#define QTD_PID_IN          (1u << 8)
#define QTD_PID_SETUP       (2u << 8)
#define QTD_IOC             (1u << 15)
#define QTD_CERR(x)         ((x) << 10)
#define QTD_BYTES(x)        ((uint32_t)(x) << 16)
#define QTD_DT              (1u << 31)

/* qH ep_char bits */
#define QH_DEVADDR(a)       ((a) & 0x7F)
#define QH_EP(n)            (((n) & 0xF) << 8)
#define QH_EPS_HS           (2u << 12)	/* high-speed */
#define QH_DTC              (1u << 14)	/* data toggle control */
#define QH_H                (1u << 15)	/* head of reclamation list */
#define QH_MAXPKT(n)        (((n) & 0x7FF) << 16)
#define QH_RL(n)            (((n) & 0xF) << 28)
#define QH_MULT(n)          (((n) & 3u) << 30)

/* EHCI Operational Register offsets */
#define EHCI_USBCMD         0x00
#define EHCI_USBSTS         0x04
#define EHCI_ASYNCLISTADDR  0x18
#define EHCI_CMD_ASE        (1u << 5)
#define EHCI_CMD_RUN        (1u << 0)
#define EHCI_STS_ASS        (1u << 15)

/* ── Driver state ────────────────────────────────────────────────────────── */
/* UAS uses a single device (same as usb_msc.c pattern) */
static uint64_t g_op_base     = 0;
static uint8_t  g_dev_addr    = 1;
static uint8_t  g_bulk_in_ep  = 0x81;	/* bulk IN endpoint address */
static uint8_t  g_bulk_out_ep = 0x02;	/* bulk OUT endpoint address */
static uint32_t g_max_lba     = 0;	/* total sectors - 1 */
static uint32_t g_block_size  = 512;	/* sector size */
static int      g_initialized = 0;	/* 1 = device found and ready */
static uint16_t g_tag         = 1;	/* monotonic command tag */

#define MAX_PKT 512	/* high-speed bulk max packet size */

/* ── EHCI MMIO helpers ──────────────────────────────────────────────────── */
static inline uint32_t op_rd(uint32_t off)
{
	return *(volatile uint32_t *)(g_op_base + off);
}

static inline void op_wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(g_op_base + off) = val;
}

static void busy_wait(volatile int n)
{
	while (n-- > 0)
		__asm__("pause");
}

/* ── Allocate a physically-contiguous buffer (page-aligned via pmm) ─────── */
static void *alloc_dma(size_t sz)
{
	(void)sz;
	uint64_t frame = pmm_alloc_frame();
	if (!frame)
		return (void *)0;
	void *p = PHYS_TO_VIRT(frame);
	memset(p, 0, 4096);
	return p;
}

/* ── Submit one async transfer via EHCI ──────────────────────────────────── */
/*
 * Sets up a single qH→qTD chain, programs the async list register, enables
 * the async schedule, polls until the qTD completes, then disables.
 *
 * @pid:     QTD_PID_SETUP / QTD_PID_IN / QTD_PID_OUT
 * @ep:      endpoint number (including direction bit for IN)
 * @data:    buffer (physical address must fit in 32-bit, < 4 kB)
 * @len:     transfer length in bytes
 * @toggle:  initial data toggle (0 or 1)
 * Returns 0 on success, negative on error.
 */
static int ehci_do_transfer(uint32_t pid, uint8_t ep, void *data,
			    uint32_t len, int toggle)
{
	if (!g_op_base)
		return -ENODEV;

	struct ehci_qh  *qh  = (struct ehci_qh  *)alloc_dma(sizeof(*qh));
	struct ehci_qtd *qtd = (struct ehci_qtd *)alloc_dma(sizeof(*qtd));
	if (!qh || !qtd) {
		if (qh)
			pmm_free_frame(VIRT_TO_PHYS((uint64_t)qh));
		if (qtd)
			pmm_free_frame(VIRT_TO_PHYS((uint64_t)qtd));
		return -ENOMEM;
	}

	/* Build qTD */
	uint32_t token = QTD_STATUS_ACTIVE
		       | QTD_CERR(3)
		       | pid
		       | QTD_BYTES(len)
		       | QTD_IOC;
	if (toggle)
		token |= QTD_DT;

	qtd->next     = 1;	/* terminate */
	qtd->alt_next = 1;
	qtd->token    = token;
	qtd->buf[0]   = (uint32_t)VIRT_TO_PHYS(data);

	/* Pre-compute page-aligned base for scatter/gather */
	uint32_t data_phys = (uint32_t)VIRT_TO_PHYS(data);
	uint32_t page_base = data_phys & ~0xFFFu;
	qtd->buf[1] = page_base + 0x1000u;
	qtd->buf[2] = page_base + 0x2000u;
	qtd->buf[3] = page_base + 0x3000u;
	qtd->buf[4] = page_base + 0x4000u;

	/* Build qH — points to itself (circular list of one) */
	uint32_t qh_phys = (uint32_t)VIRT_TO_PHYS(qh);
	qh->next_qh  = qh_phys;	/* T=0, type=00 (qH) */
	qh->ep_char  = QH_DEVADDR(g_dev_addr)
		     | QH_EP(ep & 0x0F)
		     | QH_EPS_HS
		     | QH_DTC
		     | QH_H
		     | QH_MAXPKT(MAX_PKT)
		     | QH_RL(4);
	qh->ep_cap   = QH_MULT(1);
	qh->cur_qtd  = 0;
	qh->next_qtd = (uint32_t)VIRT_TO_PHYS(qtd);	/* first qTD */
	qh->alt_qtd  = 1;
	qh->token    = 0;

	/* Programme async list */
	uint32_t old_async = op_rd(EHCI_ASYNCLISTADDR);
	uint32_t old_cmd   = op_rd(EHCI_USBCMD);

	op_wr(EHCI_ASYNCLISTADDR, qh_phys);

	/* Enable async schedule */
	op_wr(EHCI_USBCMD, old_cmd | EHCI_CMD_ASE);
	int timeout = 200000;
	while (!(op_rd(EHCI_USBSTS) & EHCI_STS_ASS) && --timeout)
		busy_wait(10);
	if (!timeout)
		goto fail;

	/* Poll for qTD completion */
	timeout = 2000000;
	while ((qtd->token & QTD_STATUS_ACTIVE) && --timeout)
		busy_wait(10);

	/* Disable async schedule */
	op_wr(EHCI_USBCMD, old_cmd & ~EHCI_CMD_ASE);
	timeout = 200000;
	while ((op_rd(EHCI_USBSTS) & EHCI_STS_ASS) && --timeout)
		busy_wait(10);

	/* Restore old async list pointer */
	op_wr(EHCI_ASYNCLISTADDR, old_async);

	int rc = 0;
	if (qtd->token & QTD_STATUS_MASK & ~QTD_STATUS_ACTIVE) {
		if (qtd->token & QTD_STATUS_HALTED)
			rc = -EIO;
	}

	pmm_free_frame(VIRT_TO_PHYS((uint64_t)qtd));
	pmm_free_frame(VIRT_TO_PHYS((uint64_t)qh));
	return rc;

fail:
	op_wr(EHCI_USBCMD, old_cmd & ~EHCI_CMD_ASE);
	op_wr(EHCI_ASYNCLISTADDR, old_async);
	pmm_free_frame(VIRT_TO_PHYS((uint64_t)qtd));
	pmm_free_frame(VIRT_TO_PHYS((uint64_t)qh));
	return -ETIMEDOUT;
}

/* ── Send a USB control request (SETUP + optional DATA + STATUS) ─────────── */
static int usb_control(uint8_t bm_req_type, uint8_t b_req,
		       uint16_t w_val, uint16_t w_idx, uint16_t w_len,
		       void *data)
{
	/* 8-byte SETUP packet */
	uint8_t *setup = (uint8_t *)alloc_dma(8);
	if (!setup)
		return -ENOMEM;

	setup[0] = bm_req_type;
	setup[1] = b_req;
	setup[2] = (uint8_t)(w_val & 0xFF);
	setup[3] = (uint8_t)(w_val >> 8);
	setup[4] = (uint8_t)(w_idx & 0xFF);
	setup[5] = (uint8_t)(w_idx >> 8);
	setup[6] = (uint8_t)(w_len & 0xFF);
	setup[7] = (uint8_t)(w_len >> 8);

	/* SETUP phase (toggle=0, PID_SETUP) */
	int rc = ehci_do_transfer(QTD_PID_SETUP, 0, setup, 8, 0);
	pmm_free_frame(VIRT_TO_PHYS((uint64_t)setup));
	if (rc < 0)
		return rc;

	/* DATA phase (toggle=1) */
	if (w_len && data) {
		uint32_t pid = (bm_req_type & 0x80) ?
			       QTD_PID_IN : QTD_PID_OUT;
		rc = ehci_do_transfer(pid, 0, data, w_len, 1);
		if (rc < 0)
			return rc;
	}

	/* STATUS phase — opposite direction, DATA1 */
	{
		uint32_t pid = (bm_req_type & 0x80) ?
			       QTD_PID_OUT : QTD_PID_IN;
		uint8_t *dummy = (uint8_t *)alloc_dma(4);
		if (!dummy)
			return -ENOMEM;
		rc = ehci_do_transfer(pid, 0, dummy, 0, 1);
		pmm_free_frame(VIRT_TO_PHYS((uint64_t)dummy));
	}

	return rc;
}

/* ── CLEAR_FEATURE(ENDPOINT_HALT) ────────────────────────────────────────── */
static int usb_clear_halt(uint8_t dev_addr, uint8_t ep_addr)
{
	(void)dev_addr;
	/* bmRequestType = 0x02 (Standard, Endpoint, Host-to-Device) */
	return usb_control(0x02, USB_REQ_CLEAR_FEATURE,
			   USB_FEATURE_ENDPOINT_HALT, ep_addr, 0, (void *)0);
}

/* ── UAS COMMAND IU builder ──────────────────────────────────────────────── */
/*
 * Build and send a UAS COMMAND Information Unit on the Bulk OUT endpoint.
 *
 * @tag:       Unique command tag
 * @lun:       Logical unit number
 * @cdb:       SCSI CDB bytes
 * @cdb_len:   CDB length (6, 10, 16)
 * @data_len:  Expected data transfer length
 * @dir:       0 = data out (host→device), 1 = data in (device→host)
 * Returns 0 on success, negative errno on failure.
 */
static int uas_send_command_iu(uint16_t tag, uint8_t lun,
			       const uint8_t *cdb, int cdb_len,
			       uint32_t data_len, int dir)
{
	struct uas_command_iu *cmd_iu;

	cmd_iu = (struct uas_command_iu *)alloc_dma(sizeof(*cmd_iu));
	if (!cmd_iu)
		return -ENOMEM;

	cmd_iu->iu_id            = UAS_IU_COMMAND;
	cmd_iu->reserved1        = 0;
	cmd_iu->tag              = tag;		/* already LE */
	cmd_iu->reserved2        = 0;
	cmd_iu->lun              = lun;
	cmd_iu->reserved3[0]     = 0;
	cmd_iu->reserved3[1]     = 0;
	cmd_iu->data_transfer_len = data_len;	/* already LE on x86 */
	cmd_iu->cmd_len          = (uint8_t)(cdb_len & 0xFF);
	cmd_iu->flags            = dir ? 0x01 : 0x00;
	cmd_iu->reserved4[0]     = 0;
	cmd_iu->reserved4[1]     = 0;
	memset(cmd_iu->cdb, 0, sizeof(cmd_iu->cdb));
	memcpy(cmd_iu->cdb, cdb, (size_t)(cdb_len < 16 ? cdb_len : 16));

	/*
	 * COMMAND IU always uses data toggle 0, sent on Bulk OUT.
	 * The EP number for the OUT pipe is just the endpoint number
	 * (without the direction bit in the address).
	 */
	int rc = ehci_do_transfer(QTD_PID_OUT,
				  g_bulk_out_ep & 0x0F,
				  cmd_iu, sizeof(*cmd_iu), 0);

	pmm_free_frame(VIRT_TO_PHYS((uint64_t)cmd_iu));
	return rc;
}

/* ── UAS STATUS / SENSE IU reader ────────────────────────────────────────── */
/*
 * Read the STATUS/SENSE IU from the Bulk IN endpoint after a command.
 * On success, @iu_id and @status are filled from the response.
 *
 * @toggle:  Data toggle for the status phase:
 *           1 (DATA1 PID) for writes and no-data commands,
 *           0 (DATA0 PID) after a data-in (read) phase.
 *
 * Returns 0 on success (with status indicating SCSI result),
 * negative errno on transport failure.
 */
static int uas_read_status_iu(uint8_t *out_iu_id, uint8_t *out_status,
			      int toggle)
{
	struct uas_sense_iu *sense_iu;

	sense_iu = (struct uas_sense_iu *)alloc_dma(sizeof(*sense_iu));
	if (!sense_iu)
		return -ENOMEM;

	int rc = ehci_do_transfer(QTD_PID_IN, g_bulk_in_ep,
				  sense_iu, sizeof(*sense_iu), toggle);

	if (rc == 0) {
		/*
		 * Check that the response is a SENSE IU or RESPONSE IU.
		 * Per UAS spec §5.2, the status IU uses either DATA1 PID
		 * (no data-in or write) or DATA0 PID (after data-in).
		 */
		if (sense_iu->iu_id == UAS_IU_SENSE ||
		    sense_iu->iu_id == UAS_IU_RESPONSE) {
			if (out_iu_id)
				*out_iu_id = sense_iu->iu_id;
			if (out_status)
				*out_status = sense_iu->status;
		} else {
			/* Unexpected IU id — protocol error */
			kprintf("[UAS] unexpected IU id 0x%02x on status pipe\n",
				sense_iu->iu_id);
			rc = -EPROTO;
		}
	}

	pmm_free_frame(VIRT_TO_PHYS((uint64_t)sense_iu));
	return rc;
}

/* ── UAS full command execution ──────────────────────────────────────────── */
/*
 * Execute a SCSI command over the UAS transport.
 *
 * Steps:
 * 1. Send COMMAND IU on Bulk OUT
 * 2. Transfer data (IN from Bulk IN, or OUT on Bulk OUT)
 * 3. Read STATUS IU from Bulk IN
 *
 * @cdb:       SCSI command descriptor block (6–16 bytes)
 * @cdb_len:   CDB length in bytes
 * @data:      Data buffer (NULL for no data phase)
 * @data_len:  Data transfer length
 * @dir:       0 = data out (host→device), 1 = data in (device→host)
 * Returns 0 on success, negative errno on failure.
 */
static int uas_scsi_exec(const uint8_t *cdb, int cdb_len,
			  void *data, uint32_t data_len, int dir)
{
	if (!g_initialized)
		return -ENODEV;

	uint16_t tag = g_tag++;
	int rc;
	int stalled;
	int max_retries = 2;

	do {
		stalled = 0;

		/* Step 1: Send COMMAND IU */
		rc = uas_send_command_iu(tag, 0, cdb, cdb_len,
					 data_len, dir);
		if (rc < 0) {
			/* COMMAND IU failed — likely OUT endpoint stalled */
			rc = usb_clear_halt(g_dev_addr, g_bulk_out_ep & 0x0F);
			if (rc == 0)
				stalled = 1;
			continue;
		}

		/* Step 2: Data transfer phase */
		if (data_len > 0 && data) {
			if (dir) {
				/* Read data from Bulk IN */
				rc = ehci_do_transfer(QTD_PID_IN,
						      g_bulk_in_ep,
						      data, data_len, 1);
			} else {
				/*
				 * Write data to Bulk OUT.
				 * Per UAS §5.2.3: after COMMAND IU
				 * (DATA0), data-out uses DATA1.
				 */
				rc = ehci_do_transfer(QTD_PID_OUT,
						      g_bulk_out_ep & 0x0F,
						      data, data_len, 1);
			}
			if (rc < 0) {
				/* Data phase failed — clear halt on endpoint */
				uint8_t ep = dir ? g_bulk_in_ep :
					   (g_bulk_out_ep & 0x0F);
				rc = usb_clear_halt(g_dev_addr, ep);
				if (rc == 0)
					stalled = 1;
				continue;
			}
		}

		/* Step 3: Read STATUS IU from Bulk IN */
		/*
		 * Status toggle depends on whether data-in occurred.
		 * Per UAS §5.2.2/5.2.3:
		 *   - Write or no-data: STATUS uses DATA1 (toggle=1)
		 *   - Read with data:    STATUS uses DATA0 (toggle=0)
		 *     because data-in already consumed DATA1.
		 */
		int status_toggle = (dir && data_len > 0 && data)
				   ? 0 : 1;
		uint8_t iu_id = 0;
		uint8_t status = 0;
		rc = uas_read_status_iu(&iu_id, &status,
					status_toggle);
		if (rc < 0) {
			/* Status read failed — IN endpoint stall */
			rc = usb_clear_halt(g_dev_addr, g_bulk_in_ep);
			if (rc == 0)
				stalled = 1;
			continue;
		}

		/* Check SCSI status */
		if (status != 0x00) {
			/*
			 * SCSI status non-zero:
			 * 0x02 = CHECK CONDITION
			 * 0x08 = BUSY
			 * 0x18 = RESERVATION CONFLICT
			 */
			kprintf("[UAS] SCSI status 0x%02x (tag %u)\n",
				status, (unsigned)tag);
			rc = -EIO;
			/* CHECK CONDITION — sense data available */
			if (status == 0x02)
				rc = -EIO;
		}

		if (rc == 0)
			break;

	} while (stalled && --max_retries > 0);

	if (rc < 0) {
		kprintf("[UAS] SCSI cmd 0x%02x failed: %d\n",
			cdb[0], rc);
	}

	return rc;
}

/* ── Config descriptor parser ────────────────────────────────────────────── */
/*
 * Parse the configuration descriptor to find a UAS interface (class 0x08,
 * subclass 0x06, protocol 0x62) and extract bulk IN/OUT endpoint addresses.
 * Returns 0 on success, negative on error.
 */
static int uas_parse_config(void)
{
	/* First get config descriptor header (9 bytes) to learn total length */
	uint8_t hdr[9];
	int rc = usb_control(0x80, USB_REQ_GET_DESCRIPTOR,
			     0x0200, 0, 9, hdr);
	if (rc < 0) {
		kprintf("  USB UAS: GET_CONFIG_DESC header failed (%d)\n", rc);
		return rc;
	}

	uint16_t total_len = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
	if (total_len < 9 || total_len > 512) {
		kprintf("  USB UAS: invalid config descriptor length %u\n",
			(unsigned)total_len);
		return -EINVAL;
	}

	/* Allocate buffer for full config descriptor */
	uint8_t *config = (uint8_t *)alloc_dma((size_t)total_len);
	if (!config)
		return -ENOMEM;

	rc = usb_control(0x80, USB_REQ_GET_DESCRIPTOR,
			 0x0200, 0, total_len, config);
	if (rc < 0) {
		kprintf("  USB UAS: GET_CONFIG_DESC full failed (%d)\n", rc);
		pmm_free_frame(VIRT_TO_PHYS((uint64_t)config));
		return rc;
	}

	/* Walk sub-descriptors looking for UAS interface */
	int found = 0;
	uint16_t offset = config[0];  /* skip config descriptor header */
	while (offset + 1 < total_len) {
		uint8_t desc_len   = config[offset];
		uint8_t desc_type  = config[offset + 1];

		if (desc_len == 0)
			break;	/* malformed: prevent infinite loop */
		if (offset + desc_len > total_len)
			break;

		if (desc_type == USB_DT_INTERFACE && desc_len >= 9) {
			uint8_t if_class    = config[offset + 5];
			uint8_t if_subclass = config[offset + 6];
			uint8_t if_proto    = config[offset + 7];

			/*
			 * UAS interface:
			 *   bInterfaceClass   = 0x08 (Mass Storage)
			 *   bInterfaceSubClass = 0x06 (SCSI)
			 *   bInterfaceProtocol = 0x62 (UAS)
			 */
			if (if_class == USB_CLASS_MASS_STOR &&
			    if_subclass == 0x06 &&
			    if_proto == USB_MSC_PROTO_UAS) {
				/* Walk endpoints inside this interface */
				uint8_t num_ep = config[offset + 4];
				uint16_t ep_offset = offset + desc_len;
				int found_in  = 0;
				int found_out = 0;

				while (num_ep > 0 &&
				       ep_offset + 1 < total_len) {
					uint8_t ep_len = config[ep_offset];
					uint8_t ep_type = config[ep_offset + 1];

					if (ep_len == 0)
						break;
					if (ep_type == USB_DT_ENDPOINT &&
					    ep_len >= 7) {
						uint8_t ep_addr =
							config[ep_offset + 2];
						uint8_t ep_attr =
							config[ep_offset + 3];

						if ((ep_attr & USB_ENDPOINT_XFERTYPE_MASK) ==
						    USB_ENDPOINT_XFER_BULK) {
							if (ep_addr & USB_ENDPOINT_DIR_IN) {
								g_bulk_in_ep = ep_addr;
								found_in = 1;
							} else {
								g_bulk_out_ep = ep_addr;
								found_out = 1;
							}
							num_ep--;
						}
					}
					ep_offset = (uint16_t)(ep_offset + ep_len);
				}

				if (found_in && found_out) {
					found = 1;
					break;
				}
			}
		}

		offset = (uint16_t)(offset + desc_len);
	}

	pmm_free_frame(VIRT_TO_PHYS((uint64_t)config));

	if (!found) {
		kprintf("  USB UAS: no UAS interface with bulk endpoints\n");
		return -ENODEV;
	}

	kprintf("  USB UAS: bulk IN=0x%02x bulk OUT=0x%02x\n",
		g_bulk_in_ep, g_bulk_out_ep);
	return 0;
}

/* ── SCSI INQUIRY (0x12) ─────────────────────────────────────────────────── */
#define SCSI_INQUIRY_LEN 36

static int scsi_inquiry(uint8_t *vendor, size_t vendor_sz,
			 uint8_t *product, size_t product_sz)
{
	uint8_t *buf = (uint8_t *)alloc_dma(SCSI_INQUIRY_LEN);
	if (!buf)
		return -ENOMEM;

	uint8_t cdb[6] = {0x12, 0, 0, 0, SCSI_INQUIRY_LEN, 0};
	int rc = uas_scsi_exec(cdb, 6, buf, SCSI_INQUIRY_LEN, 1);

	if (rc == 0 && vendor && vendor_sz > 0) {
		size_t vlen = vendor_sz - 1;
		if (vlen > 8)
			vlen = 8;
		memcpy(vendor, buf + 8, vlen);
		vendor[vlen] = '\0';
	}
	if (rc == 0 && product && product_sz > 0) {
		size_t plen = product_sz - 1;
		if (plen > 16)
			plen = 16;
		memcpy(product, buf + 16, plen);
		product[plen] = '\0';
	}

	if (rc == 0) {
		kprintf("  USB UAS: INQUIRY — %.8s %.16s rev=%.4s\n",
			(const char *)(buf + 8), (const char *)(buf + 16),
			(const char *)(buf + 32));
	}

	pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
	return rc;
}

/* ── SCSI TEST UNIT READY (0x00) ─────────────────────────────────────────── */
static int scsi_test_unit_ready(void)
{
	uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};
	return uas_scsi_exec(cdb, 6, (void *)0, 0, 0);
}

/* ── SCSI REQUEST SENSE (0x03) ───────────────────────────────────────────── */
struct scsi_sense_data {
	uint8_t  response_code;
	uint8_t  _rsvd0;
	uint8_t  sense_key;
	uint8_t  info[4];
	uint8_t  additional_len;
	uint8_t  _rsvd1[4];
	uint8_t  asc;
	uint8_t  ascq;
	uint8_t  _rsvd2[4];
} __attribute__((packed));

static int scsi_request_sense(uint8_t *sense_key, uint8_t *asc, uint8_t *ascq)
{
	struct scsi_sense_data *sense;

	sense = (struct scsi_sense_data *)alloc_dma(sizeof(*sense));
	if (!sense)
		return -ENOMEM;

	uint8_t cdb[6] = {0x03, 0, 0, 0, sizeof(*sense), 0};
	int rc = uas_scsi_exec(cdb, 6, sense, sizeof(*sense), 1);

	if (rc == 0) {
		if (sense_key)
			*sense_key = sense->sense_key & 0x0F;
		if (asc)
			*asc = sense->asc;
		if (ascq)
			*ascq = sense->ascq;
		kprintf("  USB UAS: SENSE key=0x%02x ASC=0x%02x ASCQ=0x%02x\n",
			sense->sense_key & 0x0F, sense->asc, sense->ascq);
	}

	pmm_free_frame(VIRT_TO_PHYS((uint64_t)sense));
	return rc;
}

/* ── SCSI READ CAPACITY (10) ─────────────────────────────────────────────── */
static int scsi_read_capacity(uint32_t *max_lba_out, uint32_t *block_size_out)
{
	uint8_t *buf = (uint8_t *)alloc_dma(8);
	if (!buf)
		return -ENOMEM;

	uint8_t cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	int rc = uas_scsi_exec(cdb, 10, buf, 8, 1);

	if (rc == 0) {
		uint32_t lba = ((uint32_t)buf[0] << 24) |
			       ((uint32_t)buf[1] << 16) |
			       ((uint32_t)buf[2] << 8)  |
			        (uint32_t)buf[3];
		uint32_t blk_size = ((uint32_t)buf[4] << 24) |
				    ((uint32_t)buf[5] << 16) |
				    ((uint32_t)buf[6] << 8)  |
				     (uint32_t)buf[7];
		if (max_lba_out)
			*max_lba_out = lba;
		if (block_size_out)
			*block_size_out = blk_size;
	}

	pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
	return rc;
}

/* ── SCSI READ(10) via UAS ───────────────────────────────────────────────── */
static int uas_read10(uint32_t lba, uint16_t count, void *buf)
{
	uint8_t cdb[10];

	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x28;		/* READ(10) */
	cdb[2] = (uint8_t)(lba >> 24);
	cdb[3] = (uint8_t)(lba >> 16);
	cdb[4] = (uint8_t)(lba >> 8);
	cdb[5] = (uint8_t)(lba);
	cdb[7] = (uint8_t)(count >> 8);
	cdb[8] = (uint8_t)(count);

	return uas_scsi_exec(cdb, 10, buf, (uint32_t)count * g_block_size, 1);
}

/* ── SCSI WRITE(10) via UAS ──────────────────────────────────────────────── */
static int uas_write10(uint32_t lba, uint16_t count, const void *buf)
{
	uint8_t cdb[10];

	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x2A;		/* WRITE(10) */
	cdb[2] = (uint8_t)(lba >> 24);
	cdb[3] = (uint8_t)(lba >> 16);
	cdb[4] = (uint8_t)(lba >> 8);
	cdb[5] = (uint8_t)(lba);
	cdb[7] = (uint8_t)(count >> 8);
	cdb[8] = (uint8_t)(count);

		return uas_scsi_exec(cdb, 10, (void *)(uintptr_t)buf,
				     (uint32_t)count * g_block_size, 0);
}

/* ── Block device callbacks ──────────────────────────────────────────────── */

static int uas_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
	return uas_read10(lba, count, buf);
}

static int uas_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
	return uas_write10(lba, count, buf);
}

static uint32_t uas_get_sectors(void)
{
	return g_max_lba + 1;
}

/* ── Public init / exit ──────────────────────────────────────────── */

/*
 * Probe for a UAS USB mass storage device.
 *
 * Performs USB enumeration on the first available USB device:
 * 1. SET_ADDRESS (to device address 1)
 * 2. GET_DESCRIPTOR Device (confirm alive and compatible)
 * 3. SET_CONFIGURATION 1
 * 4. Parse config descriptor for UAS interface
 * 5. Issue SCSI INQUIRY, TEST_UNIT_READY, READ_CAPACITY
 * 6. Register with block device layer
 *
 * Returns 0 on success, negative on error.
 */
static int usb_uas_init(void)
{
	if (!usb_is_present()) {
		kprintf("[USB] UAS: no USB host controller found\n");
		g_initialized = 0;
		return -ENODEV;
	}
	if (usb_get_device_count() == 0) {
		kprintf("[USB] UAS: no USB devices connected\n");
		g_initialized = 0;
		return -ENODEV;
	}

	g_op_base = ehci_get_op_base();
	if (!g_op_base) {
		kprintf("[USB] UAS: cannot get EHCI base\n");
		g_initialized = 0;
		return -ENODEV;
	}

	/* Reset device state */
	g_bulk_in_ep  = 0x81;
	g_bulk_out_ep = 0x02;
	g_tag = 1;
	g_max_lba = 0;
	g_block_size = 512;

	/* SET_ADDRESS (standard SETUP to ep0, device address = 1) */
	int rc = usb_control(0x00, USB_REQ_SET_ADDRESS, g_dev_addr, 0, 0,
			     (void *)0);
	if (rc < 0) {
		kprintf("  USB UAS: SET_ADDRESS failed (%d)\n", rc);
		g_initialized = 0;
		return rc;
	}
	busy_wait(50000);

	/* GET_DESCRIPTOR — device descriptor (just to confirm it's alive) */
	uint8_t *desc = (uint8_t *)alloc_dma(18);
	if (!desc) {
		g_initialized = 0;
		return -ENOMEM;
	}
	rc = usb_control(0x80, USB_REQ_GET_DESCRIPTOR, 0x0100, 0, 18, desc);
	if (rc < 0) {
		kprintf("  USB UAS: GET_DESCRIPTOR failed (%d)\n", rc);
		pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));
		g_initialized = 0;
		return rc;
	}

	/* Validate device descriptor */
	if (desc[0] != 18) {
		kprintf("  USB UAS: invalid descriptor length (%u)\n",
			(unsigned)desc[0]);
		pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));
		g_initialized = 0;
		return -EINVAL;
	}
	if (desc[1] != 0x01) {
		kprintf("  USB UAS: not a device descriptor (type 0x%02x)\n",
			desc[1]);
		pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));
		g_initialized = 0;
		return -EINVAL;
	}
	pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));

	/* SET_CONFIGURATION 1 */
	rc = usb_control(0x00, USB_REQ_SET_CONFIGURATION, 1, 0, 0, (void *)0);
	if (rc < 0) {
		kprintf("  USB UAS: SET_CONFIGURATION failed (%d)\n", rc);
		g_initialized = 0;
		return rc;
	}

	/* Parse config descriptor to discover UAS interface + bulk endpoints */
	rc = uas_parse_config();
	if (rc < 0) {
		kprintf("  USB UAS: no UAS interface found\n");
		g_initialized = 0;
		return rc;
	}

	/* Issue INQUIRY to identify the device */
	scsi_inquiry((uint8_t *)0, 0, (uint8_t *)0, 0);

	/* Wait for device ready (up to 5 retries) */
	for (int retry = 0; retry < 5; retry++) {
		rc = scsi_test_unit_ready();
		if (rc == 0)
			break;
		/* If check condition, request sense to clear */
		if (rc == -EIO) {
			scsi_request_sense((uint8_t *)0, (uint8_t *)0,
					   (uint8_t *)0);
		}
		busy_wait(100000);
	}

	/* READ CAPACITY to discover drive size and block size */
	uint32_t block_size = 512;
	rc = scsi_read_capacity(&g_max_lba, &block_size);
	if (rc < 0) {
		kprintf("  USB UAS: READ CAPACITY failed (%d)\n", rc);
		g_initialized = 0;
		return rc;
	}
	g_block_size = block_size;

	kprintf("  USB UAS: %lu sectors (%llu MB), block size=%u\n",
		(unsigned long)(g_max_lba + 1),
		(unsigned long long)((g_max_lba + 1) / 2048),
		(unsigned)g_block_size);

	/* Register with block device layer */
	rc = blockdev_register_legacy(BLOCKDEV_USB1, "uas0",
				      uas_read_sectors,
				      uas_write_sectors,
				      uas_get_sectors);
	if (rc < 0) {
		kprintf("  USB UAS: blockdev registration failed (%d)\n", rc);
		g_initialized = 0;
		return rc;
	}

	g_initialized = 1;
	kprintf("[OK] USB UAS — USB Attached SCSI driver\n");
	return 0;
}

/* Reverse usb_uas_init(): unregister block device and clear state */
static void usb_uas_exit(void)
{
	if (blockdev_is_registered(BLOCKDEV_USB1))
		blockdev_unregister(BLOCKDEV_USB1);
	g_op_base = 0;
	g_dev_addr = 1;
	g_max_lba = 0;
	g_initialized = 0;
	kprintf("[USB] UAS device unregistered\n");
}

module_init(usb_uas_init);
