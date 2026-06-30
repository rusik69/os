/*
 * sys_ioctl.c — Unified ioctl dispatch layer
 *
 * Provides the syscall-level ioctl(2) entry point.  Routes ioctl
 * commands to the appropriate subsystem handlers:
 *   - FD-level flags   FIOCLEX, FIONCLEX, FIONBIO, FIOASYNC
 *   - Terminal         TIOCGWINSZ, TIOCSWINSZ
 *   - Socket/Net       SIOCGIFNAME, SIOCGIFINDEX, SIOCGIFFLAGS, ...
 *   - Block device     SG_IO (SCSI generic passthrough)
 *
 * Each category is handled by a dedicated static function so that
 * later tasks (D129 tasks 6-9) can fill in stubs without touching
 * the top-level dispatch logic.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "errno.h"
#include "ioctl.h"
#include "process.h"
#include "socket.h"
#include "blockdev.h"
#include "uaccess.h"
#include "timer.h"
#include "timers.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "module.h"
#include "netdevice.h"
#include "vfs.h"
#include "sound_oss.h"

MODULE_LICENSE("MIT");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Unified ioctl dispatch layer");
MODULE_AUTHOR("OS Kernel Team");

/* ═══════════════════════════════════════════════════════════════════
 *  FD-LEVEL IOCTLS
 *  Operate on the per-process fd-table entry (flags, open_flags).
 *  ═══════════════════════════════════════════════════════════════════ */

/*
 * FIONCLEX — Clear close-on-exec flag on fd.
 * The arg parameter is ignored (pass 0).
 */
static int ioctl_fionclex(struct process *p, int fd)
{
	p->fd_table[fd].flags &= ~(uint8_t)FD_CLOEXEC;
	return 0;
}

/*
 * FIOCLEX — Set close-on-exec flag on fd.
 * The arg parameter is ignored (pass 0).
 */
static int ioctl_fioclex(struct process *p, int fd)
{
	p->fd_table[fd].flags |= (uint8_t)FD_CLOEXEC;
	return 0;
}

/*
 * FIONBIO — Set or clear the O_NONBLOCK flag on fd.
 * arg is a userspace pointer to int: non-zero → set, zero → clear.
 */
static int ioctl_fionbio(struct process *p, int fd, uint64_t arg)
{
	int val;
	if (copy_from_user(&val, arg, sizeof(val)) < 0)
		return -EFAULT;
	if (val)
		p->fd_table[fd].open_flags |= (uint8_t)04000;  /* O_NONBLOCK */
	else
		p->fd_table[fd].open_flags &= (uint8_t)~04000;
	return 0;
}

/*
 * FIOASYNC — Set or clear the async (signal-driven I/O) flag on fd.
 * arg is a userspace pointer to int: non-zero → set, zero → clear.
 */
