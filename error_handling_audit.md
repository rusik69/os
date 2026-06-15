# Error Handling Completeness Audit

## Scope
Audit of `/home/ubuntu/os/src/` with sampling across 6 subsystems: kernel/, drivers/, fs/, net/, ipc/, memory/.  
Approximately 15 files per subsystem examined (≈90 files total). 1453 `.c`/`.h` files in the tree.

---

## Pattern 1: Functions returning `int` that use `-1` instead of proper errno

### Widespread issue — `-1` used as universal error indicator

Many functions return `-1` for all error types instead of distinct errno values (`-ENOMEM`, `-EINVAL`, `-EACCES`, etc.). This makes it impossible for callers to distinguish between different failure modes.

| File | Lines | Issue |
|---|---|---|
| `src/kernel/tpm_key.c` | 73, 84, 98, 247, 265, 268 | All error paths return `-1` instead of `-ENOMEM` or `-EINVAL` |
| `src/kernel/elf.c` | 161, 179, 185, 198 | `kmalloc` failure and ELF load failures return `-1` not `-ENOMEM` |
| `src/kernel/seccomp.c` | 236, 238, 242, 249, 258 | Returns `-1` on allocation failure and invalid modes — should be `-ENOMEM`/`-EINVAL` |
| `src/ipc/pipe.c` | 22, 27, 43, 48, 49, 272, 276 | All error paths use `-1` — `kmalloc` failures should be `-ENOMEM`, invalid args `-EINVAL` |
| `src/ipc/inotify.c` | 95, 117, 127, 137 | Slot exhaustion returns `-1` — should be `-ENFILE` or `-ENOSPC` |
| `src/ipc/eventfd.c` | 40, 54, 60, 78, 91, 100 | Returns `-1` for all errors (including `kmalloc` failure, overflow, would-block) |
| `src/ipc/signalfd.c` | 61, 64, 89, 92, 120 | Returns `-1` for initialization check and allocation failures |
| `src/ipc/mqueue.c` | 47, 51, 58, 61 | Returns `-1` for table-full, uninitialized, O_EXCL conflicts |
| `src/ipc/shm.c` | 91, 101, 128 | Permission denied and allocation failure both return `-1` |
| `src/ipc/fifo.c` | 17, 22, 30, 34, 43 | All errors (exists, full, pipe_create fail) return `-1` |
| `src/ipc/timerfd.c` (drivers/) | Various | Returns `-1` instead of errno values |
| `src/net/tun.c` | 13, 21, 22, 36, 50 | All error conditions return `-1` |
| `src/drivers/nbd.c` | 35, 38, various | All errors use `-1` |
| `src/kernel/module_deps.c` | 65, 85, 110, 151, 174, 182, 238, 243, 269, 299 | All errors return `-1` — dependency resolution, cycle detection, parsing failures |

### Subsystems that DO use proper errno (good practice)

| File | Lines | Practice |
|---|---|---|
| `src/kernel/bpf_maps.c` | 109, 124, 139, 217 | Returns `-ENOMEM` on allocation failure |
| `src/kernel/firmware.c` | 81 | Returns `-ENOMEM` |
| `src/kernel/overlay.c` | 144 | Returns `-ENOMEM` |
| `src/kernel/hashtable.c` | 46 | Returns `-ENOMEM` |
| `src/drivers/dm-crypt.c` | 95 | Returns `-ENOMEM` |
| `src/drivers/firmware_class.c` | 91 | Returns `-ENOMEM` |
| `src/drivers/loop.c` | 142 | Returns `-ENOMEM` |
| `src/drivers/bonding.c` | 456 | Returns `-ENOMEM` |
| `src/fs/ext2.c` | 910, 957, 1039, 1045, 1074 | Returns `-ENOMEM` consistently |
| `src/fs/fuse.c` | 140 | Returns `-ENOMEM` |
| `src/fs/fsck.c` | 307 | Returns `-ENOMEM` |
| `src/net/af_packet.c` | 81, 511, 536 | Returns `-ENOMEM` |
| `src/net/socket.c` | 829, 834 | Returns `-ENOMEM` |
| `src/memory/page_pool.c` | 83 | Returns `-ENOMEM` |
| `src/memory/damon.c` | 46 | Returns `-ENOMEM` |
| `src/memory/zbud.c` | 42, 93 | Returns `-ENOMEM` |
| `src/kernel/vfs.c` | 637, 653 | Uses `-EACCES`, `-EIO`, `-EFSCORRUPTED` |

### Summary — IPC is the worst offender
The entire `ipc/` subsystem (pipe.c, inotify.c, eventfd.c, signalfd.c, mqueue.c, shm.c, fifo.c) uses **only `-1`** for error returns — zero use of `-ENOMEM`, `-EINVAL`, or any named errno. Callers cannot distinguish OOM from invalid parameters.

