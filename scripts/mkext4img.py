#!/usr/bin/env python3
"""
Create ext4 filesystem images for fsck testing.

Generates valid ext4 images from scratch (byte-level) with 1024-byte blocks
for maximum compatibility with e2fsck validation.

Usage: python3 mkext4img.py <output.img> <size_mb>
       python3 mkext4img.py <output.img> <size_mb> --features <features>

Features (comma-separated): extents,filetype,sparse
Default: extents,filetype
"""

import struct
import sys
import os
import math
import time

# ── Ext4 constants ──────────────────────────────────────────────────────
EXT4_SUPER_MAGIC        = 0xEF53
EXT4_ROOT_INO           = 2
EXT4_GOOD_OLD_REV       = 0
EXT4_DYNAMIC_REV        = 1

EXT4_FEATURE_INCOMPAT_FILETYPE   = 0x0002
EXT4_FEATURE_INCOMPAT_EXTENTS    = 0x0040
EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER = 0x0001
EXT4_FEATURE_RO_COMPAT_LARGE_FILE   = 0x0002
EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE  = 0x0040
EXT4_VALID_FS            = 0x0001
EXT4_EXTENTS_FL          = 0x00080000
EXT4_EXTENT_MAGIC        = 0xF30A
EXT4_FT_DIR              = 2
EXT4_GOOD_OLD_INODE_SIZE = 128
INODE_SIZE               = 128  # good old inode size for compatibility

BLOCK_SIZE               = 1024
LOG_BLOCK_SIZE           = 0  # 2^0 * 1024 = 1024
BLOCKS_PER_GROUP         = 8192
INODES_PER_GROUP         = 512
FIRST_DATA_BLOCK         = 1  # standard for 1024-byte blocks

def div_round_up(a, b):
    return (a + b - 1) // b

def pack_superblock(sb):
    """Pack a 1024-byte ext4 superblock."""
    data = struct.pack('<I', sb['s_inodes_count'])
    data += struct.pack('<I', sb['s_blocks_count'])
    data += struct.pack('<I', sb['s_r_blocks_count'])
    data += struct.pack('<I', sb['s_free_blocks_count'])
    data += struct.pack('<I', sb['s_free_inodes_count'])
    data += struct.pack('<I', sb['s_first_data_block'])
    data += struct.pack('<I', sb['s_log_block_size'])
    data += struct.pack('<I', sb['s_log_frag_size'])
    data += struct.pack('<I', sb['s_blocks_per_group'])
    data += struct.pack('<I', sb['s_frags_per_group'])
    data += struct.pack('<I', sb['s_inodes_per_group'])
    data += struct.pack('<I', sb['s_mtime'])
    data += struct.pack('<I', sb['s_wtime'])
    data += struct.pack('<H', sb['s_mnt_count'])
    data += struct.pack('<H', sb['s_max_mnt_count'])
    data += struct.pack('<H', sb['s_magic'])
    data += struct.pack('<H', sb['s_state'])
    data += struct.pack('<H', sb['s_errors'])
    data += struct.pack('<H', sb['s_minor_rev_level'])
    data += struct.pack('<I', sb['s_lastcheck'])
    data += struct.pack('<I', sb['s_checkinterval'])
    data += struct.pack('<I', sb['s_creator_os'])
    data += struct.pack('<I', sb['s_rev_level'])
    data += struct.pack('<H', sb['s_def_resuid'])
    data += struct.pack('<H', sb['s_def_resgid'])
    # Dynamic rev
    data += struct.pack('<I', sb['s_first_ino'])
    data += struct.pack('<H', sb['s_inode_size'])
    data += struct.pack('<H', sb['s_block_group_nr'])
    data += struct.pack('<I', sb['s_feature_compat'])
    data += struct.pack('<I', sb['s_feature_incompat'])
    data += struct.pack('<I', sb['s_feature_ro_compat'])
    data += sb['s_uuid'][:16].ljust(16, b'\x00')
    data += sb['s_volume_name'].encode('ascii')[:16].ljust(16, b'\x00')
    data += sb['s_last_mounted'].encode('ascii')[:64].ljust(64, b'\x00')
    data += struct.pack('<I', sb['s_algo_bitmap'])
    # Performance
    data += struct.pack('<B', sb['s_prealloc_blocks'])
    data += struct.pack('<B', sb['s_prealloc_dir_blocks'])
    data += struct.pack('<H', sb['s_reserved_gdt_blocks'])
    # Journal
    data += b'\x00' * 16  # s_journal_uuid
    data += struct.pack('<I', 0)  # s_journal_inum
    data += struct.pack('<I', 0)  # s_journal_dev
    data += struct.pack('<I', 0)  # s_last_orphan
    # Hash seed
    for _ in range(4):
        data += struct.pack('<I', 0)
    data += struct.pack('<B', 0)  # s_def_hash_version
    data += struct.pack('<B', 0)  # s_jnl_backup_type
    data += struct.pack('<H', 32)  # s_desc_size = 32 for non-64bit
    # Mount
    data += struct.pack('<I', 0x0040)  # s_default_mount_opts
    data += struct.pack('<I', 0)  # s_first_meta_bg
    data += struct.pack('<I', int(time.time()))  # s_mkfs_time
    for _ in range(17):
        data += struct.pack('<I', 0)  # s_jnl_blocks
    # 64-bit
    data += struct.pack('<I', 0)  # s_blocks_count_hi
    data += struct.pack('<I', 0)  # s_r_blocks_count_hi
    data += struct.pack('<I', 0)  # s_free_blocks_hi
    data += struct.pack('<H', 28)  # s_min_extra_isize
    data += struct.pack('<H', 28)  # s_want_extra_isize
    # Misc
    data += struct.pack('<I', 0)  # s_flags
    data += struct.pack('<H', 0)  # s_raid_stride
    data += struct.pack('<H', 0)  # s_mmp_interval
    data += struct.pack('<Q', 0)  # s_mmp_block
    data += struct.pack('<I', 0)  # s_raid_stripe_width
    data += struct.pack('<B', 0)  # s_log_groups_per_flex
    data += struct.pack('<B', 0)  # s_checksum_type
    data += struct.pack('<H', 0)  # reserved_pad
    # Snapshot
    data += struct.pack('<Q', 0)  # s_kbytes_written
    data += struct.pack('<I', 0)  # s_snapshot_inum
    data += struct.pack('<I', 0)  # s_snapshot_id
    data += struct.pack('<Q', 0)  # s_snapshot_r_blocks_count
    data += struct.pack('<I', 0)  # s_snapshot_list
    # Pad to 1024
    return data[:1024].ljust(1024, b'\x00')

