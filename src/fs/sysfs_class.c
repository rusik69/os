/*
 * sysfs_class.c — per-class directories under /sys/class/
 *
 * Creates Linux-compatible class topology under /sys/class/,
 * with a subdirectory for each device class (block, net, input,
 * sound, tty, misc, drm, video4linux, i2c-adapter, spi_master).
 *
 * Each class directory contains device entries that reference
 * devices under /sys/devices/ — analogous to the symlinks that
 * Linux creates under /sys/class/<class>/<device> -> ../../../devices/...
 */

#include "sysfs.h"
#include "genhd.h"
#include "netdevice.h"
#include "printf.h"
#include "string.h"
#include "types.h"

/* ── Forward declarations ──────────────────────────────────────────── */
static void sysfs_create_class_block(void);
static void sysfs_create_class_net(void);
static void sysfs_create_class_input(void);
static void sysfs_create_class_sound(void);
static void sysfs_create_class_tty(void);
static void sysfs_create_class_misc(void);
static void sysfs_create_class_drm(void);
static void sysfs_create_class_video4linux(void);
static void sysfs_create_class_i2c(void);
static void sysfs_create_class_spi(void);

/* ── Block class /sys/class/block/ ────────────────────────────────── */

/*
 * Create /sys/class/block/ with an entry per registered gendisk.
 *
 * Each block device gets a readable file at /sys/class/block/<name>
 * containing the canonical device path.  On Linux these would be
 * symlinks to ../../../devices/...; in our flat sysfs implementation
 * we expose the target path as file content.
 */
static void sysfs_create_class_block(void)
{
	int count = 0;

	/* Create /sys/class/block/ */
	if (sysfs_create_dir("/sys/class/block") < 0) {
		kprintf("[sysfs_class] Failed to create /sys/class/block\n");
		return;
	}

	/* Enumerate gendisk table */
	for (int i = 0; i < GENHD_MAX_DISKS; i++) {
		struct gendisk *disk = get_gendisk(i);
		if (!disk)
			continue;

		const char *name = disk_to_name(disk);
		if (name[0] == '\0') {
			put_disk(disk);
			continue;
		}

		char entry_path[96];
		int n = snprintf(entry_path, sizeof(entry_path),
				 "/sys/class/block/%s", name);
		if (n < 0 || (uint32_t)n >= sizeof(entry_path)) {
			put_disk(disk);
			continue;
		}

		/* Show the canonical device path (placeholder for now) */
		char content[100];
		int cn = snprintf(content, sizeof(content),
				 "device path: /sys/devices/block/%s\n", name);
		if (cn < 0 || (uint32_t)cn >= sizeof(content)) {
			put_disk(disk);
			continue;
		}

		if (sysfs_create_file(entry_path, content) < 0) {
			kprintf("[sysfs_class] Failed to create %s\n",
				entry_path);
		} else {
			count++;
		}

		put_disk(disk);
	}

	kprintf("[sysfs_class] Created %d entries under /sys/class/block/\n",
		count);
}

/* ── Net class /sys/class/net/ ────────────────────────────────────── */

/*
 * Create /sys/class/net/ with an entry per registered network interface.
 *
 * Each net device gets a readable file at /sys/class/net/<name>
 * containing the MAC address and canonical device path.
 */