---

## Pattern 2: kmalloc/kzalloc returning NULL as success (pointer functions) or `-1` instead of `-ENOMEM` (int functions)

### Int functions that return `-1` after kmalloc failure (should be `-ENOMEM`)

| File | Line | Code |
|---|---|---|
| `src/kernel/elf.c` | 161 | `if (!buf) return -1;` |
| `src/kernel/tpm_key.c` | 73-84 | Multiple `return -1` after kmalloc failures |
| `src/ipc/pipe.c` | 21-22 | `if (!pipe_table[i].buf) return -1;` |
| `src/ipc/pipe.c` | 24-27 | `if (!pipe_table[i].buf2) ... return -1;` |
| `src/ipc/pipe.c` | 270-272 | `if (!new_buf) return -1;` |
| `src/ipc/fifo.c` | 34 | `if (pipe_id < 0) return -1;` — pipe_create failure masked |
| `src/fs/tmpfs.c` | 477 | `if (tmpfs_mounted) return -1;` — already mounted, should be `-EBUSY` |
| `src/drivers/spi.c` | 66-70 | kmalloc failure for master — returns NULL from pointer function (expected) |
| `src/fs/cramfs.c` | Various | Pointer-returning functions return NULL on kmalloc fail (correct for pointer type) |

### Pointer functions returning NULL on kmalloc failure (correct but downstream callers may not check)

| File | Lines | Function |
|---|---|---|
| `src/kernel/ssh_client.c` | 360 | `ssh_client_connect` — `if(!cl) { ... return NULL; }` |
| `src/kernel/module.c` | 504, 527, 563, 635 | Module lookup returns NULL |
| `src/kernel/lockdep.c` | 122, 133, 315 | `lockdep_get_lock_class` returns NULL |
| `src/net/af_unix.c` | 262, 264 | `conn_alloc` returns NULL |
| `src/net/ipvs.c` | 48 | `ip_vs_conn_lookup` returns NULL |
| `src/memory/slab.c` | 430 | `kmem_cache_create` returns NULL |
| `src/memory/heap.c` | 75-80 | `kmalloc` itself returns NULL |

---

## Pattern 3: Callers that ignore return values via `(void)` casts

| File | Line | Discarded call | Risk |
|---|---|---|---|
| `src/kernel/syscall.c` | 8128 | `(void)vfs_readahead(...)` | Read-ahead failure silently ignored; may cause page faults on read |
| `src/fs/fs.c` | 513 | `(void)ata_write_sectors(...)` | **Disk write failure silently ignored** — data corruption risk |
| `src/drivers/fcoe.c` | 84 | `(void)fcoe_open_socket()` | Socket open failure silently ignored — FCoE init proceeds with dead socket |
| `src/drivers/ipmi_kcs.c` | 98 | `(void)kcs_read_data(base)` | KCS read failure silently consumed |
| `src/drivers/nvme.c` | 892 | `(void)nvme_read32(...)` | NVMe register read consumed — may mask hardware faults |
| `src/drivers/virtio_net.c` | 172 | `(void)vio_inb(VIRTIO_PCI_ISR)` | ISR status read discard — typically OK for status-clear on read, but masks missing device |
| `src/kernel/rng.c` | 102, 148, 153 | `(void)rng_get_u64()` / `(void)xorshift64(...)` | RNG state consumption discard — seeds not advanced? |
| `src/shell/cmds/cmd_cc.c` | 150 | `(void)cc_compile_one(...)` | Compilation failure silently ignored |

### Notable: `(void)vfs_readahead` in `syscall.c:8128`
This discards the return value of a VFS operation that can fail (I/O error, out-of-memory). The readahead hint is advisory, but the error could indicate a deeper filesystem issue.

### Notable: `(void)ata_write_sectors` in `fs/fs.c:513`
**Highest risk finding.** The return value from a disk write is discarded. If the write fails (bad sector, device error), there is no retry, no fallback, and no error propagation. Data corruption is possible.

---

## Pattern 4: Void functions that allocate resources or manipulate state but cannot report failures

### Subsystem initialization functions (`void *_init(void)`)

Approximately **60+** functions across the codebase use `void` return for initialization that could fail:

