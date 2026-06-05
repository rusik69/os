# Bug Hunt — 100 Tasks

> **Goal:** Find and fix 100 bugs across the ~159K LOC kernel.
> **Method:** Systematic audits covering concurrency/locking, memory safety, buffer overflows, filesystem correctness, network stack, drivers, ELF loader, shell commands, and test coverage.
> **Source data:** 4 deep audits (TODO markers, buffer overflows, locking races, integer issues, filesystem/network/driver correctness) conducted June 2026.

---

## Phase 1 — Memory Safety & Concurrency (25 tasks)

### 1.1 Locking in Interrupt Handlers (8 tasks)

**Task 1 — `scheduler_tick()`: protect process accounting fields with `sched_lock`**

`scheduler_tick()` modifies `cur->utime_ticks`, `stime_ticks`, `vruntime`, `ticks_remaining` without any lock, while these same fields can be read/written by other CPUs via `schedule()`, `sys_times()`, or `/proc/self/stat`.

**Fix:** Wrap all proc-accounting mutations in `sched_lock` (spinlock_irqsave_acquire/release). Be careful: `scheduler_tick()` runs in IRQ context, so it must save/restore interrupt flags to avoid reentrancy with itself.

**File:** `src/process/scheduler.c:678-826`

---

**Task 2 — `process_timer_tick()`: lock process table during itimer walk**

`process_timer_tick()` (from timer IRQ) iterates `process_table[]` decrementing `p->itimers[]` and calling `signal_send()` with zero synchronization. The same `itimers[]` can be written by `sys_setitimer()` on another CPU.

**Fix:** Introduce a process table lock (`proc_table_lock`) or use `sched_lock` to guard itimer mutations. The IRQ handler must use `spinlock_irqsave_acquire` and the syscall path uses `spinlock_irqsave` as well.

**File:** `src/kernel/syscall.c:2745-2784`

---

**Task 3 — `signal_send()`/`signal_check()`: add per-process or global signal lock**

`signal_send()` modifies `p->pending_signals`, `p->state`, `p->exit_code` without any lock. Called from timer IRQ (`process_timer_tick`) and the scheduler, it races with `signal_check()` running on another CPU during context switch.

**Fix:** Add a `spinlock_t sig_lock` to `struct process`. `signal_send()` acquires it with `spinlock_irqsave`, `signal_check()` with `spinlock_irqsave`. Ensure no ABBA ordering with `sched_lock`.

**File:** `src/process/signal.c:16-220`

---

**Task 4 — AHCI IRQ handler: acquire `ahci_lock` before touching slot state**

`ahci_irq_handler()` modifies `port->slots[slot].req`, `port->inflight_mask`, and calls `blk_request_done()` without `ahci_lock`. `ahci_drain_queue()` does acquire the lock — data race.

**Fix:** In `ahci_irq_handler()`, wrap slot management in `spinlock_irqsave_acquire(&ahci_lock, flags)`.

**File:** `src/drivers/ahci.c:499-580`

---

**Task 5 — e1000 TX/RX paths: add per-queue lock**

`e1000_netdev_transmit()` and `e1000_netdev_receive()` modify `tx_cur`/`rx_cur` without any lock. Called from multiple process contexts. The interrupt handler also reads `itr_pkt_count`, `itr_current` without locking.

**Fix:** Add `spinlock_t e1000_lock` (or per-queue locks) and acquire in all data-modifying paths including the ISR.

**File:** `src/drivers/e1000.c:232-677`

---

**Task 6 — `cfs_update_min_vruntime()` + `scheduler_wakeup()`: lock runqueue before access**

`scheduler_wakeup()` calls `cfs_update_min_vruntime()` which iterates the per-CPU runqueue chain before acquiring `sched_lock`. This races with `scheduler_add/remove/schedule/scheduler_tick`.

**Fix:** Move the `sched_lock` acquisition earlier in `scheduler_wakeup()` so that all runqueue access is covered, or use RCU for the min_vruntime read.

**File:** `src/process/scheduler.c:1048-1105`

---

**Task 7 — `scheduler_get_runqueue_stats()`: add remote CPU queue locking**

Reads `ci->queue_head[lvl]` on a remote CPU without any lock, while the remote CPU's `scheduler_tick` (no lock) and `scheduler_add/remove` (with lock) modify the same queues.

**Fix:** Either (a) use per-CPU `raw_spinlock_t` for queue access, or (b) use RCU-protected lists with a grace period, or (c) use atomic load-acquire for the head pointer with RCU for the chain.

**File:** `src/process/scheduler.c:895-919`

---

**Task 8 — `spinlock_acquire`/`release`: wire into lockdep**

`spinlock_acquire()` never calls `lock_acquire()`. `spinlock_release()` never calls `lock_release()`. The entire lockdep subsystem (deadlock detection, cycle detection, cross-release, sleeping-while-atomic) is dead code.

**Fix:** Add `lock_acquire(&lock->dep_map, subclass, trylock, read, flags, ip)` and `lock_release(&lock->dep_map, ip)` calls in `spinlock.h` inline functions. Wire up mutex, rwsem, and RCU read-side primitives too.

**Files:** `src/include/spinlock.h:69-104`, `src/kernel/lockdep.c`

---

### 1.2 Network Stack Locking (3 tasks)

**Task 9 — Protect `net_our_ip`, `net_gateway_ip`, routing table, ARP cache with a network lock**

All network globals (`net_our_mac`, `net_our_ip`, `rt_table[]`, `net_arp_cache[]`, `tcp_conns[]`, `net_ip_id_counter`) are read/written from any context without synchronization.

**Fix:** Add a `net_lock` (rwlock for read-mostly paths) or per-table locks. The packet receive and transmit paths must acquire appropriate read or write locks. Use RCU for the routing table (read-mostly, periodic update).