static void sysfs_create_class_net(void)
{
	int count = 0;

	/* Create /sys/class/net/ */
	if (sysfs_create_dir("/sys/class/net") < 0) {
		kprintf("[sysfs_class] Failed to create /sys/class/net\n");
		return;
	}

	/* Enumerate netif table */
	for (int i = 0; i < NETDEV_MAX; i++) {
		struct net_device *ndev = netif_get(i);
		if (!ndev)
			continue;

		char entry_path[96];
		int n = snprintf(entry_path, sizeof(entry_path),
				 "/sys/class/net/%s", ndev->name);
		if (n < 0 || (uint32_t)n >= sizeof(entry_path))
			continue;

		/* Expose MAC address + device info */
		char content[128];
		int cn = snprintf(content, sizeof(content),
				 "address: %02x:%02x:%02x:%02x:%02x:%02x\n"
				 "ifindex: %d\n"
				 "mtu: %d\n",
				 ndev->mac[0], ndev->mac[1],
				 ndev->mac[2], ndev->mac[3],
				 ndev->mac[4], ndev->mac[5],
				 ndev->ifindex, ndev->mtu);
		if (cn < 0 || (uint32_t)cn >= sizeof(content))
			continue;

		if (sysfs_create_file(entry_path, content) < 0) {
			kprintf("[sysfs_class] Failed to create %s\n",
				entry_path);
		} else {
			count++;
		}

		/* Create /sys/class/net/<name>/device subdirectory */
		char dev_dir[96];
		n = snprintf(dev_dir, sizeof(dev_dir),
			     "/sys/class/net/%s/device", ndev->name);
		if (n > 0 && (uint32_t)n < sizeof(dev_dir))
			sysfs_create_dir(dev_dir);

		/* Create /sys/class/net/<name>/statistics subdirectory */
		char stat_dir[96];
		n = snprintf(stat_dir, sizeof(stat_dir),
			     "/sys/class/net/%s/statistics", ndev->name);
		if (n > 0 && (uint32_t)n < sizeof(stat_dir)) {
			if (sysfs_create_dir(stat_dir) == 0) {
				/* Add standard stat files (placeholder) */
				char stat_rx[120];
				snprintf(stat_rx, sizeof(stat_rx),
					 "%s/rx_bytes", stat_dir);
				sysfs_create_file(stat_rx, "0\n");

				char stat_tx[120];
				snprintf(stat_tx, sizeof(stat_tx),
					 "%s/tx_bytes", stat_dir);
				sysfs_create_file(stat_tx, "0\n");

				char stat_rx_packets[120];
				snprintf(stat_rx_packets, sizeof(stat_rx_packets),
					 "%s/rx_packets", stat_dir);
				sysfs_create_file(stat_rx_packets, "0\n");

				char stat_tx_packets[120];
				snprintf(stat_tx_packets, sizeof(stat_tx_packets),
					 "%s/tx_packets", stat_dir);
				sysfs_create_file(stat_tx_packets, "0\n");
			}
		}
	}

	kprintf("[sysfs_class] Created %d entries under /sys/class/net/\n",
		count);
}

/* ── Input class /sys/class/input/ ────────────────────────────────── */

/*
 * Create /sys/class/input/ directory structure.
 *
 * Placeholder — input device registration will be added as the
 * USB HID subsystem matures.
 */
static void sysfs_create_class_input(void)
{
	/* Create /sys/class/input/ */
	if (sysfs_create_dir("/sys/class/input") < 0) {
		kprintf("[sysfs_class] Failed to create /sys/class/input\n");
		return;
	}

	kprintf("[sysfs_class] Created /sys/class/input/ (placeholder)\n");
}

/* ── Sound class /sys/class/sound/ ────────────────────────────────── */

/*
 * Create /sys/class/sound/ directory structure with per-card entries.
 *
 * Creates card0..cardN directories; actual sound card registration
 * happens in the OSS/ALSA subsystem.
 */
static void sysfs_create_class_sound(void)
{
	/* Create /sys/class/sound/ */
	if (sysfs_create_dir("/sys/class/sound") < 0) {
		kprintf("[sysfs_class] Failed to create /sys/class/sound\n");
		return;
	}

	/* Create /sys/class/sound/card0/ as default sound card */
	sysfs_create_dir("/sys/class/sound/card0");

	/* Create PCM device entries under card0 */
	sysfs_create_dir("/sys/class/sound/card0/pcmC0D0p");
	sysfs_create_dir("/sys/class/sound/card0/pcmC0D0c");

	kprintf("[sysfs_class] Created /sys/class/sound/ with card0\n");
}

/* ── TTY class /sys/class/tty/ ────────────────────────────────────── */

/*
 * Create /sys/class/tty/ directory structure.
 *
 * Placeholder — TTY subsystem entries will be created when serial
 * and console devices are registered.
 */
static void sysfs_create_class_tty(void)
{
	/* Create /sys/class/tty/ */
	if (sysfs_create_dir("/sys/class/tty") < 0) {
		kprintf("[sysfs_class] Failed to create /sys/class/tty\n");
		return;
	}

	/* Create /sys/class/tty/console/ */
	sysfs_create_dir("/sys/class/tty/console");

	kprintf("[sysfs_class] Created /sys/class/tty/ (placeholder)\n");
}

/* ── Misc class /sys/class/misc/ ──────────────────────────────────── */