| File | Function | Description |
|---|---|---|
| `src/kernel/lockdep.c:715` | `lockdep_init(void)` | Allocates lock class tracking — OOM silently tolerated |
| `src/kernel/taskstats.c:76` | `taskstats_init(void)` | Sets up taskstats — no error return |
| `src/kernel/kprobes.c:314` | `kprobes_init(void)` | Clears probe table — fine, but allocation not attempted here |
| `src/kernel/cpuset.c:22` | `cpuset_init(void)` | Initializes cpuset — no failure possible currently |
| `src/kernel/yama.c:23` | `yama_init(void)` | Sets ptrace scope — fine |
| `src/kernel/seccomp.c:37` | `seccomp_init(void)` | Logs message only — fine |
| `src/kernel/devtmpfs.c:15` | `devtmpfs_init(void)` | Initializes device table — memsets only |
| `src/kernel/memfd.c:11` | `memfd_init(void)` | Initializes memfd array — allocates no memory |
| `src/drivers/firmware_class.c:48` | `firmware_class_init(void)` | Calls `firmware_init()` which could fail, but return is void |
| `src/drivers/rtc.c:550` | `rtc_sysfs_init(void)` | **Creates sysfs entries** — sysfs creation can fail (ENOMEM, ENOSPC), but no error returned |
| `src/drivers/vmw_balloon.c:119` | `vmw_balloon_init(void)` | **Detects VMware backdoor** — detection can fail, silently returns |
| `src/drivers/spi.c:34` | `spi_init(void)` | Initializes SPI subsystem — fine |
| `src/drivers/hpet.c:9` | `hpet_init(void)` | Probes HPET MMIO — could fault, no error return |
| `src/ipc/mqueue.c:18` | `mqueue_init(void)` | Initializes message queues — memsets only |
| `src/ipc/shm.c:69` | `shm_init(void)` | Initializes shared memory — memsets only |
| `src/ipc/fifo.c:11` | `fifo_init(void)` | Initializes FIFOs — memsets only |
| `src/ipc/pipe.c:12` | `pipe_init(void)` | Initializes pipes — memsets only |
| `src/memory/compaction.c:52` | `compaction_init(void)` | Initializes compaction — fine |
| `src/memory/zbud.c:157` | `zbud_init(void)` | Initializes zbud pool — no allocation |
| `src/memory/zswap.c:103` | `zswap_init(void)` | **Allocates compression streams** — kmalloc at line 127, if it fails only a kprintf is emitted, no error propagated |
| `src/fs/tmpfs.c:516` | `tmpfs_init(void)` | Calls `tmpfs_mount()` — mount returns int, but init discards it via void |
| `src/fs/crypto.c:7` | `crypto_init(void)` | Sets dummy AES key — fine |
| `src/fs/jffs2.c:92` | `jffs2_init(void)` | No allocation — fine |

### Notable: `drivers/rtc.c:rtc_sysfs_init()` (line 550)
Void function that creates `/sys/class/rtc/`, `/sys/class/rtc/rtc0/`, and `wakealarm` via sysfs operations. Sysfs creation can return errors (allocation failure, name collision, limit reached), but this function cannot report them.

### Notable: `memory/zswap.c:zswap_init()` (line 103)
Calls `kmalloc` for compression streams at line 127. On failure it prints `[zswap] ERROR: failed to allocate compression streams` but returns void — the system continues with a partially initialized zswap.

### Notable: `fs/tmpfs.c:tmpfs_init()` (line 516)
Calls `tmpfs_mount()` which returns an `int`, but `tmpfs_init()` is `void`, unconditionally discarding the mount result.

---

## Pattern 5: Panic-on-error patterns that should return errors instead

| File | Line | Panic message | Reasoning |
|---|---|---|---|
| `src/memory/pmm.c` | 730 | `panic("[PMM] Out of memory — OOM killer and compaction failed to reclaim any frames")` | **Severe.** This is in the general allocation path. Callers that handle allocation failure gracefully (e.g., returning -ENOMEM to userspace) never get the chance — the kernel panics first. A returning error would allow graceful degradation. |
| `src/memory/pmm.c` | 923 | `panic("[PMM] Out of memory — cannot allocate %llu contiguous frames", ...)` | Same as above — contiguous allocation panics instead of returning NULL/-ENOMEM |
| `src/kernel/hung_task.c` | 97 | `panic("Hung task detected")` | Configurable via `sysctl` — acceptable for hung task detection |
| `src/kernel/kprobes.c` | 448 | `panic("Unhandled INT3 in kernel")` | Acceptable — unexpected INT3 indicates corruption or malicious action |

### Analysis: `pmm.c` OOM panic
The Physical Memory Manager in `pmm.c` tries 3 recovery levels (OOM kill, compaction+OOM, yield) before panicking. While thorough, this means **any kernel allocation that could theoretically exhaust memory will crash the system**, even if the caller is prepared to handle failure (e.g., by returning -ENOMEM to userspace). A better approach would be to return NULL from the allocator and let the caller decide.

---

## Pattern 6: Error messages lacking context

### Good examples (include function/resource context)

