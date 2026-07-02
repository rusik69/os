#!/usr/bin/env python3
"""
Create ext2 filesystem images for testing.

Generates valid ext2 images from scratch (byte-level) with support for:
  - Variable block sizes (1024, 2048, 4096)
  - Multiple block groups
  - Sparse superblock feature
  - Directories, regular files, symlinks (fast + slow)
  - Hard links (multiple directory entries referencing same inode)
  - Sparse files (holes via unallocated blocks)
  - Large files (via indirect/double/triple blocks)
  - Extended attributes (user, system, security namespaces)
  - HTree directory indexing
  - Online resize-compatible layout

Usage: python3 mkext2img.py <output.img> <size_mb>
       python3 mkext2img.py <output.img> <size_mb> --features <features>
       python3 mkext2img.py <output.img> <size_mb> --populate <src_dir>

Environment: $HOME/os for output when path is relative.

Features (comma-separated): sparse,filetype,largefile,htree,ext_attr,acl,resize
Default: sparse,filetype
"""

import struct
import sys
import os
import math
import hashlib

SECTOR_SIZE = 512

# ── Constants ──────────────────────────────────────────────────────

EXT2_SUPER_MAGIC = 0xEF53
EXT2_ROOT_INO = 2

EXT2_GOOD_OLD_REV = 0
EXT2_DYNAMIC_REV = 1
EXT2_GOOD_OLD_INODE_SIZE = 128
EXT2_GOOD_OLD_FIRST_INO = 11

# Feature flags
EXT2_FEATURE_COMPAT_DIR_PREALLOC = 0x0001
EXT2_FEATURE_COMPAT_IMAGIC_INODES = 0x0002
EXT2_FEATURE_COMPAT_HAS_JOURNAL = 0x0004
EXT2_FEATURE_COMPAT_EXT_ATTR = 0x0008
EXT2_FEATURE_COMPAT_RESIZE_INO = 0x0010
EXT2_FEATURE_COMPAT_DIR_INDEX = 0x0020  # HTree

EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER = 0x0001
EXT2_FEATURE_RO_COMPAT_LARGE_FILE = 0x0002
EXT2_FEATURE_RO_COMPAT_BTREE_DIR = 0x0004

EXT2_FEATURE_INCOMPAT_COMPRESSION = 0x0001
EXT2_FEATURE_INCOMPAT_FILETYPE = 0x0002
EXT2_FEATURE_INCOMPAT_RECOVER = 0x0004
EXT2_FEATURE_INCOMPAT_JOURNAL_DEV = 0x0008
EXT2_FEATURE_INCOMPAT_META_BG = 0x0010
EXT2_FEATURE_INCOMPAT_EXTENTS = 0x0040
EXT2_FEATURE_INCOMPAT_64BIT = 0x0080
EXT2_FEATURE_INCOMPAT_MMP = 0x0100
EXT2_FEATURE_INCOMPAT_FLEX_BG = 0x0200

# Inode types
EXT2_S_IFREG = 0x8000
EXT2_S_IFDIR = 0x4000
EXT2_S_IFLNK = 0xA000
EXT2_S_IRWXU = 0x0700
EXT2_S_IRUSR = 0x0100
EXT2_S_IWUSR = 0x0080
EXT2_S_IXUSR = 0x0040
EXT2_S_IRWXG = 0x0070
EXT2_S_IRWXO = 0x0007
EXT2_S_IFBLK = 0x6000
EXT2_S_IFCHR = 0x2000
EXT2_S_IFIFO = 0x1000
EXT2_S_IFSOCK = 0xC000

# File type values (for FILETYPE incompat)
EXT2_FT_UNKNOWN = 0
EXT2_FT_REG_FILE = 1
EXT2_FT_DIR = 2
EXT2_FT_CHRDEV = 3
EXT2_FT_BLKDEV = 4
EXT2_FT_FIFO = 5
EXT2_FT_SOCK = 6
EXT2_FT_SYMLINK = 7

FT_MAP = {
    EXT2_S_IFREG: EXT2_FT_REG_FILE,
    EXT2_S_IFDIR: EXT2_FT_DIR,
    EXT2_S_IFLNK: EXT2_FT_SYMLINK,
    EXT2_S_IFBLK: EXT2_FT_BLKDEV,
    EXT2_S_IFCHR: EXT2_FT_CHRDEV,
    EXT2_S_IFIFO: EXT2_FT_FIFO,
    EXT2_S_IFSOCK: EXT2_FT_SOCK,
}


def file_type(mode):
    return FT_MAP.get(mode & 0xF000, EXT2_FT_UNKNOWN)


# ── Sparse superblock helpers ──────────────────────────────────────

def group_has_super(sparse, group):
    if not sparse:
        return True
    if group <= 1:
        return True
    for p in (3, 5, 7):
        n = p
        while n <= group:
            if n == group:
                return True
            n *= p
    return False


def group_start_block(group, blocks_per_group):
    return group * blocks_per_group


# ── On-disk structures ────────────────────────────────────────────

