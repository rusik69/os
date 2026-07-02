/*
 * sysfs_bus.c — per-bus directories under /sys/bus/
 *
 * Creates Linux-compatible bus topology under /sys/bus/,
 * with devices/ and drivers/ subdirectories for each bus type.
 *
 * Supported buses:
 *   - pci:     PCI/PCIe bus with discovered devices and driver slots
 *   - platform:  Platform devices (ACPI, device-tree enumerated)
 *   - pnp:     Legacy PnP bus (placeholder)
 */

#include "sysfs.h"
#include "pci.h"
#include "printf.h"
#include "string.h"
#include "types.h"

/* ── Forward declarations ──────────────────────────────────────────── */
static void sysfs_create_pci_bus(void);
static void sysfs_create_platform_bus(void);
static void sysfs_create_pnp_bus(void);

/* ── PCI bus directory tree ───────────────────────────────────────── */

/*
 * Create /sys/bus/pci/ with devices/ and drivers/ subdirectories.
 *
 * devices/ contains an entry per discovered PCI device (named by BDF),
 * pointing to the device's directory under /sys/devices/pci0000:00/.
 *
 * drivers/ is a container for PCI driver directories; actual drivers
 * register themselves at probe time (handled in later D175 tasks).
 */
static void sysfs_create_pci_bus(void)
{
	int count = 0;

	/* Create /sys/bus/pci/ */
	if (sysfs_create_dir("/sys/bus/pci") < 0) {
		kprintf("[sysfs_bus] Failed to create /sys/bus/pci\n");
		return;
	}

	/* Create /sys/bus/pci/devices/ */
	if (sysfs_create_dir("/sys/bus/pci/devices") < 0) {
		kprintf("[sysfs_bus] Failed to create /sys/bus/pci/devices\n");
		return;
	}

	/* Create /sys/bus/pci/drivers/ */
	if (sysfs_create_dir("/sys/bus/pci/drivers") < 0) {
		kprintf("[sysfs_bus] Failed to create /sys/bus/pci/drivers\n");
		return;
	}

	/*
	 * Populate /sys/bus/pci/devices/ with an entry for every
	 * discovered PCI device.  Each entry is a small file whose
	 * content gives the canonical device path under /sys/devices/.
	 *
	 * We re-enumerate PCI bus 0..255, slot 0..31, matching the
	 * enumeration done in sysfs_devices.c.
	 */
	for (int bus = 0; bus < 256; bus++) {
		for (int slot = 0; slot < 32; slot++) {
			uint32_t reg0 = pci_read((uint8_t)bus,
						 (uint8_t)slot, 0, 0);
			uint16_t vid = (uint16_t)(reg0 & 0xFFFF);
			if (vid == 0xFFFF)
				continue;

			/* Check header type for multi-function */
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

				/* Build the BDF device path */
				char devlink[96];
				int n = snprintf(devlink, sizeof(devlink),
						 "/sys/devices/pci0000:00/"
						 "0000:%02X:%02X.%X",
						 (unsigned int)bus,
						 (unsigned int)slot,
						 (unsigned int)func);
				if (n < 0 || (uint32_t)n >= sizeof(devlink))
					continue;

				/*
				 * Create a readable file under
				 * /sys/bus/pci/devices/0000:BB:DD.F
				 * whose content shows the canonical device
				 * path (target of what would be a symlink
				 * on Linux).
				 */
				char entry_path[96];
				n = snprintf(entry_path, sizeof(entry_path),
					     "/sys/bus/pci/devices/"
					     "0000:%02X:%02X.%X",
					     (unsigned int)bus,
					     (unsigned int)slot,
					     (unsigned int)func);
				if (n < 0 || (uint32_t)n >= sizeof(entry_path))
					continue;

				/* Append a newline to the device path for display */
				char content[100];
				int cn = snprintf(content, sizeof(content),
						  "%s\n", devlink);
				if (cn < 0 || (uint32_t)cn >= sizeof(content))
					continue;

				if (sysfs_create_file(entry_path, content) < 0) {
					kprintf("[sysfs_bus] Failed to create "
						"%s\n", entry_path);
					continue;
				}

				count++;
			}
		}
	}

	kprintf("[sysfs_bus] Created %d PCI device entries under "
		"/sys/bus/pci/devices/\n", count);
}

/* ── Platform bus directory tree ───────────────────────────────────── */

/*
 * Create /sys/bus/platform/ with devices/ and drivers/ directories.
 *
 * Platform devices are enumerated by ACPI or device-tree; at boot
 * time there are none discovered yet, so the directories are created
 * as containers for later registration (task 3 will add class
 * directories which may enumerate platform devices).
 */
static void sysfs_create_platform_bus(void)
{
	/* Create /sys/bus/platform/ */
	if (sysfs_create_dir("/sys/bus/platform") < 0) {
		kprintf("[sysfs_bus] Failed to create /sys/bus/platform\n");
		return;
	}

	/* Create /sys/bus/platform/devices/ */
	if (sysfs_create_dir("/sys/bus/platform/devices") < 0) {
		kprintf("[sysfs_bus] Failed to create "
			"/sys/bus/platform/devices\n");
		return;
	}

	/* Create /sys/bus/platform/drivers/ */
	if (sysfs_create_dir("/sys/bus/platform/drivers") < 0) {
		kprintf("[sysfs_bus] Failed to create "
			"/sys/bus/platform/drivers\n");
		return;
	}

	kprintf("[sysfs_bus] Created /sys/bus/platform/ tree\n");
}

/* ── PnP bus directory tree (placeholder) ──────────────────────────── */

/*
 * Create /sys/bus/pnp/ with devices/ and drivers/ directories.
 *
 * Legacy PnP devices are not currently supported; this provides
 * the directory structure so that /sys/bus/ is complete.
 */
static void sysfs_create_pnp_bus(void)
{
	/* Create /sys/bus/pnp/ */
	if (sysfs_create_dir("/sys/bus/pnp") < 0) {
		kprintf("[sysfs_bus] Failed to create /sys/bus/pnp\n");
		return;
	}

	/* Create /sys/bus/pnp/devices/ */
	if (sysfs_create_dir("/sys/bus/pnp/devices") < 0) {
		kprintf("[sysfs_bus] Failed to create "
			"/sys/bus/pnp/devices\n");
		return;
	}

	/* Create /sys/bus/pnp/drivers/ */
	if (sysfs_create_dir("/sys/bus/pnp/drivers") < 0) {
		kprintf("[sysfs_bus] Failed to create "
			"/sys/bus/pnp/drivers\n");
		return;
	}

	kprintf("[sysfs_bus] Created /sys/bus/pnp/ tree (placeholder)\n");
}

/* ── Public entry point ──────────────────────────────────────────── */

/*
 * Create all per-bus sysfs directories.  Called from sysfs_init().
 *
 * Creates the /sys/bus/ root, then populates it with bus-specific
 * subdirectories:
 *   - pci:       full topology with device entries
 *   - platform:  empty container (populated by ACPI/DT at boot)
 *   - pnp:       legacy placeholder
 */
void sysfs_create_bus_dirs(void)
{
	/* Create /sys/bus/ root */
	if (sysfs_create_dir("/sys/bus") < 0) {
		kprintf("[sysfs_bus] Failed to create /sys/bus\n");
		return;
	}

	sysfs_create_pci_bus();
	sysfs_create_platform_bus();
	sysfs_create_pnp_bus();

	kprintf("[sysfs_bus] Bus directory tree created under /sys/bus/\n");
}
