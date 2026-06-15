# Buffer Overflow & Unsafe String Handling Audit Report

**Project**: `/home/ubuntu/os/src/` (667+ C files)
**Audit Scope**: Full tree — kernel/, drivers/, fs/, net/, lib/, shell/, memory/, gui/, compiler/, test/
**Date**: June 2026

---

## EXECUTIVE SUMMARY

### Status of Previous Report (S015)

| ID | Vulnerability | Status | Notes |
|----|---------------|--------|-------|
| V-001 | cmd_zforce.c — unbounded strcpy to buf[128] | **PARTIALLY FIXED** | strcpy→snprintf but residual strcat remains (see NEW-005) |
| V-002a–i | 9 shell commands — unbounded strcpy to path[64] | **ALL FIXED** ✓ | Now use snprintf with sizeof bounds |
| V-003 | cmd_chown.c — unbounded while-loop to owner[32]/grp[32] | **FIXED** ✓ | Bounds checks added: `oi < USER_MAX_NAME-1` |
| V-004 | editor.c — strcpy to path[66] | **FIXED** ✓ | Uses snprintf/strncpy with sizeof |
| V-005 | fat32.c — off-by-one strcat after lfn_build_name | **FIXED** ✓ | strcat removed from fat32.c entirely |
| V-006 | cmd_head.c / cmd_tail.c — off-by-one in path[64] | **FIXED** ✓ | Now uses snprintf with sizeof |

### New Findings Summary

| # | Severity | Type | File:Line | Issue |
|---|----------|------|-----------|-------|
| **NEW-001** | **CRITICAL** | Integer overflow → heap overflow | kernel/io_uring.c:143 | `kmalloc(iov_count * sizeof(struct iovec))` — iov_count user-controlled, no cap, 32-bit mul wraps |
| **NEW-002** | **CRITICAL** | Integer overflow → heap overflow | kernel/io_uring.c:190 | Same pattern as NEW-001 for writev |
| **NEW-003** | **HIGH** | Integer overflow → heap under-allocation | fs/fsck.c:303 | `kmalloc(bgd_blocks * block_size)` — both from on-disk superblock |
| **NEW-004** | **HIGH** | Integer overflow | fs/luks.c:212 | `kmalloc(key_bytes * stripes)` — stripes uint32_t from LUKS header, key_bytes ≤128 |
| **NEW-005** | **MEDIUM** | strcat buffer overflow | shell/cmds/cmd_zforce.c:11 | `strcat(buf, ".gz")` after snprintf truncates — 128-byte buf can be overrun by 3 bytes |
| **NEW-006** | **MEDIUM** | strcpy no bounds check | memory/cma.c:33 | `strcpy(area->name, name)` — name[32], no length limit, currently safe callers |
| **NEW-007** | **MEDIUM** | Integer overflow | net/af_packet.c:531 | `kmalloc(sizeof(void *) * ring->frame_count)` — frame_count user-controlled |
| **NEW-008** | **LOW** | strcpy no bounds check | gui/gui_widgets.c:327 | `strcpy(button->label, label)` — label[64], caller may exceed |
| **NEW-009** | **LOW** | Integer overflow (bounded) | kernel/swap.c:222 | `kmalloc(n_words * sizeof(uint64_t))` — n_words from on-disk, partial validation |

