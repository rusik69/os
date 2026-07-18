# Filesystem Subsystem

**Path:** `src/fs/`
**Headers:** `src/include/vfs.h`, `src/include/page_cache.h`, and per-filesystem headers

The filesystem subsystem implements a Linux-compatible VFS layer with 30+ on-disk,
in-memory, network, and pseudo-filesystems. It provides block caching, file
locking, extended attributes, POSIX ACLs, encryption (LUKS, fscrypt), integrity
verification (fs-verity), and quota management.

## Architecture

```
System Calls → VFS Layer (src/fs/fs.c, vfs_enhance.c)
                  │
     ┌────────────┼────────────┬──────────────┐
     │            │            │              │
  On-Disk    In-Memory    Network       Pseudo
  ───────    ────────    ───────       ──────
  ext2       tmpfs       cifs          procfs
  ext4       ramfs       nfs           sysfs
  fat32      tarfs       nfsd          devfs
  ntfs       cpio                       debugfs
  btrfs      romfs
  ...        ...
                  │
     ┌────────────┴────────────┐
     │  Page Cache / Buf Cache │
     │  Block I/O Scheduler    │
     └─────────────────────────┘
```

## File Descriptions

| File | Description |
|------|-------------|
| **VFS Core** | |
| `fs.c` | Core VFS layer — system call implementations (open/read/write/close/seek/stat/mount/umount), path resolution, dentry cache, inode management, file descriptor table, mount table |
| `vfs_enhance.c` | VFS enhancements — fallocate, copy_file_range, dedupe_file_range, fadvise, FICLONERANGE ioctl, reflink support |
| `page_cache.c` | Generic page cache — 1024-entry LRU cache keyed by (inode, block), dirty writeback with configurable ratios, readahead with adaptive window sizing, working-set estimation via exponential decay |
| `bufcache.c` | Buffer cache — 64-entry LRU sector cache (512B per entry) for block-level users (FAT32), hash-bucket lookup, dirty tracking |
| `iosched.c` | Block I/O scheduler — deadline and CFQ-like policies for merging and reordering block requests |
| `initramfs.c` | Initramfs — embedded cpio/tar archive mounting at boot, loads kernel modules from /modules/ |
| `fstab.c` | fstab parser — reads /etc/fstab for automatic mount configuration at boot |
| `freeze.c` | Filesystem freeze/thaw — block writes during snapshot or fsck operations |
| `quota.c` | Disk quota management — per-user and per-group block/inode limits, quota enforcement on write |
| **On-Disk Filesystems** | |
| `ext2.c` | Second Extended Filesystem — read/write, sparse files, large files, symlinks, fast symlinks, HTree directory indexing |
| `ext4.c` | Fourth Extended Filesystem — read/write, extents, flex_bg, HTree dirs, large inodes, nanosecond timestamps, journal replay |
| `fat32.c` | FAT filesystem — read/write for FAT12/16/32, VFAT long filename support, volume labels |
| `fat32_lfn.c` | VFAT LFN + 8.3 short name generation — create/delete long file name entries, build short 8.3 names |
| `ntfs.c` | NTFS — read-only, MFT-based directory traversal, attribute resolution, basic file read |
| `exfat.c` | exFAT — read-only, large file support, exFAT allocation table |
| `btrfs.c` | Btrfs — read-only, copy-on-write, extents, checksum verification, subvolumes |
| `hfsplus.c` | HFS+ — read-only, B-tree catalog search, extents overflow |
| `hfs.c` | HFS — read/write for classic Mac OS Hierarchical File System |
| `reiserfs.c` | ReiserFS 3.x — read-only, B*-tree directory structure, block keying |
| `iso9660.c` | ISO 9660 — read-only, Rock Ridge (POSIX attributes), Joliet (Unicode), multi-session |
| `squashfs.c` | Squashfs — read-only, compressed filesystem, block-based decompression |
| `cramfs.c` | Cramfs — read-only, compressed ROM filesystem |
| `romfs.c` | Romfs — read-only, simple ROM filesystem |
| `tarfs.c` | TAR archive — read-only, embedded initramfs support |
| `cpio.c` | cpio archive — read-only, cpio format support |
| `bfs.c` | BFS — read/write, Sony's Boot File System |
| `minix.c` | Minix — read/write for Minix v1/v2/v3 |
| `ufs.c` | UFS (FFS) — read/write for Unix File System |
| `sysv.c` | System V — read/write for System V filesystem |
| `erofs.c` | EROFS — read-only, Enhanced Read-Only Filesystem for flash |
| `f2fs.c` | F2FS — read/write, Flash-Friendly Filesystem with multi-head logging |
| `jffs2.c` | JFFS2 — read/write, Journaling Flash File System v2 |
| `nilfs2.c` | NILFS2 — read/write, Log-structured filesystem with continuous snapshotting |
| `adfs.c` | ADFS — read/write, Acorn Disk Filing System |
| **Network Filesystems** | |
| `cifs.c` | CIFS/SMB client — read/write, SMB dialects, NTLM auth, oplocks |
| `nfs.c` | NFS client — read-only (v2/v3) |
| `nfsd.c` | NFS server — read/write (v3), export table, fsid-based exports |
| **Pseudo Filesystems** | |
| `procfs.c` | ProcFS — /proc/{uptime,meminfo,cpuinfo,stat,self,interrupts,modules,...} |
| `sysfs.c` | SysFS — kobject tree, kernel parameters, device hierarchy |
| `devfs.c` | DevFS — dynamic device node creation, hotplug |
| `debugfs.c` | DebugFS — kernel debug data, register dumps |
| **In-Memory / Union** | |
| `tmpfs.c` | Tmpfs — dynamic memory-backed filesystem, symlinks, device nodes, O_TMPFILE |
| `overlay_enhance.c` | OverlayFS enhancements — union mount with upper/lower dirs, whiteouts, copy-up |
| **Security & Storage** | |
| `crypto.c` | Filesystem encryption — fscrypt-compatible per-file key management |
| `verity.c` | fs-verity — Merkle tree-based file integrity verification |
| `luks.c` | LUKS — disk encryption key management for dm-crypt volumes |
| `xattr.c` | Extended attributes — user/trusted/system namespace support |
| `posix_acl.c` | POSIX ACL — access control list enforcement on file operations |
| `file_lock.c` | File locking — POSIX advisory locks (fcntl F_SETLK/F_GETLK) |
| `readdir.c` | Directory read helpers — getdents/getdents64 implementation |
| `fsck.c` | Filesystem consistency check — basic fsck operations |

## Key Conventions

- **VFS operations:** Every filesystem fills a `struct vfs_fsops` with open,
  read, write, close, mount, unmount, and directory operation callbacks.
- **Mount table:** Global `vfs_mount mounts[VFS_MAX_MOUNTS]` (16 entries). Each
  entry records mountpoint, filesystem ops, private data, and flags.
- **Page cache:** Keyed by `(inode, block_index)`. Dirty writeback triggers at
  10% of cache (background) and 50% (throttle). Read-ahead window adapts
  between 2 and 32 pages based on access patterns.
- **Initramfs:** Mounted at `/init` during boot before real rootfs.
  Modules loaded from `/modules/` in initramfs.
