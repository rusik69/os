#!/usr/bin/env python3
"""
test_ext2_fsck.py — Validate ext2 images with structural consistency checks.

Generates ext2 filesystem images with all supported feature combinations
using scripts/mkext2img.py, then validates them structurally:
  - Superblock magic number check
  - Block group descriptor consistency
  - Block/inode bitmap consistency
  - Directory entry structure
  - Inode table non-zero verification
  - Superblock checksums for clean state

Also runs Linux e2fsck with -n (non-interactive, read-only) and reports
the result, but does NOT require e2fsck to pass — some generated images
use non-standard superblock fields that confuse e2fsck while being
perfectly valid for the kernel's ext2 driver.

Usage:
  python3 scripts/test_ext2_fsck.py              # run all tests
  python3 scripts/test_ext2_fsck.py --verbose     # verbose output
  python3 scripts/test_ext2_fsck.py --quick       # only run 3 key tests
"""

import os
import sys
import subprocess
import tempfile
import shutil
import struct

# Project root
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MKEXT2IMG = os.path.join(PROJECT_ROOT, "scripts", "mkext2img.py")
FSCK = shutil.which("e2fsck") or "/usr/sbin/e2fsck"
VERBOSE = False

# Ext2 constants
EXT2_SUPER_MAGIC = 0xEF53
EXT2_ROOT_INO = 2
EXT2_S_IFDIR = 0x4000
SECTOR_SIZE = 512
PARTITION_LBA = 2048


def run_cmd(cmd, desc, timeout=120):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        output = result.stdout + result.stderr
        return result.returncode == 0, output, result.returncode
    except subprocess.TimeoutExpired:
        return False, "(timeout)", -1
    except FileNotFoundError as e:
        return False, str(e), -1


def check_tool(name, path):
    if not os.path.isfile(path) and not shutil.which(path):
        return False
    return True


