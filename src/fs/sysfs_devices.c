/*
 * sysfs_devices.c — per-device directories under /sys/devices/
 *
 * Creates Linux-compatible device topology under /sys/devices/,
 * starting with the PCI domain 0000:00 host bridge and per-device
 * directories named in the standard BDF format (0000:BB:DD.F).
 *
 * Also creates driver link entries under each device directory
 * and populates /sys/bus/pci/drivers/<name>/ directories.
 */

#include "sysfs.h"
#include "pci.h"
#include "printf.h"
#include "string.h"
#include "types.h"

/* ── PCI driver name table ───────────────────────────────────────── */

/*
 * Lookup entries by vendor/device ID.
 * Order matters — the first matching entry wins, so put more-specific
 * (or more-capable) drivers before generic fallbacks.
 */
static const struct {
	uint16_t vendor;
	uint16_t device;
	const char *name;
} pci_vendor_drivers[] = {
	/* Intel Gigabit / 10GbE */
	{ 0x8086, 0x10C9, "igb"      },  /* 82576 — igb is multi-queue capable */
	{ 0x8086, 0x1521, "igb"      },  /* I350 */
	{ 0x8086, 0x1533, "igb"      },  /* I210 */
	{ 0x8086, 0x100E, "e1000"    },  /* 82540EM (QEMU default) */
	{ 0x8086, 0x10D3, "e1000"    },  /* 82574L */
	{ 0x8086, 0x1572, "i40e"     },  /* XL710 */
	{ 0x8086, 0x1581, "i40e"     },  /* X710 */
	{ 0x8086, 0x1580, "i40e"     },  /* XL710 alternate */
	{ 0x8086, 0x154C, "i40e"     },  /* XL710 VF */
	{ 0x8086, 0x1570, "i40e"     },  /* XL710 VF (Hyper-V) */

	/* Realtek */
	{ 0x10EC, 0x8139, "rtl8139"  },

	/* VMware */
	{ 0x15AD, 0x07B0, "vmxnet3"  },
	{ 0x15AD, 0x07C0, "vmw_pvscsi"    },
	{ 0x15AD, 0x0790, "vmw_pvscsi"    },  /* legacy PVSCSI */

	/* Mellanox */
	{ 0x15B3, 0x1003, "mlx4"     },  /* ConnectX-3 */
	{ 0x15B3, 0x1007, "mlx4"     },  /* ConnectX-3 Pro */
	{ 0x15B3, 0x1004, "mlx4"     },  /* ConnectX-3 VF */
	{ 0x15B3, 0x1013, "mlx4"     },  /* ConnectX-3 IOV */

	/* VirtIO devices (0x1AF4) */
	{ 0x1AF4, 0x1000, "virtio-net"      },
	{ 0x1AF4, 0x1001, "virtio-blk"      },
	{ 0x1AF4, 0x1002, "virtio-balloon"  },
	{ 0x1AF4, 0x1003, "virtio-console"  },
	{ 0x1AF4, 0x1004, "virtio-scsi"     },
	{ 0x1AF4, 0x1005, "virtio-rng"      },
	{ 0x1AF4, 0x1009, "virtio-9p"       },
	{ 0x1AF4, 0x1042, "virtio-fs"       },
	{ 0x1AF4, 0x1050, "virtio-gpu"      },
	{ 0x1AF4, 0x1052, "virtio-input"    },
	{ 0x1AF4, 0x1057, "virtio-iommu"    },
	{ 0x1AF4, 0x1110, "ivshmem"         },
};

/*
 * Lookup entries by PCI class/subclass.
 * Used for devices whose driver probes by class code (NVMe, AHCI, XHCI, etc.)
 */
static const struct {
	uint8_t class_code;
	uint8_t subclass;
	const char *name;
} pci_class_drivers[] = {
	{ 0x01, 0x06, "ahci"    },  /* SATA controller */
	{ 0x01, 0x08, "nvme"    },  /* NVM Express */
	{ 0x03, 0x00, "vga"     },  /* VGA-compatible */
	{ 0x04, 0x01, "ac97"    },  /* Audio device */
	{ 0x0C, 0x03, "xhci"    },  /* USB 3.0 xHCI */
};

/*
 * Look up the driver name for a PCI device at (bus, slot, func).
 * Returns a pointer to a static string, or NULL if no driver knows
 * this device.
 *
 * Searches vendor/device table first (exact match), then falls back
 * to class/subclass table.
 */