**Files:** `src/net/net.c:17-52`, `src/net/af_packet.c`, `src/net/net_tcp.c`

---

**Task 10 — Protect TCP connection table (`tcp_conns[]`) with per-connection or global lock**

`tcp_conns[]` is modified by `tcp_connect()`, `tcp_accept()`, `tcp_close()`, and read by `handle_tcp()`, all without locking. Timer-based retransmit also accesses it.

**Fix:** Add a `tcp_lock` or per-connection `spinlock_t`. Use `spinlock_irqsave` in timer and interrupt context paths.

**File:** `src/net/net_tcp.c`

---

**Task 11 — Make `net_ip_id_counter` atomic**

`net_ip_id_counter++` is non-atomic on SMP and can produce duplicate IP IDs (bad for reassembly, security).

**Fix:** Change to `__sync_fetch_and_add` or a `volatile uint64_t` with atomic increment via `lock xadd`.

**File:** `src/net/net.c` (variable declaration + all increment sites)

---

### 1.3 Integer Overflows & Truncation (9 tasks)

**Task 12 — `sys_mmap`: add `addr + length` overflow check before boundary comparison**

At syscall.c:2100 and 2121-2146, `addr + length` can wrap past `UINT64_MAX` and bypass `USER_VADDR_MAX` guard.

**Fix:** Before mapping, check `addr + length < addr` (overflow) or explicitly `addr + length > USER_VADDR_MAX || addr + length < addr`.

**File:** `src/kernel/syscall.c:2100-2146`

---

**Task 13 — `sys_read`/`sys_write`: fix `uint64_t size → uint32_t` truncation for files >4GB**

File sizes are `uint64_t st.size` but truncated to `uint32_t fsize` at lines 446-452 and 804-813. `kmalloc(fsize)` allocates wrong size.

**Fix:** Use `uint64_t` throughout the read/write path. Or clamp reads to `UINT32_MAX` bytes and return `EFBIG` for larger files.

**File:** `src/kernel/syscall.c:446-452, 804-813`

---

**Task 14 — `sys_stat`: fix truncation of file size in `uint32_t*` output**

`sys_stat` writes file size into a `uint32_t*` user pointer.

**Fix:** Write both `st->size` (uint64_t) and ensure the stat struct has a 64-bit size field, or add a separate `sys_stat64` syscall.

**File:** `src/kernel/syscall.c:872-874`

---

**Task 15 — Fix `pfd->offset += (uint32_t)count` truncation in pread/pwrite paths**

File offset increment uses `uint32_t` cast, losing high bits for large counts.

**Fix:** Use `uint64_t` arithmetic. The `pfd->offset` field must be `uint64_t` throughout.

**File:** `src/kernel/syscall.c:835`

---

**Task 16 — Fix FAT32 cluster chain loop to detect cycles**

`dir_find()` walks the FAT cluster chain without cycle detection. A corrupted FAT can cause infinite loop.

**Fix:** Add a visited-cluster bitmap or max-iteration counter (e.g., max clusters on a 2TB disk = 2^28). If a cluster repeats, return `-EIO`.

**File:** `src/fs/fat32.c:545,957`

---

**Task 17 — Fix ISO9660 CE continuation infinite loop**

`parse_rrip_entries()` at line 361 does `goto walk_susp` with no depth limit on CE (Continuation Area) entries. A crafted ISO causes infinite loop.

**Fix:** Add a hard limit (max 16 CE hops) or a visited-set of (block, offset) pairs.

**File:** `src/fs/iso9660.c:358-362`

---

**Task 18 — Fix ACPI DSDT header length validation**

`dsdt_len = hdr->length` unchecked. If `hdr->length < sizeof(struct acpi_header)`, `aml_len = dsdt_len - sizeof(struct acpi_header)` underflows → huge loop → OOB read.

**Fix:** Add `if (hdr->length < sizeof(struct acpi_header)) return -EINVAL;` and `if (aml_len < 8) return -EINVAL;` before the parsing loop.

**File:** `src/drivers/acpi.c:189-195`

---

**Task 19 — Fix `vmm_committed_bytes` race by using atomic operations**

`vmm_committed_bytes += bytes` is non-atomic. Two concurrent `mmap` calls lose increments, allowing overcommit limit to be exceeded.

**Fix:** Use `__sync_fetch_and_add`/`__sync_fetch_and_sub` or add a spinlock guard.

**File:** `src/memory/vmm.c:96-107`

---

**Task 20 — Fix `waitqueue.c`: `spinlock_release` used where `spinlock_irqsave_release` needed**

`wait_queue_sleep()` acquires lock with `spinlock_irqsave_acquire` but releases with `spinlock_release`, losing the original IF state and enabling preemption while IRQs are off.

**Fix:** Save `flags` at acquisition and use `spinlock_irqsave_release(&wq->lock, flags)` instead of manual `sti`.

**File:** `src/ipc/waitqueue.c:42-47`

---

### 1.4 Undefined Behavior & General Memory Safety (5 tasks)

**Task 21 — Fix FAT32 LFN struct cast OOB read**

At fat32.c:514, `*(struct fat32_lfn *)&entries[i]` casts a 32-byte directory entry to a 54-byte struct, reading 22 bytes past the entry into adjacent memory.

**Fix:** Either (a) define LFN entries as a separate 32-byte packed struct, or (b) copy the 32 bytes to a properly-sized LFN struct and zero the remaining bytes. Never read past the directory entry boundary.

**File:** `src/fs/fat32.c:514,560,811`

---

**Task 22 — Fix `tarfs.c`: pointer stored as `uint32_t` (breaks >4GB memory)**

`base_addr` is `uint32_t` — on systems with >4GB RAM, the initrd/tar address may be above 4GB.