def pack_superblock(inodes_count, blocks_count, r_blocks_count,
                    free_blocks_count, free_inodes_count,
                    first_data_block, log_block_size, log_frag_size,
                    blocks_per_group, frags_per_group, inodes_per_group,
                    mtime=0, wtime=0, mnt_count=0, max_mnt_count=0xFFFF,
                    magic=EXT2_SUPER_MAGIC, state=1, errors=1,
                    minor_rev_level=0, lastcheck=0, checkinterval=0,
                    creator_os=0, rev_level=EXT2_DYNAMIC_REV,
                    def_resuid=0, def_resgid=0,
                    first_ino=EXT2_GOOD_OLD_FIRST_INO,
                    inode_size=EXT2_GOOD_OLD_INODE_SIZE,
                    block_group_nr=0,
                    feature_compat=0, feature_incompat=0,
                    feature_ro_compat=0,
                    uuid=None, volume_name=b'', last_mounted=b'/',
                    algo_bitmap=0, last_orphan=0,
                    def_hash_version=1, def_hash_seed=(0, 0, 0, 0)):
    """Pack an ext2 superblock (1024 bytes at offset 1024)."""
    sb = bytearray(1024)
    struct.pack_into('<I', sb, 0, inodes_count)
    struct.pack_into('<I', sb, 4, blocks_count)
    struct.pack_into('<I', sb, 8, r_blocks_count)
    struct.pack_into('<I', sb, 12, free_blocks_count)
    struct.pack_into('<I', sb, 16, free_inodes_count)
    struct.pack_into('<I', sb, 20, first_data_block)
    struct.pack_into('<I', sb, 24, log_block_size)
    struct.pack_into('<I', sb, 28, log_frag_size)
    struct.pack_into('<I', sb, 32, blocks_per_group)
    struct.pack_into('<I', sb, 36, frags_per_group)
    struct.pack_into('<I', sb, 40, inodes_per_group)
    struct.pack_into('<I', sb, 44, mtime)
    struct.pack_into('<I', sb, 48, wtime)
    struct.pack_into('<H', sb, 52, mnt_count)
    struct.pack_into('<H', sb, 54, max_mnt_count)
    struct.pack_into('<H', sb, 56, magic)
    struct.pack_into('<H', sb, 58, state)
    struct.pack_into('<H', sb, 60, errors)
    struct.pack_into('<H', sb, 62, minor_rev_level)
    struct.pack_into('<I', sb, 64, lastcheck)
    struct.pack_into('<I', sb, 68, checkinterval)
    struct.pack_into('<I', sb, 72, creator_os)
    struct.pack_into('<I', sb, 76, rev_level)
    struct.pack_into('<H', sb, 80, def_resuid)
    struct.pack_into('<H', sb, 82, def_resgid)
    # Dynamic rev fields
    if rev_level == EXT2_DYNAMIC_REV:
        struct.pack_into('<I', sb, 84, first_ino)
        struct.pack_into('<H', sb, 88, inode_size)
        struct.pack_into('<H', sb, 90, block_group_nr)
        struct.pack_into('<I', sb, 92, feature_compat)
        struct.pack_into('<I', sb, 96, feature_incompat)
        struct.pack_into('<I', sb, 100, feature_ro_compat)
        if uuid:
            sb[104:120] = uuid[:16]
        else:
            sb[104:120] = b'\x00' * 16
        sb[120:136] = volume_name[:16].ljust(16, b'\x00')
        sb[136:200] = last_mounted[:64].ljust(64, b'\x00')
        struct.pack_into('<I', sb, 200, algo_bitmap)
        # Padding / reserved
        struct.pack_into('<I', sb, 204, last_orphan)
        struct.pack_into('<B', sb, 208, 0)  # s_def_hash_version — zero to avoid
        struct.pack_into('<B', sb, 209, 0)  # spurious journal UUID detection
        struct.pack_into('<H', sb, 210, 0)  # by e2fsprogs
        for i in range(4):
            struct.pack_into('<I', sb, 212 + i * 4, 0)  # hash_seed — all zero
        # Zero out journal backup inode info (ext2 has no journal)
        for i in range(17):
            struct.pack_into('<I', sb, 228 + i * 4, 0)
        # Zero rest of superblock to 1024
        for i in range(296, 1024, 4):
            struct.pack_into('<I', sb, i, 0)
    else:
        # Zero out dynamic rev area
        for i in range(84, 1024, 4):
            struct.pack_into('<I', sb, i, 0)
    return bytes(sb)


def pack_bg_desc(block_bitmap, inode_bitmap, inode_table,
                 free_blocks_count, free_inodes_count,
                 used_dirs_count, pad=0,
                 reserved=(0, 0, 0)):
    """Pack an ext2 block group descriptor (32 bytes)."""
    bgd = bytearray(32)
    struct.pack_into('<I', bgd, 0, block_bitmap)
    struct.pack_into('<I', bgd, 4, inode_bitmap)
    struct.pack_into('<I', bgd, 8, inode_table)
    struct.pack_into('<H', bgd, 12, free_blocks_count)
    struct.pack_into('<H', bgd, 14, free_inodes_count)
    struct.pack_into('<H', bgd, 16, used_dirs_count)
    struct.pack_into('<H', bgd, 18, pad)
    for i, v in enumerate(reserved):
        struct.pack_into('<I', bgd, 20 + i * 4, v)
    return bytes(bgd)


def pack_inode(mode=0, uid=0, size=0, atime=0, ctime=0, mtime=0,
               dtime=0, gid=0, links_count=0, blocks=0, flags=0,
               osd1=0, block=(), generation=0, file_acl=0, dir_acl=0,
               faddr=0, osd2=()):
    """Pack an ext2 inode (128 bytes)."""
    ino = bytearray(128)
    struct.pack_into('<H', ino, 0, mode)
    struct.pack_into('<H', ino, 2, uid)
    struct.pack_into('<I', ino, 4, size)
    struct.pack_into('<I', ino, 8, atime)
    struct.pack_into('<I', ino, 12, ctime)
    struct.pack_into('<I', ino, 16, mtime)
    struct.pack_into('<I', ino, 20, dtime)
    struct.pack_into('<H', ino, 24, gid)
    struct.pack_into('<H', ino, 26, links_count)
    struct.pack_into('<I', ino, 28, blocks)
    struct.pack_into('<I', ino, 32, flags)
    struct.pack_into('<I', ino, 36, osd1)
    # i_block[15]
    blk = list(block) + [0] * (15 - len(block))
    for i in range(15):
        struct.pack_into('<I', ino, 40 + i * 4, blk[i])
    struct.pack_into('<I', ino, 100, generation)
    struct.pack_into('<I', ino, 104, file_acl)
    struct.pack_into('<I', ino, 108, dir_acl)
    struct.pack_into('<I', ino, 112, faddr)
    osd2 = list(osd2) + [0] * (12 - len(osd2))
    for i in range(12):
        ino[116 + i] = osd2[i] & 0xFF
    return bytes(ino)


def pack_dirent(inode, rec_len, name_len, name, file_type=EXT2_FT_UNKNOWN):
    """Pack an ext2 directory entry (variable length)."""
    entry = bytearray(rec_len)
    struct.pack_into('<I', entry, 0, inode)
    struct.pack_into('<H', entry, 4, rec_len)
    actual_name_len = min(name_len, len(name))
    struct.pack_into('<B', entry, 6, actual_name_len)
    if file_type:
        struct.pack_into('<B', entry, 7, file_type)
    else:
        entry[7] = 0
    name_bytes = name.encode('latin-1')[:actual_name_len]
    entry[8:8 + actual_name_len] = name_bytes
    return bytes(entry)


