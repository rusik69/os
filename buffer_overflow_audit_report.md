# Buffer Overflow & Unsafe String Handling Audit Report

**Project**: `/home/ubuntu/os/src/` (667 C files)
**Audit Scope**: kernel/syscall.c, drivers/, fs/, net/, lib/, shell/
**Date**: June 2026

---

## EXECUTIVE SUMMARY

Found **6 critical** and **1 medium** buffer overflow/unsafe string handling vulnerabilities. The most widespread issue is unbounded `strcpy` of user-supplied arguments into fixed 64-byte stack buffers across 10+ shell commands. One off-by-one overflow exists in the FAT32 driver. One unbounded while-loop write exists in `chown`. The kernel core (`syscall.c`) and network stack are generally safe due to bounds checking.

---

## CRITICAL FINDINGS

### V-001: cmd_zforce.c — Unbounded strcpy to 128-byte stack buffer

**File**: `src/shell/cmds/cmd_zforce.c` (lines 8-9)
```c
char buf[128];
strcpy(buf, args);  // args is user-supplied, no length check
```
**Impact**: Any argument >127 characters overflows the stack buffer, corrupting the return address and local variables. Trivially exploitable by any user at the shell.

**Fix**: Replace with `strncpy(buf, args, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';`

---

### V-002a through V-002i: Shell commands — Unbounded strcpy to 64-byte path buffer

**Pattern** (paths 64 bytes, user args unbounded):
```c
char path[64];
if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
else strcpy(path, args);
```
**Impact**: User arguments >62 chars (or >63 for absolute paths) overflow the stack. The '/' prefix makes the max safe length 62 chars — exceeding this writes past `path[63]`.

**Affected files** (9 files confirmed dangerous):

| # | File | Lines | Notes |
|---|------|-------|-------|
| V-002a | `src/shell/cmds/cmd_touch.c` | 10-12 | |
| V-002b | `src/shell/cmds/cmd_stat.c` | 10-12 | |
| V-002c | `src/shell/cmds/cmd_mkdir.c` | 10-12 | |
| V-002d | `src/shell/cmds/cmd_wc.c` | 17-19 | |
| V-002e | `src/shell/cmds/cmd_xxd.c` | 10-12 | |
| V-002f | `src/shell/cmds/cmd_run.c` | 10-12 | |
| V-002g | `src/shell/cmds/cmd_chown.c` | 37-39 | Path arg, no bounds |
| V-002h | `src/shell/cmds/cmd_chmod.c` | 30-32 | Path arg, no bounds |
| V-002i | `src/shell/cmds/cmd_paste.c` | 26-30 | Two paths; f1/f2 bounded to 63 chars but still overflows path[64] by 1 byte with '/' prefix |

**Fix**: Replace `strcpy` with `strncpy(path, args, sizeof(path)-1); path[sizeof(path)-1] = '\0'` (or use snprintf).

---

### V-003: cmd_chown.c — Unbounded while-loop to owner[32] and grp[32]

**File**: `src/shell/cmds/cmd_chown.c` (lines 21-31)
```c
char owner[USER_MAX_NAME];  // 32 bytes
int oi = 0;
while (*p && *p != ' ' && *p != ':') owner[oi++] = *p++;
// NO bounds check on oi!
...
char grp[USER_MAX_NAME];    // 32 bytes
int gi = 0;
if (*p == ':') {
    p++;
    while (*p && *p != ' ') grp[gi++] = *p++;
    // NO bounds check on gi!
}
```
**Impact**: A long username or group argument (e.g. a 100-character user string before a space) will overflow both `owner` and `grp` stack buffers.

**Fix**: Add `&& oi < USER_MAX_NAME - 1` to both while-loop conditions.

---

### V-004: editor.c — Weakly bounded strcpy to path[66]

**File**: `src/shell/editor.c` (lines 640-642, 956-958, 977-979) — 3 occurrences
```c
char path[66];
if (b->filename[0] != '/') { path[0] = '/'; strcpy(path + 1, b->filename); }
else strcpy(path, b->filename);
```
**Impact**: `b->filename` is `char[64]`, bounded at 63 chars by `strncpy` at line 636. With '/' prefix = 1+63+0 = 64 bytes → fits in path[66]. However, the strcpy provides no runtime safety; if the strncpy bound were changed or bypassed, overflow results. Fragile design.