static const char *pci_driver_for_device(uint8_t bus, uint8_t slot,
					 uint8_t func)
{
	uint32_t reg0;
	uint16_t vendor, device;
	uint32_t reg_hdr;
	uint8_t class_code, subclass;

	/* Read vendor/device ID */
	reg0 = pci_read(bus, slot, func, 0);
	vendor = (uint16_t)(reg0 & 0xFFFF);
	device = (uint16_t)(reg0 >> 16);
	if (vendor == 0xFFFF)
		return NULL;

	/* Read class code / subclass from register 0x08 */
	reg_hdr = pci_read(bus, slot, func, 0x08);
	class_code = (uint8_t)((reg_hdr >> 24) & 0xFF);
	subclass   = (uint8_t)((reg_hdr >> 16) & 0xFF);

	/* Search vendor/device table first */
	for (size_t i = 0; i < sizeof(pci_vendor_drivers) /
		     sizeof(pci_vendor_drivers[0]); i++) {
		if (pci_vendor_drivers[i].vendor == vendor &&
		    pci_vendor_drivers[i].device == device)
			return pci_vendor_drivers[i].name;
	}

	/* Fall back to class/subclass table */
	for (size_t i = 0; i < sizeof(pci_class_drivers) /
		     sizeof(pci_class_drivers[0]); i++) {
		if (pci_class_drivers[i].class_code == class_code &&
		    pci_class_drivers[i].subclass == subclass)
			return pci_class_drivers[i].name;
	}

	return NULL;
}

/* ── PCI device directory creation ───────────────────────────────── */

/*
 * Create /sys/devices/pci0000:00/ directories for every discovered
 * PCI device.  Each device gets a directory named in the standard
 * Linux format: "0000:BB:DD.F" where BB = bus, DD = device/slot,
 * F = function number.
 *
 * For each device, a "driver" file is also created showing the name
 * of the kernel driver bound to this device (or "unknown" if no
 * driver is registered).
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

				/*
				 * Create the "driver" file under this
				 * device directory.  On Linux this is a
				 * symlink to ../bus/pci/drivers/<name>/;
				 * here we create a readable file with
				 * the driver name and canonical path.
				 */
				const char *drv_name =
					pci_driver_for_device(
						(uint8_t)bus,
						(uint8_t)slot,
						(uint8_t)func);
				if (drv_name) {
					char drv_path[96];
					int dn = snprintf(drv_path,
							  sizeof(drv_path),
							  "%s/driver",
							  devpath);
					if (dn > 0 &&
					    (uint32_t)dn < sizeof(drv_path)) {
						char drv_content[128];
						int cn = snprintf(
							drv_content,
							sizeof(drv_content),
							"%s\n",
							drv_name);
						if (cn > 0 &&
						    (uint32_t)cn <
						    sizeof(drv_content))
							sysfs_create_file(
								drv_path,
								drv_content);
					}
				}

				count++;
			}
		}
	}

	kprintf("[sysfs] Created %d PCI device directories under %s\n",
		count, pci_domain);
}

/* ── PCI driver directories under /sys/bus/pci/drivers/ ──────────── */

/*
 * Create /sys/bus/pci/drivers/<name>/ directories for every known
 * PCI driver.  Each driver directory is created as a container;
 * actual bind/unbind control files can be added in later tasks.
 *
 * This is called from sysfs_bus.c after /sys/bus/pci/drivers/ exists.
 * We discover drivers by scanning all PCI devices and collecting the
 * set of unique driver names, then create directories for them.
 */
void sysfs_create_pci_driver_dirs(void)
{
	int count = 0;

	/* Collect the set of driver names already seen */
	static const char *drivers_seen[64];
	int n_seen = 0;

	/* Re-enumerate PCI to find driver names */
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
				const char *name = pci_driver_for_device(
					(uint8_t)bus, (uint8_t)slot,
					(uint8_t)func);
				if (!name)
					continue;

				/* Check if we already have a dir for this */
				int already = 0;
				for (int i = 0; i < n_seen; i++) {
					if (strcmp(drivers_seen[i], name)
					    == 0) {
						already = 1;
						break;
					}
				}
				if (already)
					continue;

				/* Track the name */
				if (n_seen >= 64)
					continue;
				drivers_seen[n_seen++] = name;

				/*
				 * Create /sys/bus/pci/drivers/<name>/
				 * as a container directory.
				 */
				char drv_dir[96];
				int dn = snprintf(drv_dir, sizeof(drv_dir),
						 "/sys/bus/pci/drivers/%s",
						 name);
				if (dn < 0 ||
				    (uint32_t)dn >= sizeof(drv_dir))
					continue;

				if (sysfs_create_dir(drv_dir) < 0) {
					kprintf("[sysfs] Failed to create "
						"driver dir: %s\n", drv_dir);
					continue;
				}

				/*
				 * Create a "device" reference file
				 * inside the driver directory (the
				 * reverse of the device→driver link).
				 * Content shows which device BDFs are
				 * bound to this driver.
				 */
				char dev_link[120];
				dn = snprintf(dev_link, sizeof(dev_link),
					     "%s/device", drv_dir);
				if (dn > 0 &&
				    (uint32_t)dn < sizeof(dev_link)) {
					char dev_content[128];
					int cn = snprintf(
						dev_content,
						sizeof(dev_content),
						"0000:%02X:%02X.%X\n",
						(unsigned int)bus,
						(unsigned int)slot,
						(unsigned int)func);
					if (cn > 0 &&
					    (uint32_t)cn <
					    sizeof(dev_content))
						sysfs_create_file(
							dev_link,
							dev_content);
				}

				count++;
			}
		}
	}

	if (count > 0)
		kprintf("[sysfs] Created %d PCI driver directories "
			"under /sys/bus/pci/drivers/\n", count);
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
