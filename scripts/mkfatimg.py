#!/usr/bin/env python3
"""
Create a FAT32 disk image from a source root directory.

Usage: python3 mkfatimg.py <output.img> <size_mb> <source_dir>

Builds a bootable FAT32 image with MBR partition containing all files
from source_dir placed at the root of the filesystem.
Supports directories, long filenames, and nested paths.
"""

import struct, sys, os, math
from collections import defaultdict

SECTOR_SIZE = 512

def mbr_sector(part_start_lba, part_size_lba):
    mbr = bytearray(512)
    off = 0x1BE
    mbr[off] = 0x80
    off += 1
    mbr[off:off+3] = bytes([0, 1, 1])
    off += 3
    mbr[off] = 0x0C
    off += 1
    mbr[off:off+3] = bytes([254, 63, min(1023, part_size_lba // (255*63)) & 0xFF])
    off += 3
    struct.pack_into('<I', mbr, off, part_start_lba)
    off += 4
    struct.pack_into('<I', mbr, off, part_size_lba)
    mbr[510:512] = b'\x55\xAA'
    return mbr

def build_image(src_dir, size_mb, output):
    total_sectors = (size_mb * 1024 * 1024) // SECTOR_SIZE
    img = bytearray(total_sectors * SECTOR_SIZE)
    part_start = 2048
    part_sectors = total_sectors - part_start

    # Collect files
    file_list = []
    for root, dirs, files in os.walk(src_dir):
        rel = os.path.relpath(root, src_dir)
        prefix = '' if rel == '.' else rel
        for f in files:
            full = os.path.join(root, f)
            dst = os.path.join(prefix, f) if prefix else f
            file_list.append((dst, full, os.path.getsize(full)))

    dir_set = set()
    for dst, _, _ in file_list:
        parts = dst.replace('\\', '/').split('/')
        for i in range(1, len(parts)):
            dir_set.add('/'.join(parts[:i]))

    num_fats = 2
    reserved = 32

    # Calculate FAT size
    for guess in range(1, 512):
        fat_total = guess * num_fats
        ds = part_sectors - reserved - fat_total
        if ds <= 0: continue
        clusters = ds
        if math.ceil((clusters + 2) * 4 / 512) <= guess:
            fat_sectors = guess
            break
    else:
        fat_sectors = 128

    data_start_lba = part_start + reserved + fat_sectors * num_fats
    clusters_avail = (total_sectors - data_start_lba)
    cluster_bytes = 512

    # Allocate clusters for files
    next_cluster = 3
    allocs = []
    for dst, src, sz in sorted(file_list, key=lambda x: x[0]):
        with open(src, 'rb') as fh:
            data = fh.read()
        ncl = max(1, math.ceil(len(data) / cluster_bytes))
        if next_cluster + ncl - 3 > clusters_avail:
            print(f"FATAL: out of clusters at {dst}")
            sys.exit(1)
        allocs.append((dst, next_cluster, ncl, data))
        next_cluster += ncl

    # Allocate clusters for directories (enough for all entries)
    dir_clusters = {'': 2}
    dir_num_clusters = {'': 1}
    for d in sorted(dir_set, key=lambda x: len(x.split('/'))):
        # Count entries in this dir
        prefix = d + '/' if d else ''
        nentries = 2  # . and ..
        for alloc in allocs:
            dst = alloc[0]
            if dst.startswith(prefix) and '/' not in dst[len(prefix):]:
                nentries += 1
        for sd in dir_set:
            if sd != d and sd.startswith(prefix) and '/' not in sd[len(prefix):]:
                nentries += 1
        ncl = max(1, math.ceil(nentries * 32 / cluster_bytes))
        dir_clusters[d] = next_cluster
        dir_num_clusters[d] = ncl
        next_cluster += ncl

    fat_entries = next_cluster
    fat_sz_bytes = math.ceil(fat_entries * 4 / 512) * 512

    # Build FAT
    fat = bytearray(fat_sz_bytes)
    struct.pack_into('<I', fat, 0, 0x0FFFFFF8)
    struct.pack_into('<I', fat, 4, 0x0FFFFFFF)
    struct.pack_into('<I', fat, 8, 0x0FFFFFFF)  # root cluster = 2
    for dst, sc, nc, _ in allocs:
        for i in range(nc):
            cl = sc + i
            val = 0x0FFFFFFF if i == nc - 1 else cl + 1
            struct.pack_into('<I', fat, cl * 4, val)
    for d, dc in dir_clusters.items():
        nc = dir_num_clusters[d]
        for i in range(nc):
            cl = dc + i
            if cl == 2: continue  # root already set
            if cl * 4 + 4 > len(fat):
                fat.extend(b'\x00' * 512)
            val = 0x0FFFFFFF if i == nc - 1 else cl + 1
            struct.pack_into('<I', fat, cl * 4, val)

    # Helper: cluster for file path
    def cl_for(path):
        for alloc in allocs:
            if alloc[0] == path:
                return alloc[1]
        return 0

    data_start_off = data_start_lba * SECTOR_SIZE

    # Write directory entries
    for dpath, dc in dir_clusters.items():
        nc = dir_num_clusters[dpath]
        total_bytes = nc * cluster_bytes
        off = data_start_off + (dc - 2) * cluster_bytes
        entries = bytearray(total_bytes)
        pos = 0

        # . and ..
        parent_dc = dir_clusters.get('/'.join(dpath.split('/')[:-1]), 0) if dpath else 0
        # '.'
        e = bytearray(32)
        e[0:11] = b'.          '
        e[11] = 0x10
        struct.pack_into('<H', e, 20, (dc >> 16) & 0xFFFF)
        struct.pack_into('<H', e, 26, dc & 0xFFFF)
        entries[pos:pos+32] = e
        pos += 32
        # '..'
        e = bytearray(32)
        e[0:11] = b'..         '
        e[11] = 0x10
        struct.pack_into('<H', e, 20, (parent_dc >> 16) & 0xFFFF)
        struct.pack_into('<H', e, 26, parent_dc & 0xFFFF)
        entries[pos:pos+32] = e
        pos += 32

        # Files in this dir
        dir_children = []
        prefix = dpath + '/' if dpath else ''
        for alloc in allocs:
            dst, sc, nc, data = alloc
            if dst.startswith(prefix) and '/' not in dst[len(prefix):]:
                dir_children.append(('f', dst[len(prefix):], sc, len(data)))
        # Subdirs in this dir
        sub_prefix = dpath + '/' if dpath else ''
        for sd, sdc in sorted(dir_clusters.items(), key=lambda x: x[0]):
            if sd == dpath or not sd: continue
            if sd.startswith(sub_prefix) and '/' not in sd[len(sub_prefix):]:
                dir_children.append(('d', sd[len(sub_prefix):], sdc, 0))

        for ctype, cname, ccl, csz in dir_children:
            # Short name (8.3)
            name_u = cname.upper()
            if '.' in name_u:
                base = name_u.rsplit('.', 1)[0][:8].ljust(8)
                ext = name_u.rsplit('.', 1)[1][:3].ljust(3)
            else:
                base = name_u[:8].ljust(8)
                ext = '   '
            sn = base.encode('ascii', errors='replace') + ext.encode('ascii', errors='replace')

            # LFN entries for non-8.3 names
            if cname != name_u or len(cname) > 12:
                sn_base = (name_u.split('.')[0][:6] + '~1')[:8].ljust(8) if '.' not in name_u or len(name_u.split('.')[0]) > 8 else name_u.split('.')[0][:8].ljust(8)
                sn_ext = (name_u.split('.')[1][:3] if '.' in name_u else '').ljust(3)
                sn = sn_base.encode('ascii', errors='replace') + sn_ext.encode('ascii', errors='replace')
                if len(sn) != 11:
                    base_h = name_u[:8].ljust(8)
                    ext_h = b'   '
                    sn = base_h.encode() + ext_h

                cksum = 0
                for b in sn:
                    cksum = (((cksum & 1) << 7) | ((cksum >> 1) & 0x7F)) + b
                    cksum &= 0xFF

                chunks = [cname[i:i+13] for i in range(0, len(cname), 13)]
                n = len(chunks)
                for i, chunk in enumerate(chunks):
                    seq = n - i
                    if i == 0:
                        seq |= 0x40
                    le = bytearray(32)
                    le[0] = seq
                    le[11] = 0x0F
                    le[13] = cksum
                    for j in range(5):
                        if j < len(chunk):
                            struct.pack_into('<H', le, 1 + j*2, ord(chunk[j]))
                    for j in range(6):
                        if 5 + j < len(chunk):
                            struct.pack_into('<H', le, 14 + j*2, ord(chunk[5 + j]))
                    for j in range(2):
                        if 11 + j < len(chunk):
                            struct.pack_into('<H', le, 28 + j*2, ord(chunk[11 + j]))
                    if pos + 32 <= total_bytes:
                        entries[pos:pos+32] = le
                        pos += 32

            # Main entry
            e = bytearray(32)
            e[0:11] = sn
            e[11] = 0x10 if ctype == 'd' else 0x20
            struct.pack_into('<H', e, 20, (ccl >> 16) & 0xFFFF)
            struct.pack_into('<H', e, 26, ccl & 0xFFFF)
            struct.pack_into('<I', e, 28, csz)
            if pos + 32 <= total_bytes:
                entries[pos:pos+32] = e
                pos += 32

        img[off:off+total_bytes] = entries

    # Write file data
    for dst, sc, nc, data in allocs:
        off = data_start_off + (sc - 2) * cluster_bytes
        img[off:off+len(data)] = data

    # Write MBR
    img[0:512] = mbr_sector(part_start, part_sectors)

    # Write VBR
    vbr = bytearray(512)
    vbr[0:3] = b'\xEB\x58\x90'
    vbr[3:11] = b'MSWIN4.1'
    struct.pack_into('<H', vbr, 11, 512)
    vbr[13] = 1  # spc
    struct.pack_into('<H', vbr, 14, reserved)
    vbr[16] = num_fats
    vbr[21] = 0xF8
    struct.pack_into('<H', vbr, 22, 0)  # SPT
    struct.pack_into('<H', vbr, 24, 63)
    struct.pack_into('<H', vbr, 26, 255)
    struct.pack_into('<I', vbr, 28, part_start)
    struct.pack_into('<I', vbr, 32, part_sectors)
    struct.pack_into('<I', vbr, 36, fat_sectors)
    struct.pack_into('<H', vbr, 44, 2)  # root cluster
    vbr[64] = 0x80
    vbr[66] = 0x29
    struct.pack_into('<I', vbr, 67, 0x12345678)
    vbr[71:82] = b'NO NAME    '
    vbr[82:90] = b'FAT32   '
    vbr[510:512] = b'\x55\xAA'
    img[part_start * 512:(part_start * 512) + 512] = vbr

    # FSInfo sector
    fsi = bytearray(512)
    struct.pack_into('<I', fsi, 0, 0x41615252)
    struct.pack_into('<I', fsi, 484, 0x61417272)
    struct.pack_into('<I', fsi, 488, next_cluster)
    struct.pack_into('<I', fsi, 492, 0xFFFFFFFF)
    struct.pack_into('<I', fsi, 508, 0xAA550000)
    struct.pack_into('<H', fsi, 510, 0xAA55)
    img[(part_start + 1) * 512:(part_start + 2) * 512] = fsi

    # Backup VBR at sector 6
    img[(part_start + 6) * 512:(part_start + 7) * 512] = vbr

    # Write FAT1 and FAT2
    for n in range(num_fats):
        foff = (part_start + reserved + n * fat_sectors) * 512
        img[foff:foff+len(fat)] = fat

    with open(output, 'wb') as f:
        f.write(img)

    actual_mb = os.path.getsize(output) // (1024*1024)
    print(f"[mkfatimg] {output}: {actual_mb} MB, {len(allocs)} files, {len(dir_clusters)} dirs")

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <output.img> <size_mb> <source_dir>")
        sys.exit(1)
    build_image(sys.argv[3], int(sys.argv[2]), sys.argv[1])
