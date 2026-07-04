# ── Filesystem modules — obj-m entries for fs/ .ko files ────────────────────
#
# This file is included by src/modules/Makefile.modules.
# All entries use obj-m += to append to the global module list.
#
# Categories:
#   Small RO     — tarfs, romfs, debugfs, sysfs, devfs, ext2
#   CD/FS       — iso9660
#   Read/write   — fat32, ext4, squashfs, fuse, btrfs, ntfs, exfat, hfsplus,
#                  cifs, nfsd, reiserfs, hfs, cramfs, minix, ufs, sysv, adfs,
#                  bfs, erofs, f2fs, jffs2, nilfs2, nfs, verity, readdir
#   FS infra    — freeze, quota, fstab, fsck, xattr, posix_acl
#   Virtual     — procfs, tmpfs, overlay

# ── Small read-only filesystem modules ──────────────────────────────────────
obj-m += fs/tarfs.ko
obj-m += fs/romfs.ko
obj-m += fs/debugfs.ko
obj-m += fs/sysfs.ko
obj-m += fs/devfs.ko
obj-m += fs/ext2.ko
obj-m += fs/iso9660.ko
obj-m += fs/fat32.ko
obj-m += kernel/overlay.ko

# ── Production filesystem modules ──────────────────────────────────────────
obj-m += fs/procfs.ko
procfs-objs := fs/procfs fs/procfs_cpuinfo fs/procfs_meminfo fs/procfs_stat
obj-m += fs/tmpfs.ko
obj-m += fs/ext4.ko
obj-m += fs/squashfs.ko
obj-m += fs/fuse.ko
obj-m += fs/btrfs.ko
obj-m += fs/ntfs.ko
obj-m += fs/exfat.ko
obj-m += fs/hfsplus.ko
obj-m += fs/cifs.ko
obj-m += fs/nfsd.ko
obj-m += fs/reiserfs.ko
obj-m += fs/hfs.ko
obj-m += fs/cramfs.ko
obj-m += fs/minix.ko
obj-m += fs/ufs.ko
obj-m += fs/sysv.ko
obj-m += fs/adfs.ko
obj-m += fs/bfs.ko
obj-m += fs/erofs.ko
obj-m += fs/f2fs.ko
obj-m += fs/jffs2.ko
obj-m += fs/nilfs2.ko
obj-m += fs/nfs.ko
obj-m += fs/verity.ko
obj-m += fs/readdir.ko

# ── FS infrastructure modules ───────────────────────────────────────────────
obj-m += fs/freeze.ko
obj-m += fs/quota.ko
obj-m += fs/fstab.ko
obj-m += fs/fsck.ko
obj-m += fs/xattr.ko
obj-m += fs/posix_acl.ko
