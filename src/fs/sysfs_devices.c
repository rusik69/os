/*
 * sysfs_devices.c — per-device directories under /sys/devices/
 *
 * Creates Linux-compatible device topology under /sys/devices/,
 * starting with the PCI domain 0000:00 host bridge and per-device
 * directories named in the standard BDF format (0000:BB:DD.F).
 */

#include "sysfs.h"
#include "pci.h"
#include "printf.h"
#include "string.h"
#include "types.h"

/* ── PCI device directory creation ───────────────────────────────── */

/*
 * Create /sys/devices/pci0000:00/ directories for every discovered
 * PCI device.  Each device gets a directory named in the standard
 * Linux format: "0000:BB:DD.F" where BB = bus, DD = device/slot,
 * F = function number.
 */
static void sysfs_create_pci_device_dirs(void)
{
	int count = 0;
	static const char *pci_domain = "/sys/devices/pci0000:00";

	/* Create the PCI domain directory */
	if (sysfs_create_dir(pci_domain) < 0) {
		kprintf("[sysfs] Failed to create %s\n", pci_domain);
		return;
	}

	/* Enumerate all PCI buses and devices */
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

				/* Build device directory path:
				 * /sys/devices/pci0000:00/0000:BB:DD.F
				 */
				char devpath[80];
				int n = snprintf(devpath, sizeof(devpath),
						 "%s/0000:%02X:%02X.%X",
						 pci_domain,
						 (unsigned int)bus,
						 (unsigned int)slot,
						 (unsigned int)func);
				if (n < 0 || (uint32_t)n >= sizeof(devpath))
					continue;

				if (sysfs_create_dir(devpath) < 0) {
					kprintf("[sysfs] Failed to create "
						"device dir: %s\n", devpath);
					continue;
				}

				count++;
			}
		}
	}

	kprintf("[sysfs] Created %d PCI device directories under %s\n",
		count, pci_domain);
}

/* ── Public entry point ──────────────────────────────────────────── */

/*
 * Create all per-device sysfs directories.  Called from sysfs_init().
 * Currently creates PCI device directories under /sys/devices/pci0000:00/.
 * Additional device types (platform, virtio, USB) will be added in
 * subsequent D175 tasks.
 */
void sysfs_create_device_dirs(void)
{
	sysfs_create_pci_device_dirs();
}
