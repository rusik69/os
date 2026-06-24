#!/usr/bin/env python3
"""
Fix `return -1` sites to use proper negative errno values.
Analyzes context around each `return -1` to determine the right errno.
"""
import re
import os
import sys

# Target files (relative to src/)
TARGETS = [
    "fs/fs.c", "fs/fat32.c", "fs/ext2.c", "fs/tmpfs.c",
    "fs/procfs.c", "fs/sysfs.c",
    "process/process.c", "process/scheduler.c", "process/users.c",
    "drivers/pci.c", "drivers/nvme.c", "drivers/tpm_tis.c",
    "drivers/floppy.c", "drivers/blockdev.c",
    "net/socket.c", "net/netlink.c", "net/net_ns.c",
]

SRC = "/home/ubuntu/os/src"

def analyze_context(text, pos, max_lines=10):
    """Extract context lines before and after a position."""
    lines = text[:pos].split('\n')
    before = lines[-max_lines:] if len(lines) > max_lines else lines
    after = text[pos:].split('\n')
    after_lines = []
    count = 0
    for line in after:
        after_lines.append(line)
        count += 1
        if count >= max_lines:
            break
    return before, after_lines

def determine_errno(before_lines, after_lines, line_text):
    """
    Determine the correct errno based on context patterns.
    """
    context = '\n'.join(before_lines[-5:] + [line_text] + after_lines[:3])
    ctx_lower = context.lower()
    
    # I/O errors (ata_, blockdev_, read_sector, write_sector, etc.)
    if re.search(r'(ata_read_sectors|ata_write_sectors|blockdev_read|blockdev_write|read_sector|write_sector|ext2_read_block|nvme_|tpm_|floppy_)', context):
        return "-EIO"
    
    # Timeout
    if re.search(r'(timeout|timed? ?out|waited|spinning)', ctx_lower):
        return "-ETIMEDOUT"
    
    # Out of memory
    if re.search(r'(kmalloc|kcalloc|kzalloc|page_alloc|pmm_alloc|vmm_alloc|alloc_frame|!.*alloc|out of memory|oom)', ctx_lower):
        return "-ENOMEM"
    
    # Not found / not exist
    if re.search(r'(not found|not exist|component not found|no such|unknown)', ctx_lower):
        return "-ENOENT"
    
    # Already exists
    if re.search(r'(already exists|exists|eeexist)', ctx_lower):
        return "-EEXIST"
    
    # Bad pointer / null check
    if re.search(r'(!ptr|!buf|!data|!arg|!name|!path|!target|!info|!.* == NULL|null pointer)', ctx_lower):
        return "-EINVAL"
    
    # Null check that looks like input validation
    if re.search(r'(if\s*\(!\s*\w+\))', context):
        var_name = re.findall(r'if\s*\(!\s*(\w+)\)', context)
        if var_name:
            return "-EINVAL"
    
    # Index/range validation
    if re.search(r'(idx < 0|idx >=|out of range|invalid index|bad|invalid param|invalid arg)', ctx_lower):
        return "-EINVAL"
    
    # Bad type
    if re.search(r'(not.*dir|not.*file|type !=|bad type|invalid type)', ctx_lower):
        return "-EINVAL"
    
    # Permission denied
    if re.search(r'(perm|access|denied|forbidden|not allowed)', ctx_lower):
        return "-EACCES"
    
    # Device not ready
    if re.search(r'(not ready|not init|uninitialized|device not|no device)', ctx_lower):
        return "-ENODEV"
    
    # Not supported / not implemented
    if re.search(r'(not support|not impl|not yet|unimplemented|unsupported|no support for|no handler|no callback)', ctx_lower):
        return "-EOPNOTSUPP"
    
    # No space / full table
    if re.search(r'(no space|full|no free slot|exhausted|no more|table full|overflow)', ctx_lower):
        return "-ENOSPC"
    
    # Resource busy
    if re.search(r'(already in use|busy|in use|already bound)', ctx_lower):
        return "-EBUSY"
    
    # Invalid file descriptor
    if re.search(r'(bad fd|invalid fd|bad file descriptor)', ctx_lower):
        return "-EBADF"
    
    # Would block
    if re.search(r'(would block|eagain)', ctx_lower):
        return "-EAGAIN"
    
    # Read-only FS
    if re.search(r'(read.only|rofs)', ctx_lower):
        return "-EROFS"
    
    # Not a directory
    if re.search(r'(not a dir)', ctx_lower):
        return "-ENOTDIR"
    
    # Is a directory
    if re.search(r'(is a dir)', ctx_lower):
        return "-EISDIR"
    
    # Empty
    if re.search(r'(not empty|not.*empty)', ctx_lower):
        return "-ENOTEMPTY"
    
    # Try again
    if re.search(r'(try again|retry)', ctx_lower):
        return "-EAGAIN"
    
    # Symlink loop
    if re.search(r'(symlink|loop|too many|max depth)', ctx_lower):
        return "-ELOOP"
    
    # File too large
    if re.search(r'(too large|> fs_max|maximum size)', ctx_lower):
        return "-EFBIG"
    
    # Name too long
    if re.search(r'(name too long|too long)', ctx_lower):
        return "-ENAMETOOLONG"
    
    # Not a TTY
    if re.search(r'(not a tty)', ctx_lower):
        return "-ENOTTY"
    
    # Parse error / bad format
    if re.search(r'(parse|format|invalid.*value|non.numeric)', ctx_lower):
        return "-EINVAL"
    
    # Address in use
    if re.search(r'(address in use|addr in use)', ctx_lower):
        return "-EADDRINUSE"
    
    # Already connected / not connected
    if re.search(r'(not connect|disconnect)', ctx_lower):
        return "-ENOTCONN"
    
    return None  # Unknown - needs manual review

def process_file(filepath):
    """Process a single file, replacing return -1 with proper errno."""
    with open(filepath, 'r') as f:
        content = f.read()
    
    lines = content.split('\n')
    changes = []
    
    # Find all return -1 occurrences
    pattern = re.compile(r'(\s*)return\s+-1\s*;')
    
    for i, line in enumerate(lines):
        if pattern.search(line):
            # Get context
            start = max(0, i - 5)
            end = min(len(lines), i + 4)
            context_lines = lines[start:end]
            
            before_lines = lines[max(0, i-5):i]
            after_lines = lines[i+1:min(len(lines), i+4)]
            
            errno = determine_errno(before_lines, after_lines, line)
            if errno:
                changes.append((i+1, line, errno))
                # Apply the change
                lines[i] = pattern.sub(r'\1return ' + errno + ';', line)
            else:
                print(f"  UNKNOWN: {filepath}:{i+1}: {line.strip()}")
                # Default to -EIO for I/O-related or -EINVAL for validation
                lines[i] = pattern.sub(r'\1return -EINVAL;', line)
                changes.append((i+1, line, "-EINVAL (default)"))
    
    if changes:
        new_content = '\n'.join(lines)
        with open(filepath, 'w') as f:
            f.write(new_content)
        print(f"{filepath}: {len(changes)} changes")
        for lineno, old, errno in changes:
            print(f"  L{lineno}: {old.strip():40s} -> return {errno};")
    else:
        print(f"{filepath}: no changes")

# Process all files
for target in TARGETS:
    filepath = os.path.join(SRC, target)
    if os.path.exists(filepath):
        print(f"\n{'='*60}")
        print(f"Processing: {target}")
        print(f"{'='*60}")
        process_file(filepath)
    else:
        print(f"\nSKIP (not found): {target}")