**Fix:** Use `uint64_t` or `void *`.

**File:** `src/fs/tarfs.c`

---

**Task 23 — Fix `mempool.c`: `min_nr * 2` overflow**

If `min_nr > INT_MAX/2`, `min_nr * 2` overflows `int`.

**Fix:** Use `size_t` multiplication or check for overflow before doubling.

**File:** `src/lib/mempool.c:6`

---

**Task 24 — Fix ext2 block group count overflow**

`num_block_groups * 32` at ext2.c:803 can overflow `uint32_t` on very large filesystems.

**Fix:** Use `uint64_t` arithmetic for block group computation.

**File:** `src/fs/ext2.c:803`, `src/include/ext2.h:236`

---

**Task 25 — Fix `computed_slices[]` concurrent access race**

`scheduler_tick()` (IRQ context) calls `slice_for_prio()` which reads `computed_slices[lvl]`, while `recompute_time_slices()` writes to the same array from process context. No lock.

**Fix:** Either (a) protect with `sched_lock`, or (b) use a seqlock, or (c) make the array `atomic_t` and use `atomic_read`.

**File:** `src/process/scheduler.c:67-99`

---

## Phase 2 — Buffer Overflows & String Safety (10 tasks)

**Task 26 — Fix `cmd_zforce.c`: unbounded `strcpy(buf, args)` into 128-byte stack buffer**

`strcpy(buf, args)` copies user input with no length limit. User can overflow the stack.

**Fix:** Use `strncpy(buf, args, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';`

**File:** `src/shell/cmds/cmd_zforce.c:8-9`

---

**Task 27 — Fix 9 shell commands: unbounded `strcpy(path+1, args)` into 64-byte buffer**

`cmd_touch.c`, `cmd_stat.c`, `cmd_mkdir.c`, `cmd_wc.c`, `cmd_xxd.c`, `cmd_run.c`, `cmd_chown.c`, `cmd_chmod.c`, `cmd_paste.c` all do `char path[64]; path[0]='/'; strcpy(path+1, args)` — user input can overflow.

**Fix:** Use `snprintf(path, sizeof(path), "/%s", args)` or `strncpy`.

**Files:** Multiple `src/shell/cmds/` files.

---

**Task 28 — Fix `cmd_chown.c`: unbounded while-loop writing to `owner[32]` and `grp[32]`**

No guard on index when parsing user/group names. Long input corrupts stack.

**Fix:** Add bounds check on index before writing to `owner[idx]` and `grp[idx]`.

**File:** `src/shell/cmds/cmd_chown.c:21-31`

---

**Task 29 — Fix `shell/editor.c`: `strcpy(path+1, b->filename)` with no bounds check**

`path[66]` in status line / file operations. `strcpy(path+1, b->filename)` can overflow if filename > 64 chars.

**Fix:** Use `snprintf(path, sizeof(path), "/%s", b->filename)`.

**File:** `src/shell/editor.c:641,957,978`

---

**Task 30 — Fix FAT32 LFN name buffer: off-by-one NUL write via `strcat(out, "/")`**

`strcat(out, "/")` after building a 256-byte LFN name writes '/' at position 256 — one past the buffer.

**Fix:** Check `strlen(out) + 1 < LFN_MAX` before appending. Or use `snprintf` with size limit.

**File:** `src/fs/fat32.c:838,892`

---

**Task 31 — Fix `cmd_head.c` / `cmd_tail.c`: off-by-one in `path[64]`**

`path[0] = '/'; strncpy(path+1, args, 62)` — max 62 chars + '/' = 63, but buffer is 64. If args is 63 chars, no NUL terminator.

**Fix:** Use `snprintf(path, sizeof(path), "/%s", args)`.

**Files:** `src/shell/cmds/cmd_head.c:35-37`, `cmd_tail.c:34-36`

---

**Task 32 — Audit all remaining `strcpy`/`strcat`/`sprintf` across all shell commands**

117+ `strcpy` calls total. Many may use hardcoded strings (safe) but need verification.

**Fix:** Replace all user-data `strcpy/strcat/sprintf` with `strncpy/snprintf` variants. Use `grep -rn 'strcpy\|strcat\|sprintf' src/shell/cmds/` to find remaining sites.

**Files:** All `src/shell/cmds/*.c`

---

**Task 33 — Fix VFS `vfs_abs_path()` stack buffer overflow risk**

`tmp[128]` can overflow on deeply nested paths. The bound check caps component length but `memcpy` at line 269 still copies the truncated data.

**Fix:** After detecting overflow, return `-ENAMETOOLONG` instead of writing truncated data.

**File:** `src/kernel/vfs.c:266-270`

---

**Task 34 — Fix VFS xattr hash collision data overwrite**

xattr hash uses 16 slots × 4 entries. Collisions silently overwrite via `strncpy` with no collision check beyond path match.

**Fix:** When the slot is occupied, check if path matches. If not, try next entry in the slot. If all 4 are full, return `-ENOSPC`. Or use a proper hash table with chaining.

**File:** `src/kernel/vfs.c:1061-1115`

---

**Task 35 — Add ELF loader `p_filesz` validation to prevent partial reads at OOB offsets**

At elf.c:150, `if (vfs_read(fpath, elf_buf, fsize, &read_len))` where `fsize = st.size`. If `st.size` is truncated (see Task 13), the buffer truncation at `ELFSIZE_MAX` (64KB) could leave partial program headers, causing the loader to act on uninitialized data.

**Fix:** Validate `st.size >= hdr->e_phoff + hdr->e_phnum * sizeof(Elf64_Phdr)` and that each `p_vaddr + p_filesz` doesn't wrap.

**File:** `src/kernel/elf.c:150-160`

---