/*
 * Create /sys/class/misc/ directory structure.
 *
 * Placeholder — misc device entries will be populated as drivers
 * register with the misc framework.
 */
static void sysfs_create_class_misc(void)
{
	/* Create /sys/class/misc/ */
	if (sysfs_create_dir("/sys/class/misc") < 0) {
		kprintf("[sysfs_class] Failed to create /sys/class/misc\n");
		return;
	}

	kprintf("[sysfs_class] Created /sys/class/misc/ (placeholder)\n");
}

/* ── DRM class /sys/class/drm/ ────────────────────────────────────── */

/*
 * Create /sys/class/drm/ directory structure.
 *
 * Placeholder — entries will be created when GPU/drm drivers
 * initialise.
 */
static void sysfs_create_class_drm(void)
{
	/* Create /sys/class/drm/ */
	if (sysfs_create_dir("/sys/class/drm") < 0) {
		kprintf("[sysfs_class] Failed to create /sys/class/drm\n");
		return;
	}

	kprintf("[sysfs_class] Created /sys/class/drm/ (placeholder)\n");
}

/* ── Video4Linux class /sys/class/video4linux/ ────────────────────── */

/*
 * Create /sys/class/video4linux/ directory structure.
 *
 * Placeholder — video device entries will be populated as USB
 * webcam and framebuffer drivers register with V4L.
 */
static void sysfs_create_class_video4linux(void)
{
	/* Create /sys/class/video4linux/ */
	if (sysfs_create_dir("/sys/class/video4linux") < 0) {
		kprintf("[sysfs_class] Failed to create "
			"/sys/class/video4linux\n");
		return;
	}

	kprintf("[sysfs_class] Created /sys/class/video4linux/ "
		"(placeholder)\n");
}

/* ── I2C adapter class /sys/class/i2c-adapter/ ────────────────────── */

/*
 * Create /sys/class/i2c-adapter/ directory structure.
 *
 * Placeholder — entries will be created as I2C bus drivers
 * register adapters.
 */
static void sysfs_create_class_i2c(void)
{
	/* Create /sys/class/i2c-adapter/ */
	if (sysfs_create_dir("/sys/class/i2c-adapter") < 0) {
		kprintf("[sysfs_class] Failed to create "
			"/sys/class/i2c-adapter\n");
		return;
	}

	kprintf("[sysfs_class] Created /sys/class/i2c-adapter/ "
		"(placeholder)\n");
}

/* ── SPI master class /sys/class/spi_master/ ──────────────────────── */

/*
 * Create /sys/class/spi_master/ directory structure.
 *
 * Placeholder — entries will be created as SPI controllers
 * register with the SPI core.
 */
static void sysfs_create_class_spi(void)
{
	/* Create /sys/class/spi_master/ */
	if (sysfs_create_dir("/sys/class/spi_master") < 0) {
		kprintf("[sysfs_class] Failed to create "
			"/sys/class/spi_master\n");
		return;
	}

	kprintf("[sysfs_class] Created /sys/class/spi_master/ "
		"(placeholder)\n");
}

/* ── Public entry point ──────────────────────────────────────────── */

/*
 * Create all per-class sysfs directories.  Called from sysfs_init()
 * after the base /sys/class/ directory has been created.
 *
 * Creates class-specific directories under /sys/class/:
 *   - block:        Registered block devices (gendisk)
 *   - net:          Registered network interfaces (netif)
 *   - input:        Input devices (placeholder)
 *   - sound:        Sound card entries
 *   - tty:          TTY subsystem (placeholder)
 *   - misc:         Misc device entries (placeholder)
 *   - drm:          DRM/GPU device entries (placeholder)
 *   - video4linux:  Video4Linux device entries (placeholder)
 *   - i2c-adapter:  I2C adapter entries (placeholder)
 *   - spi_master:   SPI controller entries (placeholder)
 */
void sysfs_create_class_dirs(void)
{
	/* /sys/class/ is created by sysfs_init() before calling us */

	sysfs_create_class_block();
	sysfs_create_class_net();
	sysfs_create_class_input();
	sysfs_create_class_sound();
	sysfs_create_class_tty();
	sysfs_create_class_misc();
	sysfs_create_class_drm();
	sysfs_create_class_video4linux();
	sysfs_create_class_i2c();
	sysfs_create_class_spi();

	kprintf("[sysfs_class] Class directory tree created under "
		"/sys/class/\n");
}
