/*
 * sysfs_firmware.c — firmware node information under /sys/firmware/
 *
 * Creates Linux-compatible firmware topology under /sys/firmware/,
 * exposing ACPI table information and device-tree (placeholder)
 * support.
 *
 * On Linux /sys/firmware/ contains ACPI tables, EFI variables, and
 * device-tree blobs.  This implementation provides:
 *
 *   /sys/firmware/acpi/
 *   /sys/firmware/acpi/tables/     — ACPI table visibility (DSDT, SSDTs)
 *   /sys/firmware/devicetree/      — Device tree base (placeholder)
 *
 * Per-device firmware node attributes (of_node, firmware_node) are
 * added to each PCI device directory, exposing ACPI _HID / _CID
 * compatibility identifiers when available.
 */

#include "sysfs.h"
#include "acpi.h"
#include "pci.h"
#include "string.h"
#include "printf.h"
#include "types.h"
#include "heap.h"

/* ── ACPI table information ──────────────────────────────────────── */

/*
 * Read callback for /sys/firmware/acpi/tables/info.
 * Returns a summary of all known ACPI tables using the DSDT/SSDT
 * globals declared in acpi.h.
 */
static int sysfs_read_acpi_tables_info(char *buf, uint32_t max_sz, void *priv)
{
	(void)priv;
	int total = 0, n;

	/* DSDT information */
	if (g_dsdt_base && g_dsdt_length > 0) {
		n = snprintf(buf, max_sz,
			     "DSDT:\t%p\t%u bytes\t(AML: %u bytes)\n",
			     (void *)g_dsdt_base, g_dsdt_length,
			     g_dsdt_aml_length);
		if (n > 0 && (uint32_t)n < max_sz) {
			total += n;
			buf += n;
			max_sz -= (uint32_t)n;
		}
	}

	/* SSDT information */
	if (g_acpi_ssdt_count > 0) {
		for (int i = 0; i < g_acpi_ssdt_count && i < ACPI_MAX_SSDT; i++) {
			if (!g_acpi_ssdt_tables[i].base)
				continue;
			n = snprintf(buf, max_sz,
				     "SSDT%d:\t%p\t%u bytes\t(AML: %u bytes)\n",
				     i,
				     (void *)g_acpi_ssdt_tables[i].base,
				     g_acpi_ssdt_tables[i].length,
				     g_acpi_ssdt_tables[i].aml_length);
			if (n > 0 && (uint32_t)n < max_sz) {
				total += n;
				buf += n;
				max_sz -= (uint32_t)n;
			}
		}
	}

	return total;
}

/*
 * Create the /sys/firmware/acpi/ subtree.
 *
 * Creates:
 *   /sys/firmware/acpi/
 *   /sys/firmware/acpi/tables/
 *   /sys/firmware/acpi/tables/info  — dynamic table listing
 *   /sys/firmware/acpi/tables/DSDT  — DSDT presence indicator
 *   /sys/firmware/acpi/tables/SSDT<N> — per-SSDT entry
 */