## Phase 3 — Filesystem Correctness (15 tasks)

**Task 36 — Fix VFS execve: prevent stack pointer underflow via argv/envp**

`process_execve()` copies up to 512 strings of 255 chars each onto the user stack. With total size ~130KB exceeding the 64KB user stack, `sp -= total_str_size` underflows, corrupting kernel memory.

**Fix:** Bound-check `total_str_size + (envc+1)*8 + (argc+1)*8 + 8 >= USER_STACK_SIZE` and return `-E2BIG` if exceeded.

**File:** `src/kernel/elf.c:442-565`

---

**Task 37 — Fix VFS `vfs_abs_path()`: prevent `../../..` above root escaping chroot**

The ".." handling walks above root (`/`) by decrementing `wpos` past 0 then clamping to 0. This allows constructing `//` paths and bypasses proper root-sentinel logic.

**Fix:** If `wpos == 0` and component is "..", skip without decrementing (or return `-EINVAL` if strict). Ensure the root sentinel `/` is never removed.

**File:** `src/kernel/vfs.c:251-255`

---

**Task 38 — Fix VFS: add proper `hardlink()` semantics (currently a data copy)**

`vfs_link()` reads entire file into heap, writes to new path — O(n) time + memory, no inode sharing.

**Fix:** Implement true hard links: increment inode's link count in the filesystem, create a new directory entry pointing to same inode. Filesystem-specific hooks needed.

**File:** `src/kernel/vfs.c:1421-1451`

---

**Task 39 — Fix VFS `pivot_root`: propagate mount table swap to all mount namespaces**

`vfs_pivot_root()` swaps global mount table but processes with private `mnt_namespace` still see old mounts.

**Fix:** For each existing namespace, walk and update mount table entries with the new root or invalidate stale entries.

**File:** `src/kernel/vfs.c:1613-1675`

---

**Task 40 — Fix VFS mount table: add proper refcounting and umount support**

No `vfs_umount()` exists — mounts are permanent and the 16-entry table can exhaust. No refcounting on `priv` pointer.

**Fix:** Add mount refcount, reference from each open file/dentry. Implement `sys_umount` that calls `vfs_unmount()` to decrement refcount and free on zero.

**File:** `src/kernel/vfs.c`, `src/kernel/syscall.c:5549`

---

**Task 41 — Fix VFS: add dentry cache invalidation on `pivot_root`**

`dcache_remove_mount()` exists but is never called when mounts change. After pivot_root, stale dentries point to old mounts.

**Fix:** Call `dcache_remove_mount()` for old and new root after pivot_root.

**File:** `src/kernel/vfs.c:138-147`

---

**Task 42 — Fix VFS: add locking around mount table iteration in `vfs_flush()`/`vfs_sync_all()`**

`vfs_flush()` iterates mount table entries without any lock. Concurrent mount/unmount could read freed memory.

**Fix:** Acquire `mount_lock` (a new or existing lock) before iterating.

**File:** `src/kernel/vfs.c:1524-1564`

---

**Task 43 — Fix ext2: validate directory `rec_len >= 8 + name_len` before advancing**

At ext2.c:543, `pos += dirent->rec_len`. If `rec_len < 8 + name_len` (rounded up), the parser reads partial entries and can infinite-loop.

**Fix:** Add `if (dirent->rec_len < 8 + dirent->name_len) return -EIO;` before advancing. Also verify `rec_len % 4 == 0`.

**File:** `src/fs/ext2.c:534-594`

---

**Task 44 — Fix ext2: add symlink depth limit**

Ext2 path resolution never follows symlinks. When support is added (or if a filesystem has symlinks from Rock Ridge), add a recursion depth limit (max 40 follows) to prevent kernel stack overflow.

**File:** `src/fs/ext2.c:641-676`

---

**Task 45 — Fix ext2: add feature incompatibility rejection**

Mounting an ext2 image with `EXT2_FEATURE_INCOMPAT_EXTENTS` or `EXT2_FEATURE_INCOMPAT_64BIT` silently produces data corruption.

**Fix:** Read `sb->s_feature_incompat` and reject any unsupported flags with a clear error message.

**File:** `src/fs/ext2.c:831-838`, `src/include/ext2.h`

---

**Task 46 — Fix ext2: add doubly/triply indirect block support**

Files larger than ~268 blocks (for 1KB blocksize) silently truncate reads because doubly/triply indirect returns -1.

**Fix:** Implement `ext2_get_block_num_dindirect()` and `ext2_get_block_num_tindirect()` to walk the indirect block chains.

**File:** `src/fs/ext2.c:150-173`

---

**Task 47 — Fix FAT32: validate LFN entry ordering and LAST_LFN bit**

`lfn_build_name()` doesn't verify that the highest-order entry has LAST_LFN bit (0x40). Could produce garbled names.

**Fix:** After building the name, verify that `lfn_parts[count-1]` has the 0x40 bit. If not, treat as corrupt and fall back to short name.

**File:** `src/fs/fat32.c:304-326`

---

**Task 48 — Fix ISO9660: make `ce_buf` non-static to avoid reentrancy crash**

`static uint8_t ce_buf[2048]` in `parse_rrip_entries()` makes ISO9660 non-reentrant — two concurrent reads corrupt each other's data.

**Fix:** Pass a dynamically-allocated buffer or use a stack-alloca with proper bounds. Remove `static`.

**File:** `src/fs/iso9660.c:228`

---

**Task 49 — Fix ISO9660: validate SymLink component length against available data**

At iso9660.c:335, `sl_pos += 2 + comp_len` can advance past `sl_data_len`.

**Fix:** After reading `comp->len`, check `sl_pos + 2 + comp_len <= sl_data_len`. If not, heal by breaking the loop (truncate the symlink).