**Fix**: Use `snprintf(path, sizeof(path), "%s%s", b->filename[0]=='/'?"":"/", b->filename);`

---

### V-005: fat32.c — Off-by-one strcat overflow after lfn_build_name

**File**: `src/fs/fat32.c` (lines 837-838, 891-892) — 2 occurrences
```c
char *out = names[count];  // names[][FAT32_MAX_NAME=256]
if (lfn_n > 0) {
    lfn_build_name(lfn_parts, lfn_n, out, FAT32_MAX_NAME);
    // Can produce up to 255 chars + NUL at position 255
}
// ...
if (entries[i].attr & FAT32_ATTR_DIRECTORY)
    strcat(out, "/");   // Writes '/' at pos 255, NUL at pos 256 — 1 byte over!
```
**Impact**: When a Long File Name exactly fills the 256-byte buffer (255 chars + NUL), appending "/" writes NUL at byte 256 — a one-byte stack overflow. Exploitable via a crafted FAT32 directory entry with a 255-character LFN.

**Fix**: Check `strlen(out) < FAT32_MAX_NAME - 1` before strcat, or use `strncat`.

---

## MEDIUM FINDINGS

### V-006: cmd_head.c / cmd_tail.c — Off-by-one in path[64]

**Files**: `src/shell/cmds/cmd_head.c` (lines 35-37), `src/shell/cmds/cmd_tail.c` (lines 34-36)

These commands first parse `name[64]` with a 63-char limit, then do:
```c
char path[64];
if (name[0] != '/') { path[0] = '/'; strcpy(path + 1, name); }
```
With a 63-char name: path[0]='/' + 63 chars via strcpy(path+1) → writes to path[1..63] and NUL at path[64] — 1 byte past end.

**Risk**: Low, because name is bounded upstream. Still fragile.

---

## SAFE PATTERNS (Verified)

- **cmd_getty.c** (lines 176-177): `strcpy(home, "/home/")` then `strncat(home, username, sizeof(home)-strlen(home)-1)` — **safe** due to strncat with explicit remaining capacity.
- **kernel/syscall.c** (lines 1202-1208): Bounds-checked memcpy into `ap[128]` — **safe**.
- **kernel/syscall.c** (lines 1967-1976): Character-by-character copy with 255 limit into `kpath[256]` — **safe**.
- **kernel/syscall.c** (lines 1225-1228): `strncpy(cwd, ap, 63)` — **safe**.
- **kernel/module.c** (lines 265-269, 299-309): Bounds-checked memcpy with explicit size limits — **safe**.
- **lib/unistd.c** (lines 48-58): `execvp` checks `seg_len + 1 + strlen(file) + 1 > sizeof(full_path)` before copying — **safe**.
- **drivers/** (all): Use `snprintf` with `sizeof` or `memcpy` with fixed sizes — **safe**.
- **net/** (all): No string functions; `memcpy` used for packet data with calculated sizes — **safe**.
- **gui/gui_widgets.c**: Uses `strncpy` with explicit bounds — **safe**.
- **lib/printf.c**: `sprintf` delegates to `vsnprintf` — **safe**.
- No format string vulnerabilities found.
- No `gets()` calls found.
- All kernel user-space memory access (`syscall_user_read_ok`, etc.) validates ranges — **safe**.

---

## RECOMMENDATIONS

1. **Immediate priority**: Fix V-002a through V-002i (9 files) and V-001 — these are trivially triggered by any shell user.
2. **High priority**: Fix V-003 (chown unbounded while-loops), V-005 (FAT32 off-by-one).
3. **Medium priority**: Fix V-004 (editor.c pattern), V-006 (head/tail off-by-one).
4. **Systemic fix**: Create a `safe_strcpy` macro or wrapper that takes destination size, or audit all 117+ `strcpy` occurrences. Replace with `strlcpy`/`strncpy`/`snprintf` as appropriate.
5. **Build-time enforcement**: Add `-Wformat-truncation` and `-D_FORTIFY_SOURCE=2` to compiler flags if the toolchain supports them.