def pack_bgd_32(bgd):
    """Pack a 32-byte block group descriptor."""
    data = struct.pack('<I', bgd['bg_block_bitmap'])
    data += struct.pack('<I', bgd['bg_inode_bitmap'])
    data += struct.pack('<I', bgd['bg_inode_table'])
    data += struct.pack('<H', bgd['bg_free_blocks_count'])
    data += struct.pack('<H', bgd['bg_free_inodes_count'])
    data += struct.pack('<H', bgd['bg_used_dirs_count'])
    data += struct.pack('<H', bgd['bg_flags'])
    data += struct.pack('<I', bgd.get('bg_exclude_bitmap', 0))
    data += struct.pack('<H', bgd.get('bg_block_bitmap_csum', 0))
    data += struct.pack('<H', bgd.get('bg_inode_bitmap_csum', 0))
    data += struct.pack('<H', bgd.get('bg_itable_unused', 0))
    data += struct.pack('<H', bgd.get('bg_checksum', 0))
    return data[:32].ljust(32, b'\x00')

def pack_inode_128(inode):
    """Pack a 128-byte ext2 inode."""
    data = struct.pack('<H', inode['i_mode'])
    data += struct.pack('<H', inode['i_uid'])
    data += struct.pack('<I', inode['i_size_lo'])
    data += struct.pack('<I', inode['i_atime'])
    data += struct.pack('<I', inode['i_ctime'])
    data += struct.pack('<I', inode['i_mtime'])
    data += struct.pack('<I', inode['i_dtime'])
    data += struct.pack('<H', inode['i_gid'])
    data += struct.pack('<H', inode['i_links_count'])
    data += struct.pack('<I', inode['i_blocks_lo'])
    data += struct.pack('<I', inode['i_flags'])
    data += struct.pack('<I', inode.get('i_osd1', 0))
    # i_block (15 x uint32 = 60 bytes)
    for v in inode['i_block'][:15]:
        data += struct.pack('<I', v)
    data += struct.pack('<I', inode.get('i_generation', 0))
    data += struct.pack('<I', inode.get('i_file_acl_lo', 0))
    data += struct.pack('<I', inode.get('i_size_hi', 0))
    data += struct.pack('<I', inode.get('i_faddr', 0))
    return data[:128].ljust(128, b'\x00')