static void sysfs_create_firmware_acpi(void)
{
	/* Create /sys/firmware/acpi/ */
	if (sysfs_create_dir("/sys/firmware/acpi") < 0) {
		kprintf("[sysfs_firmware] Failed to create /sys/firmware/acpi\n");
		return;
	}

	/* Create /sys/firmware/acpi/tables/ */
	if (sysfs_create_dir("/sys/firmware/acpi/tables") < 0) {
		kprintf("[sysfs_firmware] Failed to create "
			"/sys/firmware/acpi/tables\n");
		return;
	}

	/* Dynamic info file showing all discovered ACPI tables */
	sysfs_create_writable_file("/sys/firmware/acpi/tables/info",
				   "ACPI tables: scanning...\n",
				   NULL,
				   sysfs_read_acpi_tables_info,
				   NULL);

	/* Create a DSDT entry showing basic table info */
	char dsdt_info[96];
	int n = snprintf(dsdt_info, sizeof(dsdt_info),
			 "DSDT at %p, length %u bytes\n",
			 (void *)g_dsdt_base, g_dsdt_length);
	if (n > 0 && (uint32_t)n < sizeof(dsdt_info))
		sysfs_create_file("/sys/firmware/acpi/tables/DSDT",
				  dsdt_info);

	/* Create entries for each SSDT found during boot */
	for (int i = 0; i < g_acpi_ssdt_count && i < ACPI_MAX_SSDT; i++) {
		if (!g_acpi_ssdt_tables[i].base)
			continue;

		char ssdt_path[80];
		n = snprintf(ssdt_path, sizeof(ssdt_path),
			     "/sys/firmware/acpi/tables/SSDT%d", i);
		if (n < 0 || (uint32_t)n >= sizeof(ssdt_path))
			continue;

		char ssdt_content[96];
		int cn = snprintf(ssdt_content, sizeof(ssdt_content),
				  "SSDT%d at %p, length %u bytes\n",
				  i,
				  (void *)g_acpi_ssdt_tables[i].base,
				  g_acpi_ssdt_tables[i].length);
		if (cn > 0 && (uint32_t)cn < sizeof(ssdt_content))
			sysfs_create_file(ssdt_path, ssdt_content);
	}

	/* Create /sys/firmware/acpi/tables/FACP if the FADT is found */
	{
		void *fadt = acpi_get_table("FACP");
		if (fadt) {
			sysfs_create_file(
				"/sys/firmware/acpi/tables/FACP",
				"FACP: Fixed ACPI Description Table "
				"(DSDT base / length tracked)\n");

			/* Create a link-style entry under /sys/firmware/acpi/ */
			char fadt_link[96];
			n = snprintf(fadt_link, sizeof(fadt_link),
				     "FACP found at %p\n", fadt);
			if (n > 0 && (uint32_t)n < sizeof(fadt_link))
				sysfs_create_file(
					"/sys/firmware/acpi/fadt", fadt_link);
		}
	}

	/* Create /sys/firmware/acpi/tables/APIC if MADT exists */
	{
		void *madt = acpi_get_table("APIC");
		if (madt) {
			sysfs_create_file(
				"/sys/firmware/acpi/tables/APIC",
				"APIC: Advanced Programmable Interrupt "
				"Controller Table (MADT)\n");

			char apic_link[96];
			n = snprintf(apic_link, sizeof(apic_link),
				     "APIC/MADT found at %p\n", madt);
			if (n > 0 && (uint32_t)n < sizeof(apic_link))
				sysfs_create_file(
					"/sys/firmware/acpi/apic", apic_link);
		}
	}

	/* Create /sys/firmware/acpi/tables/MCFG if PCI MMCONFIG table exists */
	{
		void *mcfg = acpi_get_table("MCFG");
		if (mcfg) {
			sysfs_create_file(
				"/sys/firmware/acpi/tables/MCFG",
				"MCFG: PCI Express Memory-mapped "
				"Configuration Space Table\n");

			char mcfg_link[96];
			n = snprintf(mcfg_link, sizeof(mcfg_link),
				     "MCFG found at %p\n", mcfg);
			if (n > 0 && (uint32_t)n < sizeof(mcfg_link))
				sysfs_create_file(
					"/sys/firmware/acpi/mcfg", mcfg_link);
		}
	}

	/* Create /sys/firmware/acpi/tables/HPET if HPET table exists */
	{
		void *hpet = acpi_get_table("HPET");
		if (hpet) {
			sysfs_create_file(
				"/sys/firmware/acpi/tables/HPET",
				"HPET: High Precision Event Timer Table\n");
		}
	}

	kprintf("[sysfs_firmware] Created /sys/firmware/acpi/ tree\n");
}

/* ── ACPI runtime power management ────────────────────────────────── */

/*
 * Create /sys/firmware/acpi/runtime/ with sleep state information.
 * Placeholder for ACPI runtime power management attributes.
 */
static void sysfs_create_firmware_acpi_runtime(void)
{
	if (sysfs_create_dir("/sys/firmware/acpi/runtime") < 0) {
		kprintf("[sysfs_firmware] Failed to create "
			"/sys/firmware/acpi/runtime\n");
		return;
	}

	sysfs_create_file("/sys/firmware/acpi/runtime/supported_states",
			  "S0 S3 S4 S5\n");

	kprintf("[sysfs_firmware] Created /sys/firmware/acpi/runtime/ "
		"(placeholder)\n");
}

/* ── Device Tree (placeholder) ────────────────────────────────────── */

/*
 * Create /sys/firmware/devicetree/ directory structure.
 *
 * On Linux this contains the FDT (Flattened Device Tree) base blob.
 * Placeholder — full DT support will be added when the device-tree
 * subsystem is integrated (FDT parsing, overlays, etc.).
 */
