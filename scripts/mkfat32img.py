#!/usr/bin/env python3
"""
Create a raw disk image with MBR + FAT32 partition containing specified files.
Usage: python3 mkfat32img.py <img_file> <size_mb> <file_to_add> [<dst_name>]
"""

import struct
import sys
import os
import math

MBR_SIZE = 512
SECTOR_SIZE = 512

def mbr_sector(part_start_lba, part_size_lba):
    """Create MBR with one FAT32 LBA partition entry."""
    # CHS for start (approx)
    CHS_MAGIC = 0xFE
    part_start_chs = (0, 1, 1)  # head=0, sector=1, cyl=0 — placeholder
    part_end_cyl = min(1023, part_size_lba // (255 * 63))
    part_end_chs = (254, 63, part_end_cyl)

    def pack_chs(h, s, c):
        return bytes([h, (s & 0x3F) | ((c >> 2) & 0xC0), c & 0xFF])

    mbr = bytearray(512)
    # Boot code (first 446 bytes) — simple stub that halts
    for i in range(218):
        mbr[i] = 0x00
    # Partition entry 1
    off = 0x1BE
    mbr[off] = 0x80  # bootable
    off += 1
    mbr[off:off+3] = pack_chs(*part_start_chs)
    off += 3
    mbr[off] = 0x0C  # FAT32 LBA
    off += 1
    mbr[off:off+3] = pack_chs(*part_end_chs)
    off += 3
    struct.pack_into('<I', mbr, off, part_start_lba)
    off += 4
    struct.pack_into('<I', mbr, off, part_size_lba)
    # Boot signature
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return mbr

def fat32_bpb(oem=b"MSWIN4.1", bytes_per_sector=512, sectors_per_cluster=1,
              reserved_sectors=32, num_fats=2, root_entries=0,
              total_sectors=0, media=0xF8, sectors_per_fat=0,
              sectors_per_track=63, heads=255, hidden_sectors=0,
              total_sectors_32=0, sectors_per_fat_32=0,
              ext_flags=0, fs_version=0, root_cluster=2,
              fs_info_sector=1, backup_boot_sector=6):
    """Build FAT32 BPB (90 bytes)."""
    bpb = bytearray(90)
    # Jump instruction
    bpb[0:3] = b"\xEB\x58\x90"
    # OEM name
    assert len(oem) <= 8
    bpb[3:11] = oem.ljust(8, b' ')
    struct.pack_into('<H', bpb, 11, bytes_per_sector)
    bpb[13] = sectors_per_cluster
    struct.pack_into('<H', bpb, 14, reserved_sectors)
    bpb[16] = num_fats
    struct.pack_into('<H', bpb, 17, root_entries)
    struct.pack_into('<H', bpb, 19, total_sectors)  # 0 if > 65535
    bpb[21] = media
    struct.pack_into('<H', bpb, 22, sectors_per_fat)  # 0 for FAT32
    struct.pack_into('<H', bpb, 24, sectors_per_track)
    struct.pack_into('<H', bpb, 26, heads)
    struct.pack_into('<I', bpb, 28, hidden_sectors)
    struct.pack_into('<I', bpb, 32, total_sectors_32)
    # FAT32 specific (offset 36)
    struct.pack_into('<I', bpb, 36, sectors_per_fat_32)
    struct.pack_into('<H', bpb, 40, ext_flags)
    struct.pack_into('<H', bpb, 42, fs_version)
    struct.pack_into('<I', bpb, 44, root_cluster)
    struct.pack_into('<H', bpb, 48, fs_info_sector)
    struct.pack_into('<H', bpb, 50, backup_boot_sector)
    # Reserved (12 bytes)
    # Physical drive number
    bpb[64] = 0x80
    # Reserved
    bpb[65] = 0
    # Extended boot signature
    bpb[66] = 0x29
    # Volume ID
    struct.pack_into('<I', bpb, 67, 0x12345678)
    # Volume label
    vol = b"NO NAME    "
    assert len(vol) == 11
    bpb[71:82] = vol
    # System identifier
    sysid = b"FAT32   "
    assert len(sysid) == 8
    bpb[82:90] = sysid
    return bpb

def fsinfo_sector():
    """Create FSInfo sector."""
    fsi = bytearray(512)
    struct.pack_into('<I', fsi, 0, 0x41615252)  # lead signature
    struct.pack_into('<I', fsi, 484, 0x61417272)  # middle signature
    struct.pack_into('<I', fsi, 488, 2)  # last free cluster hint (cluster 2)
    struct.pack_into('<I', fsi, 492, 0xFFFFFFFF)  # hint for next free
    struct.pack_into('<I', fsi, 508, 0xAA550000)  # trail signature (0xAA550000)
    struct.pack_into('<H', fsi, 510, 0xAA55)
    return fsi

def short_name(name):
    """Convert filename to 8.3 short name (uppercase, pad)."""
    name = name.upper()
    if '.' in name:
        base, ext = name.rsplit('.', 1)
    else:
        base, ext = name, ''
    base = (base[:8] + '       ')[:8]
    ext = (ext[:3] + '   ')[:3]
    return base.encode('ascii') + ext.encode('ascii')

def dir_entry(name, attr=0x20, cluster=0, size=0,
              create_time=0, create_date=0, access_date=0,
              modify_time=0, modify_date=0):
    """Create a 32-byte FAT directory entry."""
    entry = bytearray(32)
    # Short name (11 bytes)
    sn = short_name(name)
    entry[0:11] = sn
    entry[11] = attr
    # Reserved (NT flags)
    entry[12] = 0
    # Creation time tenths
    entry[13] = 0
    struct.pack_into('<H', entry, 14, create_time)
    struct.pack_into('<H', entry, 16, create_date)
    struct.pack_into('<H', entry, 18, access_date)
    # High 16 bits of first cluster
    struct.pack_into('<H', entry, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into('<H', entry, 22, modify_time)
    struct.pack_into('<H', entry, 24, modify_date)
    # Low 16 bits of first cluster
    struct.pack_into('<H', entry, 26, cluster & 0xFFFF)
    struct.pack_into('<I', entry, 28, size)
    return entry

def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <img_file> <size_mb> <file_to_add> [<dst_name>]")
        sys.exit(1)

    img_path = sys.argv[1]
    size_mb = int(sys.argv[2])
    src_path = sys.argv[3]
    dst_name = sys.argv[4] if len(sys.argv) > 4 else os.path.basename(src_path)

    with open(src_path, 'rb') as f:
        file_data = f.read()

    total_sectors = (size_mb * 1024 * 1024) // SECTOR_SIZE

    # Partition starts at LBA 2048 (1MB alignment for modern stuff, keep it clean)
    part_start = 2048
    part_sectors = total_sectors - part_start
    part_size_bytes = part_sectors * SECTOR_SIZE

    # FAT32 parameters
    # We'll use 1 sector per cluster for simplicity
    bps = 512
    spc = 1  # sectors per cluster
    reserved = 32
    num_fats = 2

    # Calculate FAT size
    data_sectors = part_sectors - reserved
    # Each FAT entry is 4 bytes, each cluster is spc sectors
    # Max clusters = data_sectors / spc
    # FAT entries needed = data_sectors // spc + 2 (for FAT entries 0 and 1)
    # But FAT itself takes sectors too...
    # Iterate to find correct FAT size
    for fat_sectors_guess in range(1, 256):
        fat_total = fat_sectors_guess * 2  # two FATs
        root_data_sectors = part_sectors - reserved - fat_total
        if root_data_sectors <= 0:
            continue
        clusters = root_data_sectors // spc
        fat_needed_entries = clusters + 2
        fat_needed_sectors = math.ceil(fat_needed_entries * 4 / bps)
        if fat_needed_sectors <= fat_sectors_guess:
            fat_sectors = fat_sectors_guess
            break
    else:
        print("Couldn't converge FAT size")
        sys.exit(1)

    data_start_lba = part_start + reserved + fat_sectors * num_fats
    data_start = data_start_lba  # absolute LBA
    clusters_avail = (total_sectors - data_start) // spc

    # Number of clusters needed for the file
    file_clusters = math.ceil(len(file_data) / (spc * bps))

    if file_clusters < 1:
        file_clusters = 1  # minimum 1 cluster (even for empty file? well, at least 1)

    if file_clusters > clusters_avail:
        print(f"Not enough clusters: need {file_clusters}, have {clusters_avail}")
        sys.exit(1)

    # Root directory cluster = 2
    root_cluster = 2
    # File starts at cluster 3 (if root cluster is 2)
    file_cluster = root_cluster + 1

    # Pad file data to fill exactly file_clusters
    cluster_bytes = spc * bps
    file_data_padded = file_data + b'\x00' * (file_clusters * cluster_bytes - len(file_data))

    # Build the image
    img = bytearray(total_sectors * SECTOR_SIZE)

    # Write MBR
    img[0:512] = mbr_sector(part_start, part_sectors)

    # Write FAT32 volume boot record at partition start
    vbr_offset = part_start * SECTOR_SIZE
    vbr = fat32_bpb(
        total_sectors_32=part_sectors,
        sectors_per_fat_32=fat_sectors,
        hidden_sectors=part_start,
        root_cluster=root_cluster,
    )
    # Pad BPB + boot code to 512 bytes
    vbr_full = vbr + b'\x00' * (512 - len(vbr))
    vbr_full[510] = 0x55
    vbr_full[511] = 0xAA
    img[vbr_offset:vbr_offset+512] = vbr_full

    # FSInfo sector (sector 1 within partition)
    fsi_offset = (part_start + 1) * SECTOR_SIZE
    img[fsi_offset:fsi_offset+512] = fsinfo_sector()

    # Backup boot sector (sector 6 = backup of VBR)
    bk_offset = (part_start + 6) * SECTOR_SIZE
    img[bk_offset:bk_offset+512] = vbr_full

    # Reserved sectors: zero them out (32 sectors starting from partition start)
    # Already zero from bytearray, but we wrote VBR at 0 and FSInfo at 1 and backup at 6

    # Write FATs
    fat_entries = clusters_avail + 2
    for fat_idx in range(num_fats):
        fat_start_lba = part_start + reserved + fat_idx * fat_sectors
        fat_off = fat_start_lba * SECTOR_SIZE
        fat_data = bytearray(fat_sectors * SECTOR_SIZE)
        # Entry 0: media descriptor + 0xFFFFF (end-of-chain marker for cluster 0)
        struct.pack_into('<I', fat_data, 0, 0x0FFFFFF8 | (0xF8 << 24))
        # Actually: entry 0 low byte = media type, rest = 0xFF
        # Standard: fat_entry[0] = 0x0FFFFFF8 (EOC with media type 0xF8 in low byte)
        # Entry 1: 0x0FFFFFFF (end-of-chain, clean shutdown)
        struct.pack_into('<I', fat_data, 4, 0x0FFFFFFF)
        # Root cluster (cluster 2): EOC marker
        struct.pack_into('<I', fat_data, 8, 0x0FFFFFFF)
        # File clusters: chain
        for i in range(file_clusters):
            cluster_n = root_cluster + 1 + i
            entry_off = cluster_n * 4
            if i == file_clusters - 1:
                val = 0x0FFFFFFF  # EOC
            else:
                val = cluster_n + 1  # next cluster
            struct.pack_into('<I', fat_data, entry_off, val)
        img[fat_off:fat_off+len(fat_data)] = fat_data

    # Write root directory (at data_start, which is cluster 2)
    root_off = data_start * SECTOR_SIZE
    # Create root directory entries
    root_entries_data = bytearray(cluster_bytes)
    name_for_short = dst_name
    if dst_name.upper().endswith('.ELF'):
        # Use 8.3: INIT   ELF (but we can use short_name directly)
        pass

    # File entry
    entry = dir_entry(name=dst_name, attr=0x20, cluster=file_cluster, size=len(file_data))
    root_entries_data[0:32] = entry
    img[root_off:root_off+cluster_bytes] = root_entries_data

    # Write file data at file cluster
    file_off = (data_start + file_cluster - 2) * SECTOR_SIZE
    img[file_off:file_off+len(file_data_padded)] = file_data_padded

    with open(img_path, 'wb') as f:
        f.write(img)

    actual_size = os.path.getsize(img_path)
    print(f"Created {img_path} ({actual_size} bytes, {actual_size // (1024*1024)} MB)")
    print(f"  Partition: LBA {part_start} -> {part_start + part_sectors - 1} ({part_sectors} sectors)")
    print(f"  FAT size: {fat_sectors} sectors x {num_fats}")
    print(f"  Data start: LBA {data_start}")
    print(f"  Clusters available: {clusters_avail}")
    print(f"  File '{src_path}' ({len(file_data)} bytes) -> '{dst_name}' at cluster {file_cluster}")
    print(f"  File clusters: {file_clusters}")

if __name__ == '__main__':
    main()