def pack_extent_header(num_entries=0, max_entries=4, depth=0):
    data = struct.pack('<H', EXT4_EXTENT_MAGIC)
    data += struct.pack('<H', num_entries)
    data += struct.pack('<H', max_entries)
    data += struct.pack('<H', depth)
    data += struct.pack('<I', 0)  # generation
    return data

def pack_extent(ee_block=0, ee_len=1, ee_start_hi=0, ee_start_lo=0):
    data = struct.pack('<I', ee_block)
    data += struct.pack('<H', ee_len)
    data += struct.pack('<H', ee_start_hi)
    data += struct.pack('<I', ee_start_lo)
    return data

def pack_dirent(inode, rec_len, name, file_type=0):
    name_enc = name.encode('utf-8')
    data = struct.pack('<I', inode)
    data += struct.pack('<H', rec_len)
    data += struct.pack('<B', len(name_enc))
    data += struct.pack('<B', file_type)
    data += name_enc
    return data[:rec_len].ljust(rec_len, b'\x00')

def create_image(image_path, size_mb, features_str):
    """Create an ext4 image with 1024-byte blocks."""
    features = set(f.strip() for f in features_str.split(',') if f.strip())
    use_extents = 'extents' in features or not features  # default on
    use_sparse = 'sparse' in features or not features

    bs = BLOCK_SIZE           # 1024
    bg = BLOCKS_PER_GROUP     # 8192
    ipg = INODES_PER_GROUP    # 512
    isize = INODE_SIZE        # 128
    first_db = FIRST_DATA_BLOCK  # 1

    img_bytes = size_mb * 1024 * 1024
    total_blocks = img_bytes // bs

    num_groups = div_round_up(total_blocks - first_db, bg)
    total_blocks = first_db + num_groups * bg
    img_bytes = total_blocks * bs
    total_inodes = num_groups * ipg

    # Standard layout for 1024-byte blocks:
    # Block 0: boot block (1024 bytes)
    # Block 1: superblock (offset 1024 = byte 1024)
    # Block 2+: BGD table (num_groups * 32 bytes, one block)
    # Then per group:
    #   block bitmap, inode bitmap, inode table, then data

    bgd_per_block = bs // 32  # 32 BGD entries per block
    bgd_blocks = div_round_up(num_groups, bgd_per_block)

    # sb_bgd_blocks: block 1 (superblock) + bgd_blocks
    sb_bgd_blocks = 1 + bgd_blocks  # blocks 1 to (1 + bgd_blocks - 1)

    itable_per_group_blocks = div_round_up(ipg * isize, bs)  # 512*128/1024 = 64
    meta_per_group = sb_bgd_blocks + 2 + itable_per_group_blocks

    # Build metadata layout
    bg_meta = []
    for g in range(num_groups):
        g_start = first_db + g * bg
        bb = g_start + sb_bgd_blocks
        ib = bb + 1
        it = ib + 1
        ds = it + itable_per_group_blocks
        bg_meta.append({
            'bb': bb, 'ib': ib, 'it': it, 'ds': ds,
        })

    # Allocate image
    data_start = bg_meta[0]['ds']
    img = bytearray(img_bytes)

    # ── Superblock ──────────────────────────────────────────────────
    feature_incompat = EXT4_FEATURE_INCOMPAT_FILETYPE
    if use_extents:
        feature_incompat |= EXT4_FEATURE_INCOMPAT_EXTENTS

    feature_ro_compat = EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER | \
                        EXT4_FEATURE_RO_COMPAT_LARGE_FILE | \
                        EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE

    now = int(time.time())
    sb = {
        's_inodes_count': total_inodes,
        's_blocks_count': total_blocks,
        's_r_blocks_count': max(1, total_blocks // 20),
        's_free_blocks_count': total_blocks - first_db,
        's_free_inodes_count': total_inodes,
        's_first_data_block': first_db,
        's_log_block_size': LOG_BLOCK_SIZE,
        's_log_frag_size': LOG_BLOCK_SIZE,
        's_blocks_per_group': bg,
        's_frags_per_group': bg,
        's_inodes_per_group': ipg,
        's_mtime': 0,
        's_wtime': now,
        's_mnt_count': 0,
        's_max_mnt_count': 65535,
        's_magic': EXT4_SUPER_MAGIC,
        's_state': EXT4_VALID_FS,
        's_errors': 1,
        's_minor_rev_level': 0,
        's_lastcheck': now,
        's_checkinterval': 0xFFFFFFFF,
        's_creator_os': 0,
        's_rev_level': EXT4_DYNAMIC_REV,
        's_def_resuid': 0,
        's_def_resgid': 0,
        's_first_ino': 11,
        's_inode_size': isize,
        's_block_group_nr': 0,
        's_feature_compat': 0,
        's_feature_incompat': feature_incompat,
        's_feature_ro_compat': feature_ro_compat,
        's_uuid': os.urandom(16),
        's_volume_name': 'ext4-test',
        's_last_mounted': '/',
        's_algo_bitmap': 0,
        's_prealloc_blocks': 0,
        's_prealloc_dir_blocks': 0,
        's_reserved_gdt_blocks': 0,
    }

    # Write superblock at block 1, offset 0 (since sb is at offset 1024)
    sb_data = pack_superblock(sb)
    img[1024:1024 + len(sb_data)] = sb_data

    # ── BGD table ───────────────────────────────────────────────────
    bgd_list = []
    for g in range(num_groups):
        m = bg_meta[g]
        bgd_list.append(pack_bgd_32({
            'bg_block_bitmap': m['bb'],
            'bg_inode_bitmap': m['ib'],
            'bg_inode_table': m['it'],
            'bg_free_blocks_count': bg,
            'bg_free_inodes_count': ipg,
            'bg_used_dirs_count': 1 if g == 0 else 0,
            'bg_flags': 0,
            'bg_exclude_bitmap': 0,
            'bg_block_bitmap_csum': 0,
            'bg_inode_bitmap_csum': 0,
            'bg_itable_unused': 0,
            'bg_checksum': 0,
        }))

    # BGD starts at byte 2048 (block 2, right after superblock at byte 1024-2047)
    # For 1024b blocks: block 0 = boot, block 1 = sb, block 2 = BGD starts
    bgd_off = 2048  # block 2
    for g in range(num_groups):
        off = bgd_off + g * 32
        if off + 32 <= img_bytes:
            img[off:off + 32] = bgd_list[g]

    # Superblock + BGD backups for sparse superblock
    if use_sparse:
        for g in range(num_groups):
            # Sparse: only groups 0 and powers of 3/5/7 have SB+BGD backups
            if g == 0:
                continue
            is_power = False
            for p in [3, 5, 7]:
                t = g
                while t > 0 and t % p == 0:
                    t //= p
                if t == 1:
                    is_power = True
                    break
            if not is_power and g != 1:
                continue

            # Write SB backup
            sb_bak_off = (first_db + g * bg) * bs + 1024
            if sb_bak_off + 1024 <= img_bytes:
                img[sb_bak_off:sb_bak_off + 1024] = sb_data[:1024]

            # Write BGD backup
            bgd_bak_off = (first_db + g * bg) * bs + 2048
            for bg_g in range(num_groups):
                off = bgd_bak_off + bg_g * 32
                if off + 32 <= img_bytes:
                    img[off:off + 32] = bgd_list[bg_g]

    # ── Block and inode bitmaps ─────────────────────────────────────
    for g in range(num_groups):
        m = bg_meta[g]
        bmap = bytearray(bs)
        imap = bytearray(bs)

        g_start = first_db + g * bg
        g_end = g_start + bg
        data_end = min(m['ds'], g_end)

        # Mark all metadata blocks in this group as used
        for blk in range(g_start, data_end):
            if blk < g_start + bg:
                byte_off = (blk - g_start) // 8
                bit_off = (blk - g_start) % 8
                if byte_off < bs:
                    bmap[byte_off] |= (1 << bit_off)

        # Mark root inode as used
        if g == 0:
            root_byte = (EXT4_ROOT_INO - 1) // 8
            root_bit = (EXT4_ROOT_INO - 1) % 8
            if root_byte < bs:
                imap[root_byte] |= (1 << root_bit)

        img[m['bb'] * bs:(m['bb'] + 1) * bs] = bmap
        img[m['ib'] * bs:(m['ib'] + 1) * bs] = imap

    # ── Inode table ─────────────────────────────────────────────────
    for g in range(num_groups):
        m = bg_meta[g]
        itable_off = m['it'] * bs

        for ino_off in range(ipg):
            inode_num = g * ipg + ino_off + 1
            inode_data = b'\x00' * isize

            if inode_num == EXT4_ROOT_INO:
                root_db = data_start  # first data block

                # Create ".", ".." directory entries
                bs_this = bs
                dot_rec_len = 12
                dotdot_rec_len = bs_this - dot_rec_len

                dentries = b''
                dentries += pack_dirent(EXT4_ROOT_INO, dot_rec_len,
                                        '.', EXT4_FT_DIR)
                dentries += pack_dirent(EXT4_ROOT_INO, dotdot_rec_len,
                                        '..', EXT4_FT_DIR)
                dentries = dentries[:bs_this].ljust(bs_this, b'\x00')

                # Write directory data block
                d_off = root_db * bs
                if d_off + bs <= img_bytes:
                    img[d_off:d_off + bs] = dentries

                # Build i_block entries
                i_block = [0] * 15
                if use_extents:
                    # Extent tree: header + 1 extent
                    eh = pack_extent_header(num_entries=1, max_entries=4, depth=0)
                    ext = pack_extent(ee_block=0, ee_len=1,
                                      ee_start_hi=0, ee_start_lo=root_db)
                    ext_bytes = eh + ext
                    for i in range(min(15, div_round_up(len(ext_bytes), 4))):
                        off = i * 4
                        if off + 4 <= len(ext_bytes):
                            i_block[i] = struct.unpack('<I', ext_bytes[off:off+4])[0]
                else:
                    # Indirect blocks: direct block 0 -> root_db
                    i_block[0] = root_db

                root_inode = pack_inode_128({
                    'i_mode': 0x41ED,  # 0o755 directory
                    'i_uid': 0,
                    'i_size_lo': len(dentries),
                    'i_atime': now,
                    'i_ctime': now,
                    'i_mtime': now,
                    'i_dtime': 0,
                    'i_gid': 0,
                    'i_links_count': 2,
                    'i_blocks_lo': bs // 512,
                    'i_flags': EXT4_EXTENTS_FL if use_extents else 0,
                    'i_osd1': 0,
                    'i_block': i_block,
                    'i_generation': 0,
                    'i_file_acl_lo': 0,
                    'i_size_hi': 0,
                    'i_faddr': 0,
                })
                inode_data = root_inode

            inode_off = itable_off + ino_off * isize
            if inode_off + isize <= img_bytes:
                img[inode_off:inode_off + isize] = inode_data

    # ── Update superblock free counts ───────────────────────────────
    used = data_start - first_db
    free_blocks = total_blocks - first_db - used
    free_inodes = total_inodes - 1  # root inode used

    # Compute BGD free counts
    for g in range(num_groups):
        m = bg_meta[g]
        g_start = first_db + g * bg
        g_end = g_start + bg
        g_used = min(m['ds'], g_end) - g_start
        g_free = bg - g_used
        g_free_inodes = ipg - (1 if g == 0 else 0)

        # Write group-level free counts to BGD table
        for bg_g in range(num_groups):
            off = bgd_off + bg_g * 32
            if off + 32 <= img_bytes:
                # bg_free_blocks_count at offset 12, bg_free_inodes_count at 14
                img[off + 12:off + 14] = struct.pack('<H', g_free)
                img[off + 14:off + 16] = struct.pack('<H', g_free_inodes)

    # Write superblock free counts
    img[0:4] = struct.pack('<I', total_inodes)  # s_inodes_count
    img[4:8] = struct.pack('<I', total_blocks)  # s_blocks_count
    img[12:16] = struct.pack('<I', free_blocks)  # s_free_blocks_count
    img[16:20] = struct.pack('<I', free_inodes)  # s_free_inodes_count

    # Write to file
    try:
        with open(image_path, 'wb') as f:
            f.write(img)
        return True
    except IOError as e:
        print(f"Error writing image: {e}", file=sys.stderr)
        return False

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    image_path = sys.argv[1]
    if not os.path.isabs(image_path):
        home = os.environ.get('HOME', os.path.expanduser('~'))
        repo = os.path.join(home, 'os')
        if os.path.isdir(repo):
            image_path = os.path.join(repo, image_path)

    try:
        size_mb = int(sys.argv[2])
    except ValueError:
        print(f"Invalid size: {sys.argv[2]}", file=sys.stderr)
        return 1

    features = "extents,filetype"
    if '--features' in sys.argv:
        idx = sys.argv.index('--features')
        if idx + 1 < len(sys.argv):
            features = sys.argv[idx + 1]

    if size_mb < 4:
        print("Minimum image size: 4 MB", file=sys.stderr)
        return 1

    print(f"Creating ext4 image: {image_path}")
    print(f"  Size: {size_mb} MB")
    print(f"  Features: {features}")

    success = create_image(image_path, size_mb, features)
    if success:
        fsize = os.path.getsize(image_path)
        print(f"  Created: {image_path} ({fsize} bytes)")
        return 0
    else:
        print("  FAILED", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