**File:** `src/fs/iso9660.c:300-336`

---

**Task 50 — Fix ISO9660: add proper Rock Ridge symlink following in path resolution**

Rock Ridge symlinks are stored but never resolved during path traversal. `/path/to/symlink/file` fails.

**Fix:** In `iso9660_resolve()`, when a path component has the `RRIP_HAS_SL` flag, read the symlink target and continue resolution from the target path. Add depth limit.

**File:** `src/fs/iso9660.c:450-532`

---

## Phase 4 — Network Stack Bugs (12 tasks)

**Task 51 — Fix ARP cache poisoning: validate sender IP is within subnet**

`handle_arp()` at net.c:671 immediately adds any ARP sender's IP→MAC. No subnet check, no verification, no rate limit.

**Fix:** (1) Only accept ARP for IPs in our subnet (masked IP). (2) For existing entries, log warning on mismatch but accept after N occurrences (3-strike). (3) Rate-limit to 10 updates/second.

**File:** `src/net/net.c:664-698`

---

**Task 52 — Fix TCP: add `TCP_CLOSING` state for simultaneous close**

When both sides FIN simultaneously, the TCP state machine skips `TCP_CLOSING` and goes directly FIN_WAIT → TIME_WAIT, missing the ACK validation for our FIN.

**Fix:** Add `TCP_CLOSING` state (value TBD). On FIN in FIN_WAIT_1, transition to CLOSING. On ACK in CLOSING, transition to TIME_WAIT.

**File:** `src/net/net_tcp.c:1249-1259`, `src/include/net_tcp.h`

---

**Task 53 — Fix TCP: add incoming data window validation**

`handle_tcp()` doesn't validate that received data falls within the peer's advertised window. OOB writes into `rxbuf` via `memcpy`.