static int ioctl_fioasync(struct process *p, int fd, uint64_t arg)
{
	int val;
	if (copy_from_user(&val, arg, sizeof(val)) < 0)
		return -EFAULT;
	if (val)
		p->fd_table[fd].open_flags |= (uint8_t)0x2000;  /* O_ASYNC */
	else
		p->fd_table[fd].open_flags &= (uint8_t)~0x2000;
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  TERMINAL IOCTLS (TIOC*)
 *  ═══════════════════════════════════════════════════════════════════ */

/*
 * TIOCGWINSZ — Get terminal window size.
 * arg is a userspace pointer to winsize struct { ws_row, ws_col,
 * ws_xpixel, ws_ypixel }.
 */
static int ioctl_tiocgwinsz(uint64_t arg)
{
	struct {
		unsigned short ws_row;
		unsigned short ws_col;
		unsigned short ws_xpixel;
		unsigned short ws_ypixel;
	} ws = { 25, 80, 0, 0 };

	if (copy_to_user(arg, &ws, sizeof(ws)) < 0)
		return -EFAULT;
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  NETWORK / SOCKET IOCTLS (SIOC*)
 *  ═══════════════════════════════════════════════════════════════════ */

/*
 * SIOCGIFNAME — Get interface name by index.
 * arg points to a struct ifreq with ifr_ifindex set.
 * On success, ifr_name is filled with the interface name.
 */
static int ioctl_siocgifname(uint64_t arg)
{
	struct ifreq ifr;
	int ret;

	ret = copy_from_user(&ifr, arg, sizeof(ifr));
	if (ret < 0)
		return -EFAULT;

	int idx = ifr.ifr_ifindex;
	if (idx < 0 || idx >= NETDEV_MAX)
		return -ENODEV;

	struct net_device *dev = netif_get(idx);
	if (!dev)
		return -ENODEV;

	strncpy(ifr.ifr_name, dev->name, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = '\0';

	ret = copy_to_user(arg, &ifr, sizeof(ifr));
	if (ret < 0)
		return -EFAULT;
	return 0;
}

/*
 * SIOCGIFINDEX — Get interface index by name.
 * arg points to a struct ifreq with ifr_name set.
 * On success, ifr_ifindex is filled with the interface index.
 */
static int ioctl_siocgifindex(uint64_t arg)
{
	struct ifreq ifr;
	int ret;

	ret = copy_from_user(&ifr, arg, sizeof(ifr));
	if (ret < 0)
		return -EFAULT;

	ifr.ifr_name[IFNAMSIZ - 1] = '\0';

	int idx = netif_name_to_index(ifr.ifr_name);
	if (idx < 0)
		return -ENODEV;

	ifr.ifr_ifindex = idx;

	ret = copy_to_user(arg, &ifr, sizeof(ifr));
	if (ret < 0)
		return -EFAULT;
	return 0;
}

/*
 * SIOCGIFHWADDR — Get hardware (MAC) address by interface name.
 * arg points to a struct ifreq with ifr_name set.
 * On success, ifr_hwaddr is filled with sa_family and 6-byte MAC.
 */
static int ioctl_siocgifhwaddr(uint64_t arg)
{
	struct ifreq ifr;
	int ret;

	ret = copy_from_user(&ifr, arg, sizeof(ifr));
	if (ret < 0)
		return -EFAULT;

	ifr.ifr_name[IFNAMSIZ - 1] = '\0';

	int idx = netif_name_to_index(ifr.ifr_name);
	if (idx < 0)
		return -ENODEV;

	struct net_device *dev = netif_get(idx);
	if (!dev)
		return -ENODEV;

	ifr.ifr_hwaddr.sa_family = 1;       /* ARPHRD_ETHER */
	memcpy(ifr.ifr_hwaddr.sa_data, dev->mac, 6);

	ret = copy_to_user(arg, &ifr, sizeof(ifr));
	if (ret < 0)
		return -EFAULT;
	return 0;
}

/*
 * SIOCGIFFLAGS — Get interface flags.
 * arg points to a struct ifreq with ifr_name set.
 * On success, ifr_flags is filled with the interface IFF_* flags.
 */
static int ioctl_siocgifflags(uint64_t arg)
{
	struct ifreq ifr;
	int ret;

	ret = copy_from_user(&ifr, arg, sizeof(ifr));
	if (ret < 0)
		return -EFAULT;

	ifr.ifr_name[IFNAMSIZ - 1] = '\0';

	int idx = netif_name_to_index(ifr.ifr_name);
	if (idx < 0)
		return -ENODEV;

	struct net_device *dev = netif_get(idx);
	if (!dev)
		return -ENODEV;

	ifr.ifr_flags = (short)dev->flags;

	ret = copy_to_user(arg, &ifr, sizeof(ifr));
	if (ret < 0)
		return -EFAULT;
	return 0;
}

/*
 * SIOCSIFFLAGS — Set interface flags.
 * arg points to a struct ifreq with ifr_name and ifr_flags set.
 * Only the flags that are modifiable from userspace are applied.
 */
static int ioctl_siocsifflags(uint64_t arg)
{
	struct ifreq ifr;
	int ret;

	ret = copy_from_user(&ifr, arg, sizeof(ifr));
	if (ret < 0)
		return -EFAULT;

	ifr.ifr_name[IFNAMSIZ - 1] = '\0';

	int idx = netif_name_to_index(ifr.ifr_name);
	if (idx < 0)
		return -ENODEV;

	struct net_device *dev = netif_get(idx);
	if (!dev)
		return -ENODEV;

	/* Only allow userspace to toggle IFF_UP, IFF_PROMISC,
	 * IFF_ALLMULTI, IFF_MULTICAST — not running state, etc. */
#define IFF_CHANGEABLE  (IFF_UP | IFF_PROMISC | IFF_ALLMULTI | IFF_MULTICAST)

	dev->flags = (dev->flags & ~IFF_CHANGEABLE) |
	             (ifr.ifr_flags & IFF_CHANGEABLE);

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  GENERIC FILE IOCTLS (FIONREAD, FIOQSIZE, FIGETBSZ, FS_IOC_*)
 *  ═══════════════════════════════════════════════════════════════════ */

/*
 * ioctl_file_dispatch — Handle file-level ioctl commands.
 *
 * Commands handled at the VFS layer without requiring a per-filesystem
 * callback: FIONREAD, FIOQSIZE, FIGETBSZ.
 * Commands that require filesystem support (FS_IOC_*) are dispatched
 * through vfs_ioctl() so each filesystem can implement them.
 *
 * Returns 0 on success or negative errno.
 */
static int ioctl_file_dispatch(struct process *p, int fd, uint64_t cmd,
                               uint64_t arg)
{
	const char *path = p->fd_table[fd].path;
	if (!path || path[0] == '\0')
		return -EBADF;

	switch (cmd) {
	case FIONREAD: {
		/* Return the number of bytes immediately available to read.
		 * For regular files this is the remaining bytes from the
		 * current offset to EOF. */
		struct vfs_stat st;
		int ret = vfs_stat(path, &st);
		if (ret < 0)
			return ret;

		uint64_t offset = p->fd_table[fd].offset;
		uint64_t avail = (st.size > offset) ? (st.size - offset) : 0;

		int val = (int)avail;
		if (copy_to_user(arg, &val, sizeof(val)) < 0)
			return -EFAULT;
		return 0;
	}

	case FIOQSIZE: {
		/* Return the file size as a 64-bit value */
		struct vfs_stat st;
		int ret = vfs_stat(path, &st);
		if (ret < 0)
			return ret;

		uint64_t size = st.size;
		if (copy_to_user(arg, &size, sizeof(size)) < 0)
			return -EFAULT;
		return 0;
	}

	case FIGETBSZ: {
		/* Return the filesystem block size */
		struct vfs_statfs st;
		int ret = vfs_statfs(path, &st);
		if (ret < 0)
			return ret;

		int bsize = (int)st.f_bsize;
		if (copy_to_user(arg, &bsize, sizeof(bsize)) < 0)
			return -EFAULT;
		return 0;
	}

	case FS_IOC_GETFLAGS:
	case FS_IOC_SETFLAGS:
	case FS_IOC_GETVERSION:
	case FS_IOC_SETVERSION:
		/* Dispatch filesystem-specific ioctls through VFS */
		return vfs_ioctl(path, cmd, arg);

	default:
		return -ENOTTY;
	}
}

/* ═══════════════════════════════════════════════════════════════════
 *  BLOCK-DEVICE IOCTLS
 *  ═══════════════════════════════════════════════════════════════════ */

/*
 * SG_IO — SCSI generic passthrough.
 * Submit a CDB (SCSI command descriptor block) to a block device
 * identified by the fd's path (/dev/sda, /dev/nvme0n1, etc.).
 * Returns sense data and status in the sg_io_hdr structure.
 */
static int ioctl_sg_io(struct process *p, int fd, uint64_t arg)
{
	struct sg_io_hdr hdr;

	if (copy_from_user(&hdr, arg, sizeof(hdr)) < 0)
		return -EFAULT;

	/* Validate interface ID */
	if (hdr.interface_id != 'S')
		return -ENOTTY;

	/* iovec not supported in this simple implementation */
	if (hdr.iovec_count != 0)
		return -EINVAL;

	/* Validate CDB length */
	if (hdr.cmd_len > SG_MAX_CDB_SIZE || hdr.cmd_len == 0)
		return -EINVAL;

	/* Resolve block device ID from the fd's path */
	const char *path = p->fd_table[fd].path;
	if (!path || path[0] == '\0')
		return -EBADF;

	int dev_id = blockdev_find_by_name(path);
	if (dev_id < 0)
		return -ENODEV;

	/* Copy CDB from user space */
	uint8_t cdb[SG_MAX_CDB_SIZE];
	memset(cdb, 0, sizeof(cdb));
	if (copy_from_user(cdb, (uint64_t)(uintptr_t)hdr.cmdp, hdr.cmd_len) < 0)
		return -EFAULT;

	/* Allocate data buffer (up to 64KB for safety) */
	uint32_t data_len = hdr.dxfer_len;
	if (data_len > 65536)
		return -EINVAL;

	void *data_buf = NULL;
	if (data_len > 0 && hdr.dxferp) {
		data_buf = (void *)kmalloc(data_len);
		if (!data_buf)
			return -ENOMEM;

		if (hdr.dxfer_direction == SG_DXFER_TO_DEV ||
		    hdr.dxfer_direction == SG_DXFER_TO_FROM_DEV) {
			/* Copy data FROM user for writes */
			if (copy_from_user(data_buf, (uint64_t)(uintptr_t)hdr.dxferp, data_len) < 0) {
				kfree(data_buf);
				return -EFAULT;
			}
		}
	}

	/* Sense buffer */
	uint8_t sense[SG_MAX_SENSE_SIZE];
	int sense_len = 0;
	memset(sense, 0, sizeof(sense));

	/* Submit the SCSI command */
	uint64_t start_tick = 0;
	if (timer_available())
		start_tick = timer_get_ticks();

	int ret = blockdev_scsi_submit(dev_id, cdb, hdr.cmd_len,
	                               data_buf, (int)data_len,
	                               hdr.dxfer_direction,
	                               sense, &sense_len,
	                               (int)hdr.timeout);

	/* Calculate duration in ms */
	uint32_t duration_ms = 0;
	if (timer_available()) {
		uint64_t elapsed = timer_get_ticks() - start_tick;
		duration_ms = (uint32_t)(elapsed * 1000 / TIMER_FREQ);
	}

	/* Fill in the hdr result fields */
	if (ret < 0) {
		hdr.status = 0xFF;                        /* SCSI status = host error */
		hdr.host_status = (unsigned short)(-ret);
		hdr.resid = (int)data_len;                  /* all data residual */
	} else {
		hdr.status = 0;                              /* GOOD status */
		hdr.host_status = 0;
		hdr.resid = 0;
	}
	hdr.duration = duration_ms;
	hdr.sb_len_wr = (unsigned char)sense_len;
	if (sense_len > 0 && hdr.sbp) {
		unsigned char mx_sb = hdr.mx_sb_len;
		if ((int)mx_sb > sense_len)
			mx_sb = (unsigned char)sense_len;
		if (copy_to_user((uint64_t)(uintptr_t)hdr.sbp, sense, mx_sb) < 0) {
			if (data_buf)
				kfree(data_buf);
			return -EFAULT;
		}
	}

	/* Copy data back to user for reads */
	if (data_buf && data_len > 0 && hdr.dxferp &&
	    (hdr.dxfer_direction == SG_DXFER_FROM_DEV ||
	     hdr.dxfer_direction == SG_DXFER_TO_FROM_DEV)) {
		if (copy_to_user((uint64_t)(uintptr_t)hdr.dxferp, data_buf, data_len) < 0) {
			kfree(data_buf);
			return -EFAULT;
		}
	}

	if (data_buf)
		kfree(data_buf);

	/* Copy the updated hdr back to user */
	if (copy_to_user(arg, &hdr, sizeof(hdr)) < 0)
		return -EFAULT;

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  MAIN DISPATCHER  —  sys_ioctl(fd, cmd, arg)
 *  ═══════════════════════════════════════════════════════════════════ */

/*
 * sys_ioctl — Top-level ioctl(2) entry point.
 *
 * Validates the file descriptor, then dispatches the command to the
 * appropriate sub-handler based on the ioctl command code.
 *
 * Returns 0 on success or a negative errno on failure (Linux syscall
 * convention — the caller wraps this in a uint64_t return).
 */
uint64_t sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
	struct process *p = process_get_current();
	if (!p)
		return (uint64_t)(int64_t)-ESRCH;
	if (fd >= PROCESS_FD_MAX || !p->fd_table[fd].used)
		return (uint64_t)(int64_t)-EBADF;

	/*
	 * FIOCLEX and FIONCLEX do not use arg as a pointer — allow arg == 0.
	 * All other commands that take a pointer argument reject NULL.
	 */
	switch (cmd) {
	case FIOCLEX:
	case FIONCLEX:
		break;
	default:
		if (!arg)
			return (uint64_t)(int64_t)-EINVAL;
		break;
	}

	/* Dispatch by command code */
	switch (cmd) {

	/* ── FD-level ioctls ────────────────────────────────────── */
	case FIONCLEX:
		return (uint64_t)(int64_t)ioctl_fionclex(p, (int)fd);
	case FIOCLEX:
		return (uint64_t)(int64_t)ioctl_fioclex(p, (int)fd);
	case FIONBIO:
		return (uint64_t)(int64_t)ioctl_fionbio(p, (int)fd, arg);
	case FIOASYNC:
		return (uint64_t)(int64_t)ioctl_fioasync(p, (int)fd, arg);

	/* ── Terminal ioctls ────────────────────────────────────── */
	case TIOCGWINSZ:
		return (uint64_t)(int64_t)ioctl_tiocgwinsz(arg);

	/* ── Network / Socket ioctls ────────────────────────────── */
	case SIOCGIFNAME:
		return (uint64_t)(int64_t)ioctl_siocgifname(arg);
	case SIOCGIFINDEX:
		return (uint64_t)(int64_t)ioctl_siocgifindex(arg);
	case SIOCGIFHWADDR:
		return (uint64_t)(int64_t)ioctl_siocgifhwaddr(arg);
	case SIOCGIFFLAGS:
		return (uint64_t)(int64_t)ioctl_siocgifflags(arg);
	case SIOCSIFFLAGS:
		return (uint64_t)(int64_t)ioctl_siocsifflags(arg);

	/* ── Generic file ioctls ───────────────────────────────── */
	case FIONREAD:
	case FIOQSIZE:
	case FIGETBSZ:
	case FS_IOC_GETFLAGS:
	case FS_IOC_SETFLAGS:
	case FS_IOC_GETVERSION:
	case FS_IOC_SETVERSION:
		return (uint64_t)(int64_t)ioctl_file_dispatch(p, (int)fd, cmd, arg);

	/* ── Block-device ioctls ────────────────────────────────── */
	case SG_IO:
		return (uint64_t)(int64_t)ioctl_sg_io(p, (int)fd, arg);

	/* ── OSS audio / sound ioctls ──────────────────────────── */
	case SNDCTL_DSP_RESET:
	case SNDCTL_DSP_SYNC:
	case SNDCTL_DSP_SPEED:
	case SNDCTL_DSP_GETBLKSIZE:
	case SNDCTL_DSP_SETFMT:
	case SNDCTL_DSP_CHANNELS:
	case SNDCTL_DSP_GETTRIGGER:
	case SNDCTL_DSP_SETTRIGGER:
	case SNDCTL_DSP_GETOSPACE:
	case SNDCTL_DSP_GETISPACE:
	case SNDCTL_DSP_SETFRAGMENT:
	case SNDCTL_DSP_GETCAPS:
	case SNDCTL_DSP_POST:
	case SNDCTL_DSP_GETIPTR:
	case SNDCTL_DSP_GETOPTR:
	case SNDCTL_DSP_SETRECORD_SOURCE:
	case SNDCTL_DSP_GETRECORD_SOURCE:
	case SNDCTL_DSP_SETRECORD_GAIN:
	case SNDCTL_DSP_GETRECORD_GAIN:
	case SOUND_MIXER_READ_VOLUME:
	case SOUND_MIXER_WRITE_VOLUME:
	case SOUND_MIXER_READ_MUTE:
	case SOUND_MIXER_WRITE_MUTE:
	case SOUND_MIXER_READ_RECMASK:
	case SOUND_MIXER_READ_DEVMASK:
	case SOUND_MIXER_READ_RECSRC:
	case SOUND_MIXER_WRITE_RECSRC:
	case SOUND_MIXER_READ_STEREO:
	case SOUND_MIXER_READ_CAPS:
		return (uint64_t)(int64_t)sound_oss_ioctl((int)cmd, arg);

	default:
		return (uint64_t)(int64_t)-ENOTTY;
	}
}