static void sysfs_create_firmware_devicetree(void)
{
	/* Create /sys/firmware/devicetree/ */
	if (sysfs_create_dir("/sys/firmware/devicetree") < 0) {
		kprintf("[sysfs_firmware] Failed to create "
			"/sys/firmware/devicetree\n");
		return;
	}

	/* Create /sys/firmware/devicetree/base/ (placeholder) */
	if (sysfs_create_dir("/sys/firmware/devicetree/base") < 0) {
		kprintf("[sysfs_firmware] Failed to create "
			"/sys/firmware/devicetree/base\n");
		return;
	}

	/* Create placeholder info files */
	sysfs_create_file("/sys/firmware/devicetree/base/model",
			  "Hermes-OS (placeholder, no FDT loaded)\n");
	sysfs_create_file("/sys/firmware/devicetree/base/compatible",
			  "hermes-os,unknown\n");

	kprintf("[sysfs_firmware] Created /sys/firmware/devicetree/ "
		"(placeholder)\n");
}

/* ── Per-device firmware node attributes ──────────────────────────── */

/*
 * Per-device firmware node — allocated for each PCI device to track
 * ACPI path and compatibility identifiers.
 *
 * On Linux this would reference a specific device-tree node or ACPI
 * namespace path.  We emulate it by providing ACPI _HST (bus number,
 * device number) which maps PCI devices to their ACPI namespace
 * equivalents.
 */
struct sysfs_fwnode_state {
	uint8_t  bus;
	uint8_t  slot;
	uint8_t  func;
};

/*
 * Read callback for /sys/devices/<path>/of_node/firmware_node.
 *
 * Returns the ACPI-style path for this PCI device, computed from
 * its bus/slot/function numbers.  On Linux with ACPI, PCI devices
 * show up under \_SB.PCI0.BB_DD_FN; we emulate that format here.
 */
static int sysfs_read_fwnode_path(char *buf, uint32_t max_sz, void *priv)
{
	struct sysfs_fwnode_state *fn = (struct sysfs_fwnode_state *)priv;
	if (!fn)
		return -EINVAL;

	int n = snprintf(buf, max_sz,
			 "\\_SB.PCI0.%02X_%02X_%X\n",
			 (unsigned int)fn->bus,
			 (unsigned int)fn->slot,
			 (unsigned int)fn->func);
	if (n < 0)
		return -EINVAL;
	return (uint32_t)n < max_sz ? n : (int)(max_sz - 1);
}

/*
 * Read callback for /sys/devices/<path>/of_node/compatible.
 *
 * Returns ACPI-compatible identifiers for this PCI device based on
 * its vendor and device IDs.  Format: "PNPXXXX" style compatible ID.
 */
static int sysfs_read_fwnode_compatible(char *buf, uint32_t max_sz,
					void *priv)
{
	struct sysfs_fwnode_state *fn = (struct sysfs_fwnode_state *)priv;
	if (!fn)
		return -EINVAL;

	uint32_t reg0 = pci_read(fn->bus, fn->slot, fn->func, 0);
	uint16_t vid = (uint16_t)(reg0 & 0xFFFF);
	uint16_t did = (uint16_t)(reg0 >> 16);

	int n = snprintf(buf, max_sz,
			 "PCI\\VEN_%04X&DEV_%04X\n"
			 "PCI\\VEN_%04X&DEV_%04X&REV_00\n",
			 vid, did, vid, did);
	if (n < 0)
		return -EINVAL;
	return (uint32_t)n < max_sz ? n : (int)(max_sz - 1);
}

/*
 * Create firmware node (of_node) subdirectory under a PCI device
 * directory, containing ACPI-style path and compatibility info.
 *
 * @devpath:  full path to the device directory (e.g.
 *            "/sys/devices/pci0000:00/0000:00:03.0")
 * @bus:      PCI bus number
 * @slot:     PCI slot number
 * @func:     PCI function number
 *
 * Creates:
 *   of_node/firmware_node  — ACPI namespace path string
 *   of_node/compatible     — ACPI _CID style compatibility IDs
 */