**Fix:** After sequence number validation, check that `seq + len` is within `their_window` (from the peer's last ACK). Drop out-of-window data.

**File:** `src/net/net_tcp.c:811,1241`

---

**Task 54 — Fix IP fragment reassembly: validate IHL consistency across fragments**

Reassembled IP uses IHL from first fragment but IP header from last fragment. If they differ (e.g., due to IP options), header misparse follows.

**Fix:** Either (a) verify all fragments have the same IHL value, or (b) use the IHL from the fragment that provides the final header.

**File:** `src/net/net.c:929,934`

---

**Task 55 — Fix IP fragment reassembly: prevent OOB bitmap access via TOCTOU**

At frag reassembly, `frag_off + part` bounds-checked at line 877 but not re-checked in the bitmap-mark path at line 903. Concurrent modification (unlikely but no lock) could bypass.

**Fix:** Re-validate `off + part <= IP_FRAG_BUF_SIZE` in the bitmap mark path, or use a single-threaded per-slot design with proper locking.

**File:** `src/net/net.c:891-904`

---

**Task 56 — Fix IP reassembly timer: add timeout for incomplete fragments**

There's no timer-based cleanup for partial fragment chains. A single fragment can hang in memory indefinitely, exhausting the limited reassembly slot.

**Fix:** Add a last-seen timestamp to each reassembly slot. In `net_poll()` or a timer callback, reap slots older than 30 seconds.

**File:** `src/net/net.c:870-920`

---

**Task 57 — Fix TCP retransmit timer: add exponential backoff and RTO tracking**

Currently either missing or hardcoded. Must implement Karn's algorithm + Jacobson's RTO estimation.

**Fix:** Implement TCP RTO calculation: `srtt += (m - srtt) >> 3`, `rttvar += (abs(m - srtt) - rttvar) >> 2`, `RTO = srtt + max(G, 4*rttvar)`. Cap at min 200ms, max 120s.

**File:** `src/net/net_tcp.c`

---

**Task 58 — Fix TCP SYN cookie generation**

SYN cookies exist but need verification: proper secret key rotation (every 60s), correct MSS encoding, TCP timestamp support.

**Fix:** Review `tcp_syn_cookie()` implementation. Ensure secret is re-generated every 60s. Add syncookie for `tcp_tfo.c` too.

**Files:** `src/net/net_tcp.c`, `src/include/net_tcp.h`

---

**Task 59 — Fix ICMP unreachable generation for invalid packets**

ICMP unreachable/needfrag/timeexceeded should be rate-limited (icmp_ratelimit) to prevent reflection attacks.

**Fix:** Add `icmp_ratelimit()` — allow max 100 ICMP errors per second. Token bucket or simple timestamp-based.

**File:** `src/net/net.c` (ICMP generation)

---

**Task 60 — Fix TCP urgent pointer handling**

TCP urgent data (URG flag + urgent pointer) must be handled per RFC 961 §3.3. Currently may be ignored or misinterpreted, causing connection desync.

**Fix:** Track `urg_seq` during handshake. On URG packet, notify the socket. May not need full OOB delivery but must advance seq correctly.

**File:** `src/net/net_tcp.c`

---

## Phase 5 — Driver Bugs (12 tasks)

**Task 61 — Fix PCI: scan all 8 functions per device, not just function 0**

`pci_find_device()` and `pci_find_class()` only check function 0. Multi-function devices at functions 1-7 are invisible.

**Fix:** For each device (bus:slot:0), check if header type has MF (multi-function) bit. If yes, probe functions 0-7. Always probe functions 0 for single-function devices.

**File:** `src/drivers/pci.c:600-649`

---

**Task 62 — Fix virtio-blk: validate feature negotiation results**

After `virtio_negotiate_features_ex()`, the driver reads queue sizes without checking that the device accepted the expected features. If `VIRTIO_BLK_F_MQ` wasn't accepted, probing queue 1+ is undefined.

**Fix:** Read `GUEST_FEATURES` register after negotiation and verify the feature bits you requested are set. If not, fall back gracefully.

**File:** `src/drivers/virtio_blk.c:213-221`

---

**Task 63 — Fix virtio-blk: add modern MMIO transport support**

`VIRTIO_BLK_CAPACITY_LO`/`HI` at legacy I/O offsets 0x14/0x18. Modern virtio 1.0 devices using MMIO are not detected.

**Fix:** Check BAR type (I/O vs memory). For memory BARs, use MMIO access per virtio 1.0 spec. Detect modern transport via PCI vendor+device IDs.

**File:** `src/drivers/virtio_blk.c:31-32,224-226`

---

**Task 64 — Fix USB MSC: validate device descriptor contents**

`usb_msc_init()` calls GET_DESCRIPTOR and receives 18 bytes but validates none — no bLength check, no bDescriptorType check, no bDeviceClass validation.

**Fix:** After control transfer, verify `desc[0] == 18`, `desc[1] == 0x01` (DEVICE), `desc[4] & 0xEF` (interface class). Return `-ENODEV` on mismatch.

**File:** `src/drivers/usb_msc.c:443-453`

---

**Task 65 — Fix USB EHCI: handle NULL data pointer in zero-length control transfers**

`usb_control()` called with `data = (void *)0` for zero-length transfers. If EHCI dereferences the pointer, NULL deref.

**Fix:** In the EHCI `usb_control()` implementation, check `len == 0` and skip data-stage TD creation. Or in the caller, pass a dummy buffer.

**File:** `src/drivers/usb_ehci.c`, `src/drivers/usb_msc.c:456,463`

---

**Task 66 — Fix ACPI: validate `find_rsdp()` physical memory bounds**

RSDP scan uses `PHYS_TO_VIRT(addr)` for addresses in the `start` to `end` range. If these addresses map to non-existent physical memory, a page fault occurs.

**Fix:** Add a `e820_valid_phys(addr)` check before dereferencing. Or restrict scan to known-good ranges (EBDA 0x80000-0x9FFFF, BIOS 0xE0000-0xFFFFF).

**File:** `src/drivers/acpi.c:166-176`

---

**Task 67 — Fix ACPI: hardcoded `_S3_`/`_S5_` values (QEMU-specific)**

Sleep state detection uses byte-pattern matching (`0x07` = `_S3_`, `0x05` = `_S5_`). These are QEMU defaults, not actually parsed from AML packages.

**Fix:** Implement proper AML NameOp parsing for `_S3_`, `_S4_`, `_S5_` packages. Extract the first integer from each package.

**File:** `src/drivers/acpi.c:195,242`

---

**Task 68 — Fix floppy driver: UMA buffer alignment**

Floppy DMA uses the ISA DMA controller which requires buffers below 16MB and 64KB-aligned or physically contiguous. Current kmalloc may not satisfy.

**Fix:** Use `pmm_alloc_frame()` for physically-contiguous pages below 16MB, or add DMA zone allocation.

**File:** `src/drivers/floppy.c`

---

**Task 69 — Fix NVMe: PRP list boundary crossing**

NVMe PRP entries cannot cross a page boundary (4KB). If `prp1 + len` crosses page boundary, must use PRP list.

**Fix:** In NVMe command submission, after setting PRP1/PRP2, check if the data transfer crosses a page boundary. If so, build a PRP list in contiguous DMA memory.

**File:** `src/drivers/nvme.c`

---

**Task 70 — Fix e1000: add TX queue full handling (non-blocking)**

When `tx_cur` wraps around and catches `tx_next`, the TX ring is full. Currently may silently drop or corrupt packets.

**Fix:** Return `NETDEV_TX_BUSY` and let the upper layer retry. Don't overwrite pending descriptors.

**File:** `src/drivers/e1000.c`

---

**Task 71 — Fix watchdog timer: ensure periodic kick from idle loop**

Watchdog driver needs periodic petting. If the idle loop enters a deep C-state, the watchdog might not get kicked, causing spurious reset.

**Fix:** In `cpuidle_idle()`, before HLT, check if the watchdog needs a kick. Or use a timer-based kick via the timer IRQ handler (more reliable).

**Files:** `src/drivers/watchdog.c`, `src/kernel/cpuidle.c`

---

**Task 72 — Fix AHCI NCQ: tag collision on slot reuse**

When an NCQ slot completes and is reused before the completion handler finishes, the new command's `slot->req` overwrites the old one.

**Fix:** Add an in-flight / completing / free state per slot. Don't reuse a slot until `ahci_irq_handler()` fully completes the old command (including `blk_request_done`).

**File:** `src/drivers/ahci.c`

---

## Phase 6 — ELF Loader & Security (6 tasks)

**Task 73 — Fix ELF: implement setuid/setgid binary elevation**

`elf_exec()` and `process_execve()` never check the file's setuid/setgid mode bits and never elevate privileges.

**Fix:** In `elf_exec()`, after `vfs_stat()` on the binary, check `st.mode & S_ISUID` and/or `S_ISGID`. If set, call `process_set_uid(st.uid)` before jumping to userspace entry. Respect `no_new_privs` (from prctl or seccomp) to skip elevation.

**File:** `src/kernel/elf.c:396-400`

---

**Task 74 — Fix ELF: prevent PT_LOAD segment overlap with user stack region**

PT_LOAD segments are mapped without checking for overlap with the user stack region (`USER_STACK_TOP - STACK_SIZE` to `USER_STACK_TOP`).

**Fix:** After loading all PT_LOAD segments, verify none overlaps with the stack region. Return `-ENOEXEC` if an overlap exists.

**File:** `src/kernel/elf.c:199-258`

---

**Task 75 — Fix ELF: increase ELFSIZE_MAX from 64KB to support real binaries**

`ELFSIZE_MAX = 65536` limits all ELF binaries to 64KB. Real-world binaries (libc, shell, compilers) are larger.

**Fix:** Either (a) increase to 1MB, or (b) make dynamic: read ELF header first, then `p_filesz` for each segment, allocate and load per-segment instead of whole-binary.

**File:** `src/kernel/elf.c:14`

---

**Task 76 — Fix ELF: validate program headers don't wrap on `p_vaddr + p_memsz`**

At elf.c:99, `p_vaddr + p_memsz` can wrap UINT64_MAX. The overflow check at line 73 uses the original `p_memsz` before alignment rounding.

**Fix:** Re-check `p_vaddr + aligned_memsz` after alignment. If smaller than `p_vaddr`, reject as wrapping.

**File:** `src/kernel/elf.c:68-73, 95-99`

---

**Task 77 — Fix ELF: enforce `AT_SECURE` auxv entry for setuid binaries**

When setuid is used (Task 73), auxv `AT_SECURE` must be set to 1 so the dynamic linker disables dangerous env vars (LD_PRELOAD, LD_LIBRARY_PATH, etc.).

**Fix:** Set `AT_SECURE = 1` in auxv if the binary had setuid/setgid or if the process has `no_new_privs`. Currently may always be 0.

**File:** `src/kernel/elf.c:530-580` (auxv setup)

---

**Task 78 — Add stack gap / guard to userspace stack allocation**

After mapping the user stack, there's no guard page below it. Stack growth beyond `STACK_SIZE` will silently map into adjacent user memory.

**Fix:** When mapping user stack, reserve an unmapped guard page (or page range) below the stack bottom by marking those PTEs not present.

**File:** `src/kernel/elf.c:258-265`

---

## Phase 7 — Shell Commands (8 tasks)

**Task 79 — Fix `cmd_ncdu.c`: validate `st.size` bounds before `kmalloc`**

Allocates `kmalloc(st.size)` for readonly view of file contents. If `st.size` is huge, OOM freeze.

**Fix:** Cap at 4MB per file read. Use chunked read instead of one big alloc.

**File:** `src/shell/cmds/cmd_ncdu.c`

---

**Task 80 — Fix `cmd_fm.c`: prevent file list buffer overflow**

FM (file manager) stores file list in fixed-size buffer. Long directory listing overflows.

**Fix:** Use dynamic allocation or paginate the listing.

**File:** `src/shell/cmds/cmd_fm.c`

---

**Task 81 — Fix `cmd_inetd.c`: validate port number range**

No validation that port number is in range [1-65535]. Port 0 or >65535 causes undefined listen behavior.

**Fix:** Add `if (port < 1 || port > 65535) return -EINVAL;`.

**File:** `src/shell/cmds/cmd_inetd.c`

---

**Task 82 — Fix `cmd_ping.c`: validate ICMP packet length before send**

No validation that the ICMP payload length is within reasonable bounds. Too-large payload causes OOM or truncation.

**Fix:** Clamp payload to max 1472 bytes (standard Ethernet MTU - IP header - ICMP header). Return error if user requests more.

**File:** `src/shell/cmds/cmd_ping.c`

---

**Task 83 — Fix `cmd_dhcp.c`: validate server response length**

BOOTP packet parsing assumes server response is at least 300 bytes. Short response causes OOB read.

**Fix:** Check `rlen >= sizeof(struct bootp_pkt)` before accessing fields.

**File:** `src/shell/cmds/cmd_dhcp.c`

---

**Task 84 — Fix `cmd_route.c`: validate netmask prefix length**

No validation that prefix length is 0-32 for IPv4. /33 or /-1 causes undefined.

**Fix:** Add `if (prefix < 0 || prefix > 32) return -EINVAL;`.

**File:** `src/shell/cmds/cmd_route.c`

---

**Task 85 — Fix `cmd_ps.c`: protect against process table race**

Iterates `process_table[]` without any lock. A process exiting concurrently can produce a corrupted snapshot.

**Fix:** Acquire `proc_table_lock` (from Task 2) during iteration. Copy minimal fields under lock, then format outside.

**File:** `src/shell/cmds/cmd_ps.c`

---

**Task 86 — Fix `cmd_kill.c`: validate signal number range**

No validation that signal number is in [1, NSIG-1]. Signal 0 (null signal for existence check) may be misinterpreted.

**Fix:** Add `if (sig < 0 || sig >= NSIG) return -EINVAL;`. Allow `sig == 0` for error checking.

**File:** `src/shell/cmds/cmd_kill.c`

---

## Phase 8 — Boot Sequence & Init (5 tasks)

**Task 87 — Fix boot: ensure PIC remap happens before any interrupt is enabled**

`pic_remap()` must happen before `sti()`. Currently PIC is remapped during `pic_init()` but interrupts stay off until the end. Verify that no IRQ fires between PIC remap and IDT setup.

**Fix:** Ensure `pic_remap()` is immediately followed by `idt_init()` with no sti in between. Move `sti()` to the very end of `kernel_main`.

**File:** `src/kernel/kernel.c:252-258`

---

**Task 88 — Fix boot: add APIC timer calibration fallback if HPET unavailable**

ACPI indicates HPET presence but if it's not emulated (e.g. QEMU TCG without -machine hpet=on), TSC deadline timer calibration may use wrong frequency.

**Fix:** In `tsc_deadline_init()` / `timer_init()`, detect HPET/PMTIMER presence. If missing, fall back to PIT calibration or use a hardcoded QEMU TSC frequency (2GHz on KVM, 100MHz on TCG).

**File:** `src/drivers/timer.c`, `src/kernel/tsc_deadline.c`

---

**Task 89 — Fix boot: handle initrd address above 4GB (multiboot2 support)**

Multiboot1 passes addresses as uint32_t. If GRUB2 loads the initrd above 4GB (e.g. with large RAM), the mod_start/mod_end are truncated.

**Fix:** Support multiboot2 which passes 64-bit addresses. For multiboot1, warn if mod_start > 4GB.

**File:** `src/kernel/kernel.c:996-1023`

---

**Task 90 — Fix boot: verify `multiboot_info_phys` pointer before dereferencing**

`PHYS_TO_VIRT(multiboot_info_phys)` at line 292 assumes the address is a valid physical address. No validation.

**Fix:** Check that `multiboot_info_phys` is within known-good physical memory (via PMM range or at least non-zero).

**File:** `src/kernel/kernel.c:291-300`

---

**Task 91 — Fix boot: add `INIT_TASK` stack canary injection before first process spawn**

The canary is set in `kernel_main()` but the idle process and first kernel thread may use a stale canary.

**Fix:** Ensure `__stack_chk_guard` is set before `process_create()` is called. Currently it's set at the very start of `kernel_main` so this is likely OK, but verify.

**File:** `src/kernel/kernel.c:220`, `src/process/process.c`

---

## Phase 9 — Test Coverage & KUnit (6 tasks)

**Task 92 — Add KUnit test: slab allocator stress (large allocs, edge sizes)**

Currently no slab tests cover: large object allocations (>PAGE_SIZE), zero-size allocations, concurrent alloc/free from multiple threads.

**Target:** `src/test/kunit_slab.c`

---

**Task 93 — Add KUnit test: VMM page table operations (unmap, remap, huge pages)**

Cover: `vmm_unmap_page()` followed by `vmm_map_page()` to same address, 2MB/1GB huge page splitting, PML4/PDPT/PD/PT walk for all level sizes.

**Target:** `src/test/kunit_vmm.c`

---

**Task 94 — Add KUnit test: TCP state machine transitions**

Cover every transition in the TCP state diagram: LISTEN → SYN_RCVD → ESTABLISHED → FIN_WAIT + simultaneous close (CLOSING) → TIME_WAIT, RST handling.

**Target:** `src/test/kunit_tests.c` (new TCP section)

---

**Task 95 — Add KUnit test: FAT32 corrupt directory entry resilience**

Feed crafted FAT32 images with: short-entry-only directories, LFN entries with wrong checksum, cyclic cluster chains, entries with rec_len=0.

**Target:** `src/test/kunit_tests.c` (new FAT32 section, needs simulated block device)

---

**Task 96 — Add KUnit test: ELF loading edge cases**

Test: PT_LOAD overlap, p_vaddr wraparound, binary with no program headers, p_filesz > p_memsz, huge argv/envp (should return E2BIG), setuid/setgid detection.

**Target:** `src/test/kunit_tests.c` (new ELF section)

---

**Task 97 — Add KUnit test: signal delivery and return**

Test: signal during syscall (restart vs interrupt), nested signals, SIGKILL/SIGSTOP can't be caught/ignored, signal delivery to multithreaded processes.

**Target:** `src/test/kunit_tests.c` (new signal section)

---

## Phase 10 — Compiler Warnings & Static Analysis (3 tasks)

**Task 98 — Enable `-Wsign-compare` and fix all occurrences**

`-Wsign-compare` from `-Wextra` is already on, but `-Wconversion` should catch many of the truncation bugs (Task 12-15).

**Fix:** Add `-Wconversion -Wno-sign-conversion` to CFLAGS. Fix all new warnings — these will catch `uint64_t → uint32_t` truncations, `int → uint32_t` sign mismatches, etc.

**File:** `Makefile:59-65` (CFLAGS)

---

**Task 99 — Run cppcheck and fix all findings**

cppcheck is mentioned in `build-strict` target but the target doesn't exist. Add static analysis to the build.

**Fix:** Create `build-strict` Makefile target:
```
build-strict: cppcheck
cppcheck:
    cppcheck --enable=all --suppress=missingIncludeSystem \
      -Isrc/include --std=c17 --platform=unix64 src/
```
Fix all `error`, `warning`, `performance`, and `portability` findings. Style notes can be reviewed manually.

**File:** `Makefile` (new target)

---

**Task 100 — Add Clang static analyzer run**

Add a `analyze` Makefile target using clang's `--analyze`:

```
analyze:
    $(CC) --analyze -Xanalyzer -analyzer-output=text \
      $(CFLAGS) $(C_SRCS) 2>&1 | tee build/analyzer-report.txt
```

Fix all reported issues (dead stores, null deref, memory leaks, API misuse).

**File:** `Makefile` (new target)

---

## Bug Summary

| Category | Tasks | Severities |
|----------|-------|------------|
| Concurrency/Locking | 1-11 | 8 critical, 3 high |
| Integer Overflows | 12-25 | 2 critical, 4 high, 3 medium |
| Buffer Overflows | 26-35 | 5 critical, 3 high |
| Filesystem Correctness | 36-50 | 3 critical, 5 high, 4 medium |
| Network Stack | 51-62 | 3 critical, 4 high, 2 medium |
| Driver Bugs | 61-72 | 3 critical, 4 high |
| ELF Loader/Security | 73-78 | 2 critical, 3 high |
| Shell Commands | 79-86 | 2 high, 4 medium |
| Boot Sequence | 87-91 | 1 high, 4 medium |
| Test Coverage | 92-97 | — (new tests) |
| Tooling/Warnings | 98-100 | — (infrastructure) |

**Total critical:** ~23  
**Total high:** ~35  
**Total medium:** ~25  
**Total test improvements:** 6  
**Total infrastructure:** 3  

Each task corresponds to a tracked item. Tasks are ordered by severity roughly (most critical first within each phase). Many tasks are independent and can be parallelized.