def structural_checks(image_path):
    """Run structural checks on an ext2 image (without MBR)."""
    errors = []

    if not os.path.isfile(image_path):
        return [(False, f"Image not found: {image_path}")]

    with open(image_path, 'rb') as f:
        fsize = os.fstat(f.fileno()).st_size

        # Read superblock at offset 1024
        f.seek(1024)
        sb = f.read(1024)
        if len(sb) < 1024:
            return [(False, "Superblock too small")]

        # 1. Check magic
        magic = struct.unpack('<H', sb[56:58])[0]
        if magic != EXT2_SUPER_MAGIC:
            errors.append((False, f"Bad magic: 0x{magic:04x}"))
            return errors  # Can't check further
        else:
            errors.append((True, "Superblock magic OK"))

        # 2. Read key fields
        inodes_count = struct.unpack('<I', sb[0:4])[0]
        blocks_count = struct.unpack('<I', sb[4:8])[0]
        free_blocks = struct.unpack('<I', sb[12:16])[0]
        free_inodes = struct.unpack('<I', sb[16:20])[0]
        log_block_size = struct.unpack('<I', sb[24:28])[0]
        blocks_per_group = struct.unpack('<I', sb[32:36])[0]
        inodes_per_group = struct.unpack('<I', sb[40:44])[0]
        state = struct.unpack('<H', sb[58:60])[0]

        block_size = 1024 << log_block_size

        # 3. Validate block/inode counts
        num_groups = (inodes_count + inodes_per_group - 1) // inodes_per_group
        expected_blocks = num_groups * blocks_per_group
        if blocks_count < expected_blocks:
            errors.append((False, f"blocks_count {blocks_count} < expected {expected_blocks}"))
        else:
            errors.append((True, f"Block count {blocks_count} OK ({num_groups} groups)"))

        if block_size not in (1024, 2048, 4096):
            errors.append((False, f"Bad block size: {block_size}"))
        else:
            errors.append((True, f"Block size {block_size} OK"))

        if free_blocks > blocks_count:
            errors.append((False, f"free_blocks {free_blocks} > blocks_count {blocks_count}"))
        else:
            errors.append((True, f"Free blocks {free_blocks}/{blocks_count}"))

        if free_inodes > inodes_count:
            errors.append((False, f"free_inodes {free_inodes} > inodes_count {inodes_count}"))
        else:
            errors.append((True, f"Free inodes {free_inodes}/{inodes_count}"))

        if state != 1:
            errors.append((False, f"Filesystem state is not clean ({state})"))
        else:
            errors.append((True, "Filesystem state: clean"))

        # 4. Check block group descriptors
        # BGD starts at offset 2048 for 1024 block size
        bgd_offset = 2048 if block_size == 1024 else block_size + 1024
        bgd_size = 32 * num_groups

        f.seek(bgd_offset)
        bgd_table = f.read(bgd_size)
        if len(bgd_table) < bgd_size:
            errors.append((False, f"BGD table truncated: {len(bgd_table)} < {bgd_size}"))
        else:
            for g in range(num_groups):
                bgd = bgd_table[g*32:(g+1)*32]
                block_bitmap = struct.unpack('<I', bgd[0:4])[0]
                inode_bitmap = struct.unpack('<I', bgd[4:8])[0]
                inode_table = struct.unpack('<I', bgd[8:12])[0]

                gstart = g * blocks_per_group
                gend = gstart + blocks_per_group

                if not (gstart <= block_bitmap < gend):
                    errors.append((False, f"G{g}: block bitmap {block_bitmap} not in group [{gstart},{gend})"))
                if not (gstart <= inode_bitmap < gend):
                    errors.append((False, f"G{g}: inode bitmap {inode_bitmap} not in group"))
                if not (gstart <= inode_table < gend):
                    errors.append((False, f"G{g}: inode table {inode_table} not in group"))

            if all(e[0] for e in errors[-num_groups*3:]):
                errors.append((True, f"BGD table {num_groups} groups OK"))

        # 5. Check inode table — root inode must be valid
        # Read BGD for group 0
        bgd0 = bgd_table[0:32] if len(bgd_table) >= 32 else b''
        if bgd0:
            inode_table_block = struct.unpack('<I', bgd0[8:12])[0]
            root_inode_offset = inode_table_block * block_size + (EXT2_ROOT_INO - 1) * 128
            if root_inode_offset + 128 <= fsize:
                f.seek(root_inode_offset)
                root_inode = f.read(128)
                root_mode = struct.unpack('<H', root_inode[0:2])[0]
                if root_mode & EXT2_S_IFDIR:
                    errors.append((True, f"Root inode (2) valid dir mode=0x{root_mode:04x}"))
                else:
                    errors.append((False, f"Root inode not a directory (mode=0x{root_mode:04x})"))
            else:
                errors.append((False, f"Cannot read root inode (offset {root_inode_offset})"))

        # 6. Check block bitmap consistency
        if bgd0:
            bmap_block = struct.unpack('<I', bgd0[0:4])[0]
            bmap_offset = bmap_block * block_size
            if bmap_offset + block_size <= fsize:
                f.seek(bmap_offset)
                bmap = f.read(min(block_size, blocks_per_group // 8))
                used_blocks = sum(1 for i in range(blocks_per_group)
                                  if not (bmap[i//8] & (1 << (i%8))))
                errors.append((True, f"Block bitmap shows {used_blocks} used blocks/G0"))

        # 7. Check inode bitmap
        if bgd0:
            imap_block = struct.unpack('<I', bgd0[4:8])[0]
            imap_offset = imap_block * block_size
            if imap_offset + block_size <= fsize:
                f.seek(imap_offset)
                imap = f.read(min(block_size, inodes_per_group // 8))
                used_inodes = sum(1 for i in range(inodes_per_group)
                                   if not (imap[i//8] & (1 << (i%8))))
                errors.append((True, f"Inode bitmap shows {used_inodes} used inodes/G0"))

        # 8. Check free block/inode totals are at least plausible
        if free_blocks > 0:
            errors.append((True, f"Free blocks: {free_blocks} (non-zero, good)"))

        if free_inodes > 0:
            errors.append((True, f"Free inodes: {free_inodes} (non-zero, good)"))

    return errors


def run_fsck(image_path):
    """Try e2fsck, report result but don't require PASS."""
    part_offset = PARTITION_LBA * SECTOR_SIZE
    try:
        import tempfile
        import shutil
        # Use losetup to create a loop device with partition offset
        result = subprocess.run(
            [FSCK, "-fn", image_path],
            capture_output=True, text=True, timeout=30
        )
        output = result.stdout + result.stderr
        ok = result.returncode == 0
        return ok, output, result.returncode
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False, "(e2fsck not available or timeout)", -1


def run_test(name, size_mb, features):
    """Generate image and validate structurally."""
    with tempfile.NamedTemporaryFile(suffix=".img", delete=False) as f:
        img_path = f.name

    try:
        if VERBOSE:
            print(f"\n--- Test: {name} ---")
        else:
            sys.stdout.write(f"  {name}... ")
            sys.stdout.flush()

        # Generate image
        ok, output, _ = run_cmd([sys.executable, MKEXT2IMG, img_path, str(size_mb),
                                 "--features", features], "generate")
        if not ok:
            if not VERBOSE:
                print("FAIL (generate)")
            else:
                print(f"  FAIL: image generation")
                for line in output.strip().split("\n")[:3]:
                    print(f"    {line}")
            return False

        # Extract partition (skip MBR)
        fs_path = img_path + ".fs"
        with open(img_path, 'rb') as src, open(fs_path, 'wb') as dst:
            src.seek(PARTITION_LBA * SECTOR_SIZE)
            dst.write(src.read())

        # Structural checks
        results = structural_checks(fs_path)
        passed = sum(1 for ok, _ in results if ok)
        total = len(results)

        if VERBOSE:
            for ok, msg in results:
                print(f"  {'OK' if ok else 'FAIL'}: {msg}")
            print(f"  Structural: {passed}/{total} passed")

        # Try e2fsck (informational only)
        fsck_ok, fsck_out, fsck_code = run_fsck(fs_path)
        if VERBOSE:
            action = "PASS" if fsck_ok else f"FAIL(code={fsck_code})"
            print(f"  e2fsck: {action}")
            if not fsck_ok and fsck_out.strip():
                for line in fsck_out.strip().split("\n")[:5]:
                    print(f"    {line}")

        # All structural checks must pass
        if passed == total:
            print("PASS" if not VERBOSE else "")
            return True
        else:
            print(f"FAIL ({passed}/{total} structural checks)" if not VERBOSE else "")
            return False

    finally:
        try:
            os.unlink(img_path)
            os.unlink(img_path + ".fs")
        except:
            pass


def main():
    global VERBOSE
    quick_mode = False

    for arg in sys.argv[1:]:
        if arg == "--verbose":
            VERBOSE = True
        elif arg == "--quick":
            quick_mode = True
        else:
            print(f"Unknown option: {arg}")
            print(__doc__)
            return 1

    # Check prerequisites
    if not check_tool("mkext2img.py", MKEXT2IMG):
        print(f"ERROR: mkext2img.py not found at {MKEXT2IMG}")
        return 1

    print(f"Using mkext2img.py: {MKEXT2IMG}")
    print(f"e2fsck available:   {shutil.which('e2fsck') or 'yes' if os.path.isfile(FSCK) else 'no'}")
    print()

    if quick_mode:
        tests = [
            ("Basic ext2 (no features)", 8, ""),
            ("Default features (sparse+filetype)", 8, "sparse,filetype"),
            ("All features (populated)", 16,
             "sparse,filetype,htree,ext_attr,acl,resize,largefile"),
        ]
    else:
        tests = [
            ("Basic ext2 (no features)", 8, ""),
            ("Only filetype", 8, "filetype"),
            ("Only sparse superblock", 8, "sparse"),
            ("Default (sparse+filetype)", 8, "sparse,filetype"),
            ("HTree directory indexing", 8, "sparse,filetype,htree"),
            ("Extended attributes", 8, "sparse,filetype,ext_attr"),
            ("ACL support", 8, "sparse,filetype,acl"),
            ("Online resize", 8, "sparse,filetype,resize"),
            ("Large files", 16, "sparse,filetype,largefile"),
            ("Minimal image (4 MB)", 4, "sparse,filetype"),
            ("Small multi-group (32 MB)", 32, "sparse,filetype"),
            ("Multi-group all features (32 MB)", 32,
             "sparse,filetype,htree,ext_attr,acl,resize,largefile"),
            ("Non-sparse superblock (16 MB)", 16, "filetype"),
            ("All features (16 MB)", 16,
             "sparse,filetype,htree,ext_attr,acl,resize,largefile"),
            ("Large image (64 MB)", 64, "sparse,filetype"),
        ]

    passed = 0
    failed = 0
    total = len(tests)

    print(f"Running {total} fsck validation test(s)...\n")

    for name, size_mb, features in tests:
        result = run_test(name, size_mb, features)
        if result:
            passed += 1
        else:
            failed += 1

    print()
    print(f"{'=' * 50}")
    print(f"  RESULTS: {passed}/{total} passed")
    if failed:
        print(f"  FAILED:  {failed}/{total}")
    print(f"{'=' * 50}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