### Safe (Verified) — No issues found in:
- **sprintf**: Only `lib/printf.c:529` which delegates to vsnprintf — safe
- **scanf**: No `scanf` calls found with field-width issues
- **gets**: No `gets()` calls found anywhere
- **syscall.c**: All memcpy operations bounded with explicit limits (63, 255 bytes)
- **drivers/**: memcpy with fixed struct sizes or bounded lengths — generally safe
- **net/**: memcpy for packet data with calculated sizes — safe
- **lib/string.c**: strcpy/strcat implementations themselves — safe
- **lib/unistd.c:57**: `strcpy(full_path + seg_len + 1, file)` — guarded by size check on line 53
- **gui/gui_widgets.c:90**: `strcpy(full + fl, names[i])` — guarded by length check on line 88
- **gui/gui_widgets.c:172**: `strcpy(fb->selected, fb->files[idx].name)` — selected[256] > name[64], safe

---

## DETAILED FINDINGS

---

### NEW-001 / NEW-002: Integer overflow in io_uring READV/WRITEV (CRITICAL)

**Files**: `src/kernel/io_uring.c` lines 143, 190
```c
// Lines 139-147 (readv)
uint32_t iov_count = sqe->len;     // User-controlled, NO upper bound check
if (iov_count == 0) { ... break; } // Only checks for zero
struct iovec *iov = (struct iovec *)kmalloc(iov_count * sizeof(struct iovec));
// 32-bit multiplication: iov_count (uint32_t) * 16 (sizeof iovec) can wrap to 0
// kmalloc(0) returns ZERO_SIZE_PTR or small allocation
memcpy(iov, (void *)(uintptr_t)sqe->addr, iov_count * sizeof(struct iovec));
// Then copies the full 64-bit size into the tiny buffer → heap overflow
```

**Trigger**: Set `sqe->len = 0x10000000` (268,435,456). Then `iov_count * 16 = 0x100000000` wraps to 0 in 32-bit. `kmalloc(0)` returns a small/heap-adjacent pointer. `memcpy` copies 4GB from user space → **immediate heap corruption**.

**Same pattern** at line 190 for `IORING_OP_WRITEV`.

**Fix**: Add a cap like `if (iov_count > 1024) iov_count = 1024;` before the kmalloc, or check for overflow: `if (iov_count > SIZE_MAX / sizeof(struct iovec)) return -EINVAL;`.

---

### NEW-003: Integer overflow in fsck block group descriptor allocation (HIGH)

**File**: `src/fs/fsck.c` line 303
```c
uint32_t bgd_blocks = (num_groups * sizeof(struct ext2_bg_desc)
                       + block_size - 1) / block_size;  // From on-disk superblock
uint8_t *bgd_buf = (uint8_t *)kmalloc(bgd_blocks * block_size);
//                                   ^^^^^^^^^^ ^^^^^^^^^^ both from disk
```

Both `bgd_blocks` and `block_size` are derived from the on-disk ext2 superblock (`num_groups` and `block_size`). A crafted filesystem can set these to values where `bgd_blocks * block_size` overflows a 32-bit multiplication, resulting in a small buffer allocation. Subsequent I/O operations fill past the buffer.

**Fix**: Use `size_mul()` helper or check: `if (bgd_blocks > SIZE_MAX / block_size) return -EINVAL;`

---

### NEW-004: Integer overflow in LUKS key material allocation (HIGH)

**File**: `src/fs/luks.c` line 212
```c
uint32_t key_bytes = hdr->key_bytes;   // Capped at 128 (line 205)
uint32_t stripes   = ks->stripes;       // From LUKS keyslot header, no upper bound!
key_material = (uint8_t *)kmalloc(key_bytes * stripes);
//                                ^^^^^^^^^ ^^^^^^^^ uint32_t × uint32_t → 32-bit overflow
```

With `key_bytes = 128` and a crafted `stripes = 0x02000000` (33 million), the product `0x100000000` wraps to 0. kmalloc(0) + subsequent I/O reads into the buffer → **heap overflow**.

Line 229 compounds this:
```c
uint32_t km_sectors = (uint32_t)((key_bytes * stripes + 511) / 512);
```

**Fix**: Reject `stripes > SIZE_MAX / key_bytes` or `stripes > 65536`.

---

### NEW-005: Residual strcat overflow in cmd_zforce.c (MEDIUM)

**File**: `src/shell/cmds/cmd_zforce.c` line 11
```c
char buf[128];
snprintf(buf, sizeof(buf), "%s", args);  // Truncates to 127 + NUL
// ...
if (!dot) { strcat(buf, ".gz"); }        // Writes 4 bytes (".gz" + NUL) at buf+127+
```

If `args` is a 125+ character filename without a dot, snprintf writes 127 bytes + NUL. Then strcat writes `".gz\0"` at positions 127–130, which is **3 bytes past** the 128-byte buffer (buf[127]–buf[130] are out of bounds).

**Exploitation**: Any shell user can trigger this via `zforce <125+ chars without dot>`.

**Fix**: Use `snprintf(buf + len, sizeof(buf) - len, ".gz")` after finding the length, or check `strlen(buf) + 3 < sizeof(buf)` before strcat.

---

### NEW-006: strcpy without bounds in CMA name copy (MEDIUM)

**File**: `src/memory/cma.c` line 33
```c
struct cma_area {
    // ...
    char name[32];             // 32-byte buffer
};
strcpy(area->name, name);      // No bounds check on `name`
```

Currently the only callers use short literal strings (`"test"`, `"default"`), so not exploitable today. But a future caller passing a long name (or user-controlled name) will overflow the stack-allocated `cma_areas[]`.

**Fix**: Use `strncpy(area->name, name, sizeof(area->name) - 1); area->name[sizeof(area->name) - 1] = '\0';`

---

### NEW-007: Integer overflow in AF_PACKET frame pointer array (MEDIUM)

**File**: `src/net/af_packet.c` line 531
```c
ring->frame_count = req->tp_frame_nr;  // User-controlled (setsockopt)
ring->frames = (volatile struct tpacket_hdr **)
    kmalloc(sizeof(void *) * ring->frame_count);
//          8 (on 64-bit) × frame_count → could overflow
```

A large `tp_frame_nr` value causes the multiplication to overflow, leading to undersized allocation.

**Fix**: Check `frame_count > SIZE_MAX / sizeof(void *)` before allocating.

---

### NEW-008: Unbounded strcpy in taskbar button label (LOW)

**File**: `src/gui/gui_widgets.c` line 327
```c
struct gui_taskbar {
    struct {
        gui_widget_t *btn;
        char label[64];
    } buttons[16];
};
strcpy(tb->buttons[tb->button_count].label, label);  // No length check
```

The `label` parameter comes from callers of `gui_taskbar_add_button()`. If any caller passes a string > 63 characters, the label[64] buffer overflows. Currently callers use short window titles.

**Fix**: Replace with `strncpy(tb->buttons[tb->button_count].label, label, sizeof(tb->buttons[0].label) - 1);`

---

### NEW-009: Integer overflow in swap bitmap allocation (LOW)

**File**: `src/kernel/swap.c` line 222
```c
uint32_t n_words = SWAP_BITMAP_SIZE(sb.total_slots);  // From on-disk swap header
uint64_t *bitmap = (uint64_t *)kmalloc(n_words * sizeof(uint64_t));
```

`total_slots` is read from the on-disk swap header. Although version and checksum validation exist, no explicit upper bound on `total_slots`. A crafted swap image with huge `total_slots` could cause `n_words * 8` to overflow.

**Fix**: Cap `total_slots` at a reasonable maximum (e.g., swap device size / page size).

---

## PATTERNS FOUND — STATISTICAL SUMMARY

| Pattern | Occurrences | Safe | Unsafe | Notes |
|---------|-------------|------|--------|-------|
| `strcpy` | 35 | 28 | 2 (CMA, gui_widgets taskbar) | Rest are constant strings or bounded |
| `strcat` | 2 (1 impl, 1 caller) | 1 | 1 (cmd_zforce.c:11) | Implementation safe, caller overflows |
| `sprintf` | 1 | 1 | 0 | Delegates to vsnprintf — safe |
| `gets` | 0 | — | — | No occurrences — good |
| `scanf` | 0 | — | — | No occurrences — good |
| `kmalloc(a*b)` | 18 | 12 | 6 | 3 critical, 1 high, 1 medium, 1 low |
| `memcpy` to fixed buffer | ~200 | ~195 | ~5 evaluated | Most bounded; io_uring cases are real |

---

## RECOMMENDATIONS

1. **IMMEDIATE**: Fix NEW-001/NEW-002 (io_uring integer overflow) — add upper bound check on `iov_count`.
2. **IMMEDIATE**: Fix NEW-003 (fsck) and NEW-004 (LUKS) — add overflow checks before kmalloc.
3. **HIGH**: Fix NEW-005 (cmd_zforce strcat residual) — use snprintf append instead of strcat.
4. **HIGH**: Fix NEW-007 (AF_PACKET integer overflow) — bounds check on frame_count.
5. **MEDIUM**: Fix NEW-006 (CMA strcpy) and NEW-008 (gui taskbar strcpy) — replace with strncpy.
6. **LOW**: Fix NEW-009 (swap bitmap) — cap total_slots.
7. **SYSTEMIC**: Introduce a `kmalloc_array()` helper that checks for overflow (like Linux kernel's `kmalloc_array`). Currently there are 18 `kmalloc(a*b)` patterns, 6 of which are potentially exploitable.
8. **BUILD-TIME**: Consider `-D_FORTIFY_SOURCE=2` if toolchain supports `__builtin_object_size` and `__builtin___memcpy_chk` — this catches io_uring-style memcpy overflows at runtime.