| File | Line | Message |
|---|---|---|
| `src/drivers/pmem.c` | 65 | `[pmem] ERROR: invalid device id %d (pmem_idx=%d)` |
| `src/drivers/pmem.c` | 74 | `[pmem] ERROR: cannot get SPA info for index %d` |
| `src/drivers/vdpa.c` | 168 | `[vdpa] ERROR: device '%s' has no ops` |
| `src/kernel/syscall.c` | 3452 | `[fsync] vfs_flush failed (%d) for %s` |
| `src/kernel/workqueue.c` | 229 | `[workqueue] failed to allocate workqueue "%s" (max %d)` |
| `src/kernel/service.c` | 329 | `[svc] %s: dependency '%s' failed (rc=%d), aborting` |
| `src/fs/luks.c` | 109 | `[luks] read header failed: %d` |
| `src/fs/ext2.c` | 1163 | `[ext2] resize: failed to init group %u: %d` |

### Poor examples (lack context — no function name, no error code, no resource identifier)

| File | Line | Message | Missing context |
|---|---|---|---|
| `src/kernel/elf.c` | 183 | `kprintf("elf: load failed\n")` | No error code, no path, no reason |
| `src/kernel/elf.c` | 196 | `kprintf("elf: cannot create page tables\n")` | No error code from vmm_create_user_pml4 |
| `src/kernel/elf.c` | 238 | `kprintf("elf: vmm_map_user_page failed\n")` | No virtual address, no error code |
| `src/kernel/elf.c` | 272 | `kprintf("elf: vmm_map_user_page failed for stack\n")` | No stack va, no error code |
| `src/kernel/elf.c` | 177 | `kprintf("elf: cannot read %s\n", path)` | No vfs_read error code |
| `src/kernel/spawn_kernel.c` | 117 | `kprintf("[spawn_kernel] vmm_map_user_page failed\n")` | No va, no flags, no error code |
| `src/drivers/e1000.c` | 444 | `kprintf("  e1000: failed to map MMIO\n")` | No PCI BAR info, no error code |
| `src/fs/tmpfs.c` | (init chain) | No error messages at all on mount failure | Silent failure |
| `src/drivers/vmw_balloon.c` | 119 | `vmw_balloon_init(void)` — silent on detection failure | No error message, no return value |

### Notable: `kernel/elf.c` (lines 183, 196, 238, 272)
Multiple error messages in the ELF loader lack specific error codes or resource identifiers, making it hard to diagnose which segment/address/page failed.

---

## SUMMARY

### Severity ranking of findings

1. **CRITICAL** — `/home/ubuntu/os/src/fs/fs.c:513`: `(void)ata_write_sectors()` discards disk write failure return — potential silent data corruption.

2. **HIGH** — `/home/ubuntu/os/src/memory/pmm.c:730,923`: OOM panics instead of returning errors to callers — prevents graceful failure handling anywhere in the kernel.

3. **HIGH** — IPC subsystem (`ipc/`): All 7 files use `-1` for every error return — no errno distinction, making all IPC error handling ambiguous.

4. **MEDIUM** — `(void)vfs_readahead()` discard in `syscall.c:8128`: Read-ahead failures silently ignored.

5. **MEDIUM** — `(void)fcoe_open_socket()` discard in `drivers/fcoe.c:84`: Socket failure silently ignored.

6. **MEDIUM** — `kernel/elf.c`, `kernel/spawn_kernel.c`, `drivers/e1000.c`: Error messages lack error codes and resource identifiers.

7. **MEDIUM** — `drivers/rtc.c:rtc_sysfs_init()` and `memory/zswap.c:zswap_init()`: Void functions that perform fallible operations (sysfs create, kmalloc) cannot propagate errors.

8. **LOW** — Approximately 197 instances of `return -1` across `kernel/`, 202 in `ipc/`, and many more in `drivers/` and `fs/` where proper errno values (`-ENOMEM`, `-EINVAL`, `-EACCES`) should be used.

9. **LOW** — 60+ void initialization functions that cannot report failures — mostly benign (memset only), but `zswap_init()` and `rtc_sysfs_init()` perform actual fallible operations.

### Positive observations
- `fs/ext2.c`, `net/af_packet.c`, `kernel/bpf_maps.c`, `drivers/dm-crypt.c`, and `memory/page_pool.c` consistently use proper errno returns.
- Most error messages in `drivers/`, `fs/`, and `kernel/` include `[subsystem]` tags.
- `kernel/vfs.c` uses distinct errno values (`-EACCES`, `-EIO`, `-EFSCORRUPTED`).
- `kernel/spawn_kernel.c` and `kernel/hashtable.c` use `-ENOMEM` properly.
- `kernel/service.c` error messages include dependency name and return code — excellent context.

### Files created
- `/home/ubuntu/os/error_handling_audit.md` — this report