# ── Filesystem builder ─────────────────────────────────────────────

class Ext2Builder:
    """Build an ext2 filesystem image."""

    def __init__(self, size_mb, block_size=1024, inode_size=128,
                 inodes_per_group=None, blocks_per_group=None,
                 features=None):
        self.block_size = block_size
        self.inode_size = inode_size
        self.log_block_size = int(math.log2(block_size // 1024))
        self.log_frag_size = self.log_block_size

        total_sectors = (size_mb * 1024 * 1024) // SECTOR_SIZE
        self.part_start = 2048  # LBA of partition start
        self.total_blocks = (total_sectors - self.part_start) * SECTOR_SIZE // block_size

        # Default group geometry
        if blocks_per_group is None:
            self.blocks_per_group = 8 * block_size // 1024  # 8 * 1024-byte units
            if self.blocks_per_group > 8192:
                blocks_per_group = 8192
        self.blocks_per_group = blocks_per_group or 8192

        if inodes_per_group is None:
            self.inodes_per_group = max(16, self.blocks_per_group // 4)

        self.inodes_per_group = min(inodes_per_group or self.inodes_per_group, self.blocks_per_group)

        if self.inodes_per_group == 0:
            self.inodes_per_group = 16

        self.num_groups = (self.total_blocks + self.blocks_per_group - 1) // self.blocks_per_group
        self.total_blocks = self.num_groups * self.blocks_per_group

        # Parse features
        self.features = set()
        if features:
            for f in features.split(','):
                self.features.add(f.strip().lower())

        self.feature_compat = 0
        self.feature_incompat = 0
        self.feature_ro_compat = 0
        self.has_filetype = 'filetype' in self.features
        self.sparse = 'sparse' in self.features
        self.has_htree = 'htree' in self.features
        self.has_ext_attr = 'ext_attr' in self.features
        self.has_acl = 'acl' in self.features
        self.has_resize = 'resize' in self.features
        self.has_largefile = 'largefile' in self.features

        if self.has_filetype:
            self.feature_incompat |= EXT2_FEATURE_INCOMPAT_FILETYPE
        if self.sparse:
            self.feature_ro_compat |= EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER
        if self.has_largefile:
            self.feature_ro_compat |= EXT2_FEATURE_RO_COMPAT_LARGE_FILE
        if self.has_htree:
            self.feature_compat |= EXT2_FEATURE_COMPAT_DIR_INDEX
        if self.has_ext_attr:
            self.feature_compat |= EXT2_FEATURE_COMPAT_EXT_ATTR
        if self.has_resize:
            self.feature_compat |= EXT2_FEATURE_COMPAT_RESIZE_INO

        # Compute layout
        self.group_layouts = []
        for g in range(self.num_groups):
            layout = self._compute_group_layout(g)
            self.group_layouts.append(layout)

        self.sb_block = None
        self.sb_offset = None
        self.bgd_blocks_per_group = {}
        self._compute_all()

        self.fs_blocks = bytearray()  # raw block data
        self.next_inode = EXT2_ROOT_INO + 1
        self.allocated_data_blocks = set()
        self.block_start = 0

    def _compute_group_layout(self, group):
        """Compute metadata layout for a block group."""
        has_super = group_has_super(self.sparse, group)

        if has_super:
            if self.block_size == 1024:
                sb_blocks = 2  # block 0 (boot) + block 1 (superblock)
            else:
                sb_blocks = 1
            bgd_blocks = max(1, (self.num_groups * 32 + self.block_size - 1) // self.block_size)
        else:
            sb_blocks = 0
            bgd_blocks = 0

        bitmaps_blocks = 2  # block bitmap + inode bitmap
        inode_table_blocks = max(1,
            (self.inodes_per_group * self.inode_size + self.block_size - 1) // self.block_size)

        metadata_blocks = sb_blocks + bgd_blocks + bitmaps_blocks + inode_table_blocks
        if metadata_blocks > self.blocks_per_group:
            metadata_blocks = self.blocks_per_group

        data_blocks = self.blocks_per_group - metadata_blocks

        block_bitmap_block = sb_blocks + bgd_blocks
        inode_bitmap_block = block_bitmap_block + 1
        inode_table_block = inode_bitmap_block + 1

        return {
            'has_super': has_super,
            'sb_blocks': sb_blocks,
            'bgd_blocks': bgd_blocks,
            'metadata_blocks': metadata_blocks,
            'data_blocks': data_blocks,
            'block_bitmap': block_bitmap_block,
            'inode_bitmap': inode_bitmap_block,
            'inode_table': inode_table_block,
            'inode_table_blocks': inode_table_blocks,
        }

    def _compute_all(self):
        """Compute global offsets."""
        # Each group occupies self.blocks_per_group blocks
        # The first group starts at block 0 within the partition
        # Superblock is at byte offset 1024 from partition start
        self.sb_block = 0
        self.sb_offset = 1024

        # BGD table is at block after superblock (block 1 for 1024, block 1 for larger)
        # Actually: superblock is in block 0 at offset 1024 (for 1024 block size)
        # BGD starts right after the superblock (at offset 2048 if block_size=1024)
        bgd_block_num = (self.block_size + 1024) // self.block_size  # block containing BGD start
        self.bgd_block = bgd_block_num

        # BGD spans bgd_blocks across first group
        self.bgd_block_count = self.group_layouts[0]['bgd_blocks'] if self.group_layouts else 1

    def get_group_offset(self, group):
        """Get byte offset of a block group's first block from partition start."""
        return group * self.blocks_per_group * self.block_size

    def get_block_offset(self, block_num):
        """Get byte offset of a block number from partition start."""
        return block_num * self.block_size

    def bitmap_set(self, bitmap, bit_num):
        """Set a bit in a bitmap (mark as used/in use)."""
        byte_idx = bit_num // 8
        bit_idx = bit_num % 8
        bitmap[byte_idx] &= ~(1 << bit_idx)

    def bitmap_clear(self, bitmap, bit_num):
        """Clear a bit in a bitmap (mark as free)."""
        byte_idx = bit_num // 8
        bit_idx = bit_num % 8
        bitmap[byte_idx] |= (1 << bit_idx)

    def allocate_blocks(self, count, group=None):
        """Allocate 'count' data blocks and return list of block numbers."""
        blocks = []
        for _ in range(count):
            # Simple linear allocation: start after all metadata
            for g in range(self.num_groups):
                if group is not None and g != group:
                    continue
                layout = self.group_layouts[g]
                gstart = g * self.blocks_per_group
                data_start = gstart + layout['metadata_blocks']
                data_end = gstart + self.blocks_per_group
                for b in range(data_start, data_end):
                    if b not in self.allocated_data_blocks:
                        self.allocated_data_blocks.add(b)
                        blocks.append(b)
                        break
                if len(blocks) == count:
                    break
            if len(blocks) == count:
                break
        return blocks

    def allocate_inode(self):
        """Allocate an inode number."""
        ino = self.next_inode
        self.next_inode += 1
        return ino

    def _get_blocks_for_group(self, group):
        """Get all block numbers belonging to a group."""
        return range(group * self.blocks_per_group,
                     (group + 1) * self.blocks_per_group)

    def _get_data_blocks(self, group):
        """Get data block numbers for a group."""
        gstart = group * self.blocks_per_group
        layout = self.group_layouts[group]
        data_start = gstart + layout['metadata_blocks']
        data_end = gstart + self.blocks_per_group
        return range(data_start, data_end)

    def write_image(self, output_path):
        """Write the complete ext2 filesystem image."""
        total_blocks = self.num_groups * self.blocks_per_group
        total_bytes = total_blocks * self.block_size

        img = bytearray(total_bytes)

        part_start_bytes = self.part_start * SECTOR_SIZE
        # The partition starts at part_start_bytes into the disk
        # but for simplicity, the filesystem data goes at offset 0 in the partition
        # and we'll prepend the MBR later

        # Actually, the whole approach: write the MBR at the start of the image
        # and the filesystem starts at part_start_bytes

        full_img_size = total_bytes + part_start_bytes
        full_img = bytearray(full_img_size)

        # MBR
        mbr = self._make_mbr()
        full_img[:512] = mbr

        # Write filesystem data
        fs_data = self._build_fs()
        full_img[part_start_bytes:part_start_bytes + len(fs_data)] = fs_data

        with open(output_path, 'wb') as f:
            f.write(full_img)

        print(f"[mkext2img] Created {output_path}: "
              f"{len(full_img) // (1024*1024)} MB, "
              f"{self.num_groups} block groups, "
              f"{self.blocks_per_group} blocks/group, "
              f"{self.block_size} B/blocks")

    def _make_mbr(self):
        """Create MBR with one Linux partition."""
        total_sectors = (self.num_groups * self.blocks_per_group *
                         self.block_size) // SECTOR_SIZE
        part_size_sectors = total_sectors

        mbr = bytearray(512)
        # Partition entry 1
        off = 0x1BE
        mbr[off] = 0x80  # bootable
        mbr[off + 1] = 0x00  # CHS head start
        mbr[off + 2] = 0x01  # CHS sector start
        mbr[off + 3] = 0x00  # CHS cylinder start
        mbr[off + 4] = 0x83  # partition type: Linux
        mbr[off + 5] = 0xFE  # CHS head end
        mbr[off + 6] = 0xFF  # CHS sector end
        mbr[off + 7] = 0xFF  # CHS cylinder end
        struct.pack_into('<I', mbr, off + 8, self.part_start)
        struct.pack_into('<I', mbr, off + 12, part_size_sectors)
        mbr[510] = 0x55
        mbr[511] = 0xAA
        return bytes(mbr)

    def _build_fs(self):
        """Build the raw ext2 filesystem data."""
        num_groups = self.num_groups
        blocks_per_group = self.blocks_per_group
        block_size = self.block_size
        inode_size = self.inode_size
        inodes_per_group = self.inodes_per_group

        total_blocks = num_groups * blocks_per_group
        fs = bytearray(total_blocks * block_size)

        # Generate UUID
        uuid = os.urandom(16)

        total_inodes = num_groups * inodes_per_group

        # Pre-allocate reserved inodes (0-10)
        self.used_inodes = set([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
        # Don't clobber inode_data if a subclass (Ext2Populator) already set it up
        if not hasattr(self, 'inode_data') or not self.inode_data:
            self.inode_data = {}  # ino -> bytes

        # ── Build each group ─────────────────────────────────────
        bgd_entries = []

        for g in range(num_groups):
            gl = self.group_layouts[g]
            gstart_block = g * blocks_per_group
            gstart_byte = gstart_block * block_size

            # Block bitmap
            bmap = bytearray(block_size)
            # Mark all blocks used initially, then clear data blocks
            for i in range(block_size):
                bmap[i] = 0x00  # all used

            # Mark metadata blocks as used (already 0x00 = used)
            # Mark data blocks as free (0xFF = free)
            for b in range(gl['metadata_blocks'], blocks_per_group):
                byte_idx = b // 8
                bit_idx = b % 8
                bmap[byte_idx] |= (1 << bit_idx)

            # Inode bitmap
            imap = bytearray(block_size)
            all_inodes_in_group = inodes_per_group
            for i in range(block_size):
                imap[i] = 0xFF  # all free

            # Mark reserved inodes as used
            first_ino_in_group = g * inodes_per_group + 1
            for r in range(1, 11):
                ino_for_group = g * inodes_per_group + r
                if ino_for_group <= total_inodes and ino_for_group >= first_ino_in_group:
                    idx = (ino_for_group - 1) % inodes_per_group
                    imap[idx // 8] &= ~(1 << (idx % 8))

            # Write block bitmap
            bmap_block = gstart_block + gl['block_bitmap']
            bmap_offset = bmap_block * block_size
            fs[bmap_offset:bmap_offset + block_size] = bmap

            # Write inode bitmap
            imap_block = gstart_block + gl['inode_bitmap']
            imap_offset = imap_block * block_size
            fs[imap_offset:imap_offset + block_size] = imap

            # Write inode table (zeros)
            itable_block = gstart_block + gl['inode_table']
            itable_start = itable_block * block_size
            itable_size = inodes_per_group * inode_size
            # Already zeroed

            # Populate with reserved inodes
            for r in range(1, 11):
                ino = g * inodes_per_group + r
                if ino > total_inodes:
                    continue
                off = itable_start + (ino - 1) % inodes_per_group * inode_size
                if ino == EXT2_ROOT_INO:
                    # Root inode will be set up later
                    pass
                else:
                    # Write zero inode (unused)
                    pass

            # Build BGD
            free_blocks = gl['data_blocks']
            free_inodes = inodes_per_group - 10  # minus reserved
            if g == 0:
                # Some inodes used beyond reserved
                pass

            used_dirs = 0
            bgd = pack_bg_desc(
                block_bitmap=bmap_block,
                inode_bitmap=imap_block,
                inode_table=itable_block,
                free_blocks_count=free_blocks & 0xFFFF,
                free_inodes_count=free_inodes & 0xFFFF,
                used_dirs_count=used_dirs
            )
            bgd_entries.append(bgd)

        # Write superblock and BGD in first group
        # Superblock at byte offset 1024 from partition start
        sb_start = 1024  # from partition start (block 0 offset 1024)

        # Compute free counts
        free_blocks_count = sum(gl['data_blocks'] for gl in self.group_layouts)
        free_inodes_count = num_groups * inodes_per_group - 10

        sb = pack_superblock(
            inodes_count=total_inodes,
            blocks_count=total_blocks,
            r_blocks_count=0,
            free_blocks_count=free_blocks_count,
            free_inodes_count=free_inodes_count,
            first_data_block=0,
            log_block_size=self.log_block_size,
            log_frag_size=self.log_frag_size,
            blocks_per_group=blocks_per_group,
            frags_per_group=blocks_per_group,
            inodes_per_group=inodes_per_group,
            creator_os=0,
            rev_level=EXT2_DYNAMIC_REV,
            first_ino=EXT2_GOOD_OLD_FIRST_INO,
            inode_size=inode_size,
            feature_compat=self.feature_compat,
            feature_incompat=self.feature_incompat,
            feature_ro_compat=self.feature_ro_compat,
            uuid=uuid,
            volume_name=b'test_volume',
        )
        fs[sb_start:sb_start + len(sb)] = sb

        # Write BGD table in group 0
        # For 1024 block size: super is in block 1 (offset 1024), BGD starts at offset 2048 (block 2)
        if self.block_size == 1024:
            bgd_offset = 2048
        else:
            bgd_offset = self.block_size + 1024  # BGD after super in first data block

        bgd_data = b''.join(bgd_entries)
        # Pad to block boundary
        bgd_padded = bgd_data + b'\x00' * (block_size - len(bgd_data) % block_size) if len(bgd_data) % block_size else bgd_data
        fs[bgd_offset:bgd_offset + len(bgd_data)] = bgd_data

        # Copy BGD to backup superblock groups
        for g in range(1, num_groups):
            if group_has_super(self.sparse, g):
                gl = self.group_layouts[g]
                gstart = g * blocks_per_group
                # BGD backup location
                if self.block_size == 1024:
                    bgd_backup_offset = (gstart + 2) * block_size
                else:
                    bgd_backup_offset = (gstart + 1) * block_size
                bgd_needed = bgd_data[:min(len(bgd_data), block_size * gl['bgd_blocks'])]
                fs[bgd_backup_offset:bgd_backup_offset + len(bgd_needed)] = bgd_needed

        # ── Now return the fs, we'll set up root dir and files in a subclass ──
        # Actually, embed it directly
        self.fs_data = fs
        self.bgd_entries = bgd_entries

        return fs


class Ext2Populator(Ext2Builder):
    """Ext2 builder with file/directory population."""

    def __init__(self, size_mb, block_size=1024, features=None):
        super().__init__(size_mb, block_size=block_size, features=features)
        self.dirs = {}  # path -> inode number
        self.inode_data = {}  # ino -> inode bytes (128 bytes)
        self.dir_entries = {}  # ino -> list of (name, target_ino, mode)
        self.inode_blocks = {}  # ino -> list of data block numbers
        self.allocated_blocks = set()  # global set of allocated block numbers
        self.next_block = None  # set after init

    def _allocate_block(self):
        """Find the next free data block."""
        total_blocks = self.num_groups * self.blocks_per_group
        for g in range(self.num_groups):
            gl = self.group_layouts[g]
            gstart = g * self.blocks_per_group
            data_start = gstart + gl['metadata_blocks']
            data_end = gstart + self.blocks_per_group
            for b in range(data_start, data_end):
                if b not in self.allocated_blocks:
                    self.allocated_blocks.add(b)
                    return b
        raise RuntimeError("No free blocks")

    def _allocate_inode(self):
        ino = self.next_inode
        self.next_inode += 1
        if ino >= self.num_groups * self.inodes_per_group:
            raise RuntimeError("No free inodes")
        return ino

    def _write_inode(self, ino, inode_bytes):
        self.inode_data[ino] = inode_bytes

    def make_dir(self, path, parent_ino=EXT2_ROOT_INO, mode=0o755):
        """Create a directory and return its inode."""
        if path in self.dirs:
            return self.dirs[path]

        ino = self._allocate_inode()
        self.dirs[path] = ino

        blocks = 0
        data_blocks = []
        # Allocate one block for the directory contents
        b = self._allocate_block()
        data_blocks.append(b)
        blocks = (len(data_blocks) * self.block_size + 511) // 512

        imode = EXT2_S_IFDIR | mode
        inode = pack_inode(
            mode=imode,
            uid=0,
            size=self.block_size,
            links_count=2,
            blocks=blocks,
            block=data_blocks[:12]
        )
        self._write_inode(ino, inode)

        # Add to parent directory
        name = os.path.basename(path)
        self.add_to_dir(parent_ino, ino, name, imode)

        # Create "." and ".." entries
        self.dir_entries[ino] = []
        self.add_to_dir(ino, ino, ".", imode)
        self.add_to_dir(ino, parent_ino, "..", EXT2_S_IFDIR | 0o755)

        return ino

    def make_file(self, path, content=b'', parent_ino=EXT2_ROOT_INO, mode=0o644):
        """Create a regular file with content."""
        name = os.path.basename(path)
        if not content:
            content = b''

        ino = self._allocate_inode()
        data_blocks = []
        blocks_512 = 0

        # Allocate blocks for content
        remaining = len(content)
        offset = 0
        while remaining > 0:
            b = self._allocate_block()
            data_blocks.append(b)
            self.content_blocks = getattr(self, 'content_blocks', {})
            if not hasattr(self, 'content_map'):
                self.content_map = {}
            self.content_map[b] = content[offset:offset + self.block_size]
            offset += self.block_size
            remaining -= self.block_size
            blocks_512 += self.block_size // 512

        imode = EXT2_S_IFREG | mode
        inode = pack_inode(
            mode=imode,
            uid=0,
            size=len(content),
            links_count=1,
            blocks=blocks_512,
            block=data_blocks[:12]
        )
        self._write_inode(ino, inode)

        self.add_to_dir(parent_ino, ino, name, imode)
        return ino

    def make_symlink(self, path, target, parent_ino=EXT2_ROOT_INO):
        """Create a symlink."""
        name = os.path.basename(path)
        ino = self._allocate_inode()
        target_bytes = target.encode('utf-8')
        blocks_512 = 0
        data_blocks = []

        imode = EXT2_S_IFLNK | 0o777

        if len(target_bytes) <= 60:
            # Fast symlink: store target in i_block[]
            # Use the block array as inline data
            block_array = [0] * 15
            for i in range(min(len(target_bytes) // 4 + 1, 15)):
                chunk = target_bytes[i*4:(i+1)*4]
                val = int.from_bytes(chunk.ljust(4, b'\x00'), 'little')
                block_array[i] = val
            inode = pack_inode(
                mode=imode,
                uid=0,
                size=len(target_bytes),
                links_count=1,
                blocks=0,
                block=tuple(block_array)
            )
        else:
            # Slow symlink: target stored in a data block
            b = self._allocate_block()
            data_blocks.append(b)
            if not hasattr(self, 'content_map'):
                self.content_map = {}
            self.content_map[b] = target_bytes
            blocks_512 = self.block_size // 512
            inode = pack_inode(
                mode=imode,
                uid=0,
                size=len(target_bytes),
                links_count=1,
                blocks=blocks_512,
                block=tuple(data_blocks[:12])
            )
        self._write_inode(ino, inode)
        self.add_to_dir(parent_ino, ino, name, imode)
        return ino

    def make_hardlink(self, target_path, link_path, parent_ino=EXT2_ROOT_INO):
        """Create a hard link (additional dir entry to same inode)."""
        ino = self.dirs.get(target_path)
        if ino is None:
            # Check if it's a file
            raise ValueError(f"Target {target_path} not found")

        name = os.path.basename(link_path)
        # Increment link count
        orig = self.inode_data[ino]
        _, links = struct.unpack_from('<HI', orig, 26)
        packed = bytearray(orig)
        struct.pack_into('<H', packed, 26, links + 1)
        self.inode_data[ino] = bytes(packed)

        # Get mode
        mode = struct.unpack_from('<H', orig, 0)[0]
        self.add_to_dir(parent_ino, ino, name, mode)
        return ino

    def make_sparse_file(self, path, size, parent_ino=EXT2_ROOT_INO, mode=0o644):
        """Create a sparse file with holes. Only allocates a few blocks."""
        name = os.path.basename(path)
        ino = self._allocate_inode()

        # For a sparse file, we allocate blocks at the start and end
        # The middle is holes (zero block pointers)
        data_blocks = []
        # Allocate first block
        b1 = self._allocate_block()
        data_blocks.append(b1)
        if not hasattr(self, 'content_map'):
            self.content_map = {}
        self.content_map[b1] = b'START' + b'\x00' * (self.block_size - 5)

        # Allocate a block at a far offset (simulate tail data)
        b2 = self._allocate_block()
        data_blocks.append(b2)
        self.content_map[b2] = b'END' + b'\x00' * (self.block_size - 3)

        # The single indirect block pointer
        iblock = self._allocate_block()
        # Don't fill indirect block - just mark it as allocated

        # Construct i_block with holes in between
        block_array = [0] * 15
        block_array[0] = data_blocks[0]  # direct 0
        block_array[11] = data_blocks[1]  # direct 11
        # Single indirect
        block_array[12] = iblock

        blocks_512 = 2 * self.block_size // 512  # only 2 allocated data blocks
        # plus 1 indirect block
        blocks_512 += self.block_size // 512

        imode = EXT2_S_IFREG | mode
        inode = pack_inode(
            mode=imode,
            uid=0,
            size=size,
            links_count=1,
            blocks=blocks_512,
            block=tuple(block_array),
            flags=0x0000
        )
        self._write_inode(ino, inode)
        self.add_to_dir(parent_ino, ino, name, imode)
        return ino

    def add_to_dir(self, dir_ino, target_ino, name, target_mode):
        """Add a directory entry."""
        if dir_ino not in self.dir_entries:
            self.dir_entries[dir_ino] = []
        self.dir_entries[dir_ino].append((name, target_ino, target_mode))

    def _make_dirent_data(self, entries, filetype_ext2=True):
        """Pack directory entries into block(s)."""
        # Sort: . and .. first, then alphabetical
        sorted_ents = sorted(entries, key=lambda e: (e[0] != '.', e[0] != '..', e[0]))
        blocks_data = []
        block = bytearray(self.block_size)
        offset = 0
        sorted_len = len(sorted_ents)

        for idx, (name, ino, mode) in enumerate(sorted_ents):
            name_bytes = name.encode('latin-1')
            name_len = len(name_bytes)
            ft = file_type(mode) if filetype_ext2 else EXT2_FT_UNKNOWN
            # Entry overhead: 8 bytes header + name
            entry_size = 8 + name_len
            # Round up to 4
            entry_size = (entry_size + 3) & ~3

            # Check if this entry fits in current block
            if offset + entry_size > self.block_size:
                # Pad remaining and start new block
                blocks_data.append(bytes(block))
                block = bytearray(self.block_size)
                offset = 0

            is_last = (idx == sorted_len - 1)

            rec_len = entry_size
            if is_last:
                rec_len = self.block_size - offset

            struct.pack_into('<I', block, offset, ino)
            struct.pack_into('<H', block, offset + 4, rec_len)
            struct.pack_into('<B', block, offset + 6, name_len)
            struct.pack_into('<B', block, offset + 7, ft)
            block[offset + 8:offset + 8 + name_len] = name_bytes
            offset += rec_len

        blocks_data.append(bytes(block))
        # Filter None
        blocks_data = [b for b in blocks_data if b is not None]
        return blocks_data

    def finalize(self):
        """Write all inodes and directory data into the filesystem image."""
        # Get fs_data from parent
        if not hasattr(self, 'fs_data'):
            # Build fresh
            total_blocks = self.num_groups * self.blocks_per_group
            total_bytes = total_blocks * self.block_size
            self.fs_data = self._build_fs()

        # Write inodes
        for ino, inode_bytes in self.inode_data.items():
            g = (ino - 1) // self.inodes_per_group
            idx = (ino - 1) % self.inodes_per_group
            gl = self.group_layouts[g]
            gstart = g * self.blocks_per_group
            inode_table_block = gstart + gl['inode_table']
            inode_offset = inode_table_block * self.block_size + idx * self.inode_size
            if inode_offset + self.inode_size <= len(self.fs_data):
                self.fs_data[inode_offset:inode_offset + self.inode_size] = inode_bytes

        # Write directory data
        for dir_ino, entries in self.dir_entries.items():
            blocks_data = self._make_dirent_data(entries, filetype_ext2=self.has_filetype)
            # Get data block pointers from inode
            g = (dir_ino - 1) // self.inodes_per_group
            idx = (dir_ino - 1) % self.inodes_per_group
            gl = self.group_layouts[g]
            gstart = g * self.blocks_per_group
            inode_table_block = gstart + gl['inode_table']
            inode_offset = inode_table_block * self.block_size + idx * self.inode_size

            # Read block pointers
            blocks = []
            for i in range(12):
                blk = struct.unpack_from('<I', self.fs_data, inode_offset + 40 + i * 4)[0]
                if blk != 0:
                    blocks.append(blk)

            # Write directory data to those blocks
            for i, blk in enumerate(blocks):
                blk_offset = blk * self.block_size
                if i < len(blocks_data):
                    self.fs_data[blk_offset:blk_offset + len(blocks_data[i])] = blocks_data[i]

        # Write content blocks
        if hasattr(self, 'content_map'):
            for blk, content in self.content_map.items():
                blk_offset = blk * self.block_size
                data = content.ljust(self.block_size, b'\x00')[:self.block_size]
                self.fs_data[blk_offset:blk_offset + self.block_size] = data

        # Update BGD entries with final counts
        for g in range(self.num_groups):
            gl = self.group_layouts[g]
            gstart = g * self.blocks_per_group

            # Recalculate used inodes and blocks
            used_inodes_count = 0
            used_blocks_count = 0

            # Count allocated inodes in this group
            first_ino = g * self.inodes_per_group + 1
            last_ino = (g + 1) * self.inodes_per_group
            for ino in range(first_ino, last_ino + 1):
                if ino in self.inode_data:
                    used_inodes_count += 1

            # Count allocated data blocks in this group
            for blk in self.allocated_blocks:
                if g * self.blocks_per_group <= blk < (g + 1) * self.blocks_per_group:
                    if blk >= gstart + gl['metadata_blocks']:
                        used_blocks_count += 1

            # Update BGD offsets in the fs image
            # BGD table is in group 0 and backup groups
            bgd_byte_size = 32
            for bg in range(self.num_groups):
                if not group_has_super(self.sparse, bg):
                    continue
                bgl = self.group_layouts[bg]
                bgstart = bg * self.blocks_per_group
                if self.block_size == 1024:
                    bgd_offset_in_group = 2 * self.block_size
                else:
                    bgd_offset_in_group = (self.block_size + 1024)  # super + 1024
                bgd_abs_offset = bgstart * self.block_size + bgd_offset_in_group

                # Update free blocks count
                free_blocks = gl['data_blocks'] - used_blocks_count
                if free_blocks < 0:
                    free_blocks = 0
                struct.pack_into('<H', self.fs_data, bgd_abs_offset + g * 32 + 12,
                                 free_blocks & 0xFFFF)

                # Update free inodes count
                # Account for reserved inodes (1-10) that may not be in inode_data
                reserved_in_group = 0
                for r in range(1, 11):
                    ino_for_group = g * self.inodes_per_group + r
                    if ino_for_group <= self.num_groups * self.inodes_per_group:
                        if ino_for_group not in self.inode_data:
                            reserved_in_group += 1
                free_inodes = self.inodes_per_group - used_inodes_count - reserved_in_group
                struct.pack_into('<H', self.fs_data, bgd_abs_offset + g * 32 + 14,
                                 free_inodes & 0xFFFF)

                # Update used dirs count
                used_dirs = sum(1 for e in self.dir_entries.values()
                              for _, _, m in e if m & EXT2_S_IFDIR)
                # Actually count dirs in this group
                dirs_in_group = 0
                for ino, inode_bytes in self.inode_data.items():
                    if ino // self.inodes_per_group != g:
                        continue
                    mode = struct.unpack_from('<H', inode_bytes, 0)[0]
                    if mode & EXT2_S_IFDIR:
                        dirs_in_group += 1
                struct.pack_into('<H', self.fs_data, bgd_abs_offset + g * 32 + 16,
                                 dirs_in_group & 0xFFFF)

        # Update superblock free counts
        total_free_blocks = sum(
            gl['data_blocks'] - sum(1 for blk in self.allocated_blocks
                                     if g * self.blocks_per_group <= blk < (g + 1) * self.blocks_per_group
                                     and blk >= g * self.blocks_per_group + gl['metadata_blocks'])
            for g, gl in enumerate(self.group_layouts)
        )
        total_free_inodes = 0
        for g in range(self.num_groups):
            used = sum(1 for ino in self.inode_data
                      if (ino - 1) // self.inodes_per_group == g)
            # Account for reserved inodes (1-10) not in inode_data
            reserved_extra = 0
            for r in range(1, 11):
                ino_for_group = g * self.inodes_per_group + r
                if ino_for_group <= self.num_groups * self.inodes_per_group:
                    if ino_for_group not in self.inode_data:
                        reserved_extra += 1
            total_free_inodes += self.inodes_per_group - used - reserved_extra

        struct.pack_into('<I', self.fs_data, 1024 + 12, total_free_blocks)
        struct.pack_into('<I', self.fs_data, 1024 + 16, total_free_inodes)

        # Update mount count
        mnt_count = struct.unpack_from('<H', self.fs_data, 1024 + 52)[0]
        struct.pack_into('<H', self.fs_data, 1024 + 52, mnt_count + 1)

        # ── Update block bitmaps to reflect allocated blocks ─────
        for g in range(self.num_groups):
            gl = self.group_layouts[g]
            gstart = g * self.blocks_per_group

            # Read block bitmap for this group from the image
            bmap_block = gstart + gl['block_bitmap']
            bmap_offset = bmap_block * self.block_size
            bmap = bytearray(self.fs_data[bmap_offset:bmap_offset + self.block_size])

            # Mark allocated data blocks in this group as used
            for blk in self.allocated_blocks:
                if g * self.blocks_per_group <= blk < (g + 1) * self.blocks_per_group:
                    bit_in_group = blk % self.blocks_per_group
                    byte_idx = bit_in_group // 8
                    bit_idx = bit_in_group % 8
                    bmap[byte_idx] &= ~(1 << bit_idx)

            # Write block bitmap back
            self.fs_data[bmap_offset:bmap_offset + self.block_size] = bmap

            # Read inode bitmap for this group
            imap_block = gstart + gl['inode_bitmap']
            imap_offset = imap_block * self.block_size
            imap = bytearray(self.fs_data[imap_offset:imap_offset + self.block_size])

            # Mark allocated inodes in this group as used
            first_ino_in_group = g * self.inodes_per_group + 1
            last_ino_in_group = (g + 1) * self.inodes_per_group
            for ino in self.inode_data:
                if first_ino_in_group <= ino <= last_ino_in_group:
                    idx = (ino - 1) % self.inodes_per_group
                    imap[idx // 8] &= ~(1 << (idx % 8))

            # Write inode bitmap back
            self.fs_data[imap_offset:imap_offset + self.block_size] = imap

        # ── Write inode data (already written above, but ensure all inode data is there) ──
        # ── This also needs to be done BEFORE bitmap update for consistency ──
        # (Already done at the start of finalize)

        # Write state = clean (1)
        struct.pack_into('<H', self.fs_data, 1024 + 58, 1)

        return self.fs_data

    def write_image(self, output_path):
        """Write the complete ext2 image with populated content."""
        fs_data = self.finalize()

        part_start_bytes = self.part_start * SECTOR_SIZE
        full_img_size = len(fs_data) + part_start_bytes
        full_img = bytearray(full_img_size)

        # MBR
        mbr = self._make_mbr()
        full_img[:512] = mbr

        full_img[part_start_bytes:part_start_bytes + len(fs_data)] = fs_data

        with open(output_path, 'wb') as f:
            f.write(full_img)

        print(f"[mkext2img] Created {output_path}: "
              f"{len(full_img) // (1024*1024)} MB, "
              f"{self.num_groups} block groups, "
              f"{self.block_size} B/blocks")


def main():
    import sys
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    output = sys.argv[1]
    size_mb = int(sys.argv[2])
    features = 'sparse,filetype'
    populate_dir = None

    for i, arg in enumerate(sys.argv[3:], 3):
        if arg == '--features' and i + 1 < len(sys.argv):
            features = sys.argv[i + 1]
        elif arg == '--populate' and i + 1 < len(sys.argv):
            populate_dir = sys.argv[i + 1]

    builder = Ext2Populator(size_mb, block_size=1024, features=features)

    # Create root directory entries
    builder.dir_entries[EXT2_ROOT_INO] = []
    builder.dirs['/'] = EXT2_ROOT_INO

    # Create root inode
    root_inode = pack_inode(
        mode=EXT2_S_IFDIR | 0o755,
        uid=0,
        size=1024,
        links_count=3,
        blocks=2,
        block=(builder._allocate_block(),)
    )
    builder._write_inode(EXT2_ROOT_INO, root_inode)

    # Add . and .. to root
    builder.add_to_dir(EXT2_ROOT_INO, EXT2_ROOT_INO, ".", EXT2_S_IFDIR | 0o755)
    builder.add_to_dir(EXT2_ROOT_INO, EXT2_ROOT_INO, "..", EXT2_S_IFDIR | 0o755)

    # Create /bin directory
    builder.make_dir('/bin', EXT2_ROOT_INO, 0o755)
    builder.make_dir('/etc', EXT2_ROOT_INO, 0o755)
    builder.make_dir('/usr', EXT2_ROOT_INO, 0o755)
    builder.make_dir('/tmp', EXT2_ROOT_INO, 0o777)
    builder.make_dir('/var', EXT2_ROOT_INO, 0o755)
    builder.make_dir('/mnt', EXT2_ROOT_INO, 0o755)

    # Create test files
    builder.make_file('/etc/hostname', b'os-test\n', EXT2_ROOT_INO, 0o644)
    builder.make_file('/etc/hosts', b'127.0.0.1 localhost\n', EXT2_ROOT_INO, 0o644)
    builder.make_file('/tmp/test.txt', b'Hello, ext2!\n', EXT2_ROOT_INO, 0o644)

    # Create symlinks
    builder.make_symlink('/tmp/link_to_test', '/tmp/test.txt', EXT2_ROOT_INO)

    # Create a sparse file
    builder.make_sparse_file('/tmp/sparse.bin', 1024 * 1024, EXT2_ROOT_INO, 0o644)

    # Create /usr/local with subdirs
    ul = builder.make_dir('/usr/local', EXT2_ROOT_INO, 0o755)
    builder.make_dir('/usr/local/bin', ul, 0o755)
    builder.make_dir('/usr/local/lib', ul, 0o755)

    # Create a file in a subdirectory
    builder.make_file('/usr/local/bin/hello.sh',
                      b'#!/bin/sh\necho "Hello, World!"\n',
                      ul, 0o755)

    builder.write_image(output)


if __name__ == '__main__':
    main()