static void sysfs_create_dev_firmware_node(const char *devpath,
					   uint8_t bus, uint8_t slot,
					   uint8_t func)
{
	char of_dir[96];
	int n = snprintf(of_dir, sizeof(of_dir),
			 "%s/of_node", devpath);
	if (n < 0 || (uint32_t)n >= sizeof(of_dir))
		return;

	if (sysfs_create_dir(of_dir) < 0) {
		/* Directory may already exist from a prior run */
		return;
	}

	/* Create firmware_node — ACPI namespace path */
	char fwnode_path[100];
	n = snprintf(fwnode_path, sizeof(fwnode_path),
		     "%s/firmware_node", of_dir);
	if (n > 0 && (uint32_t)n < sizeof(fwnode_path)) {
		/* Static content — embedded ACPI path string */
		char content[80];
		int cn = snprintf(content, sizeof(content),
				  "\\_SB.PCI0.%02X_%02X_%X\n",
				  (unsigned int)bus,
				  (unsigned int)slot,
				  (unsigned int)func);
		if (cn > 0 && (uint32_t)cn < sizeof(content))
			sysfs_create_file(fwnode_path, content);
		else
			sysfs_create_file(fwnode_path, "unknown\n");
	}

	/* Create compatible — ACPI _CID style compatibility IDs */
	char compat_path[100];
	n = snprintf(compat_path, sizeof(compat_path),
		     "%s/compatible", of_dir);
	if (n > 0 && (uint32_t)n < sizeof(compat_path)) {
		uint32_t reg0 = pci_read(bus, slot, func, 0);
		uint16_t vid = (uint16_t)(reg0 & 0xFFFF);
		uint16_t did = (uint16_t)(reg0 >> 16);

		char comp_content[120];
		int cn = snprintf(comp_content, sizeof(comp_content),
				  "PCI\\VEN_%04X&DEV_%04X\n"
				  "PCI\\VEN_%04X&DEV_%04X&REV_00\n",
				  vid, did, vid, did);
		if (cn > 0 && (uint32_t)cn < sizeof(comp_content))
			sysfs_create_file(compat_path, comp_content);
	}
}

/*
 * Add firmware node (of_node) subdirectories to every discovered PCI
 * device under /sys/devices/pci0000:00/.
 *
 * This mirrors the same PCI enumeration done in sysfs_devices.c.
 * Each device gets an of_node/ directory with firmware_node and
 * compatible files.
 */
static void sysfs_create_pci_firmware_nodes(void)
{
	int count = 0;

	for (int bus = 0; bus < 256; bus++) {
		for (int slot = 0; slot < 32; slot++) {
			uint32_t reg0 = pci_read((uint8_t)bus,
						 (uint8_t)slot, 0, 0);
			uint16_t vid = (uint16_t)(reg0 & 0xFFFF);
			if (vid == 0xFFFF)
				continue;

			uint32_t reg_hdr = pci_read((uint8_t)bus,
						   (uint8_t)slot, 0, 0x0C);
			int is_multi = (reg_hdr & (1U << 23)) ? 1 : 0;
			int max_func = is_multi ? 8 : 1;

			for (int func = 0; func < max_func; func++) {
				reg0 = pci_read((uint8_t)bus,
						(uint8_t)slot,
						(uint8_t)func, 0);
				vid = (uint16_t)(reg0 & 0xFFFF);
				if (vid == 0xFFFF)
					continue;

				char devpath[80];
				int n = snprintf(devpath, sizeof(devpath),
						 "/sys/devices/pci0000:00/"
						 "0000:%02X:%02X.%X",
						 (unsigned int)bus,
						 (unsigned int)slot,
						 (unsigned int)func);
				if (n < 0 || (uint32_t)n >= sizeof(devpath))
					continue;

				sysfs_create_dev_firmware_node(
					devpath,
					(uint8_t)bus,
					(uint8_t)slot,
					(uint8_t)func);
				count++;
			}
		}
	}

	if (count > 0)
		kprintf("[sysfs_firmware] Added firmware nodes to %d "
			"PCI devices\n", count);
}

/* ── Public entry point ──────────────────────────────────────────── */

/*
 * Create all firmware node sysfs directories and per-device firmware
 * node attributes.  Called from sysfs_init() after the base /sys/
 * directory exists and after /sys/devices/ has been populated.
 *
 * Creates:
 *   /sys/firmware/                      — firmware root
 *   /sys/firmware/acpi/                 — ACPI table visibility
 *   /sys/firmware/acpi/tables/          — individual table entries
 *   /sys/firmware/acpi/runtime/         — ACPI sleep states
 *   /sys/firmware/devicetree/           — Device tree (placeholder)
 *   /sys/devices/.../of_node/           — per-device firmware nodes
 *
 * Per-device firmware nodes (of_node) reference the ACPI namespace
 * path for the device and expose _CID-style compatibility IDs.
 */
void sysfs_create_firmware_dirs(void)
{
	/* Create /sys/firmware/ root */
	if (sysfs_create_dir("/sys/firmware") < 0) {
		kprintf("[sysfs_firmware] Failed to create /sys/firmware\n");
		return;
	}

	sysfs_create_firmware_acpi();
	sysfs_create_firmware_acpi_runtime();
	sysfs_create_firmware_devicetree();

	/*
	 * Add per-device firmware node (of_node) subdirectories to all
	 * PCI devices.  This must run AFTER sysfs_create_device_dirs()
	 * has created the PCI device directories.
	 */
	sysfs_create_pci_firmware_nodes();

	kprintf("[sysfs_firmware] Firmware directory tree created "
		"under /sys/firmware/\n");
}
