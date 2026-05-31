# OS — Improvement & Feature History

## Codebase Stats

| Metric | Value |
|--------|-------|
| Total source lines | 47,148 (C + ASM + headers) |
| C source files | 226 |
| Header files | 72 |
| Shell commands | 137 (`cmd_*.c` files) |
| Syscalls implemented | 274 (SYS_ #defines) |
| Syscall dispatch cases | 419 |
| In-kernel tests | 30+ (`src/test/test.c`) |
| E2E test groups | 113 (`tests/e2e.py`) |
| CI jobs | 8 (build, build-strict, static-analysis, test, e2e, virtio-net-smoke, usb-fat-smoke, libc-test) |

---

## Completed Improvements

### Memory Protection & Process Isolation

| Feature | Commit | Status |
|---------|--------|--------|
| SMEP (Supervisor Mode Execution Prevention) | `6968d51` | ✅ |
| SMAP (Supervisor Mode Access Prevention) | `6968d51` | ✅ |
| NXE (No-Execute bit on page table entries) | `6968d51` | ✅ |
| UMIP (User Mode Instruction Prevention) | `6968d51` | ✅ |
| NX pages for user ELF segments | `6968d51` | ✅ |
| Kernel stack guard pages (overflow detection) | `5aeaec3` | ✅ |
| User stack guard pages (ring 3 overflow detection) | `fd9e94e` | ✅ |
| Guard page + VMM hugepage split + ELF edge-case tests | `9fafac9` | ✅ |

### Syscall Expansion

60+ new syscalls added across multiple batches:

**Batch 1** (`95b5016`): pipe, getppid, alarm, pause, access, uid/gid syscalls, rmdir, rename, chmod, fsync, sigprocmask, readv/writev, getrandom, reboot, hostname/sethostname, O_NONBLOCK, umask, mknod, enhanced procfs, PRNG

**Batch 2** (`ab06f87`): dup/dup2, fcntl, select/pselect, itimers (get/set), nanosleep, execve with argv/envp, sysconf, uname

**Batch 3** (`a22d0b7`): openat/mkdirat/renameat/linkat/symlinkat/readlinkat/fchownat/fstatat/unlinkat, getdents64, timerfd_create/gettime/settime, signalfd, splice/vmsplice/tee, madvise, setsid, sigaltstack, personality

**Batch 4** (`08ec571`): socket/bind/listen/accept/connect, setsockopt/getsockopt, sendmsg/recvmsg, getsockname/getpeername/socketpair, epoll_create1/ctl/wait/pwait, clock_gettime/settime/getres, POSIX timers (create/settime/gettime/getoverrun/delete), pipe2, dup3, mkdtemp, utimensat/futimens, statfs/fstatfs, getrusage, sysinfo, getresuid/setresuid/getresgid/setresgid, mq_open/send/receive/unlink, sched_getparam/setparam

### Concurrency & Performance

| Feature | Commit | Status |
|---------|--------|--------|
| Wait queues | `e4432e6` | ✅ |
| RW-locks | `e4432e6` | ✅ |
| Completions | `e4432e6` | ✅ |
| mmap/munmap/mprotect | `e4432e6` | ✅ |
| SMP work stealing | `e4432e6` | ✅ |
| CPU affinity (sched_setaffinity) | `e4432e6` | ✅ |
| Per-CPU kthread API | `e4432e6` | ✅ |
| SMP boot (AP bringup via SIPI) | `e4432e6` | ✅ |
| Local APIC + I/O APIC | `e4432e6` | ✅ |

### Testing & CI

| Feature | Status |
|---------|--------|
| ccache caching in all CI jobs | ✅ |
| `-j$(nproc)` parallel builds | ✅ |
| `-Werror` strict build job | ✅ |
| cppcheck static analysis (informational) | ✅ |
| In-kernel unit tests in QEMU | ✅ |
| E2E tests via telnet (113 groups) | ✅ |
| virtio-net smoke test | ✅ |
| USB + FAT32 smoke test | ✅ |
| Host-side libc unit tests | ✅ |
| Release automation (tag pushes) | ✅ |
| Doom framebuffer validation test | ✅ |
| `isa-debug-exit` for reliable test VM shutdown | ✅ |
| `[[TEST_DONE]]` sentinel for test completion | ✅ |
| `SKIP_DISK_TESTS` flag for TCG CI speedup | ✅ |
| Test timeout raised to 600s for TCG | ✅ |
| Architecture diagram (SVG dark-themed) | ✅ |

### Kernel Infrastructure

| Feature | Status |
|---------|--------|
| `#pragma` format checking on `kprintf` family | ✅ |
| `-Wformat` catches printf-arg mismatches | ✅ |
| Ramdisk block device for in-memory testing | ✅ |
| Chunked `dmesg` flush (no 64 KB stack frame) | ✅ |
| Production subsystems init (socket, epoll, mq, POSIX timers) | ✅ |
| ELF userspace init binary autodetection | ✅ |
| Enhanced `.gdbinit` with `walk_page` and `ps` macros | ✅ |
| Enhanced procfs (CPU info, meminfo, uptime, mounts) | ✅ |
| PRNG (non-cryptographic random number generator) | ✅ |
| Process CPU time accounting + getrusage (utime/stime/nvcsw/nivcsw/minflt/majflt) | ✅ |
| ITIMER_VIRTUAL + ITIMER_PROF (SIGVTALRM + SIGPROF delivery) | ✅ |
| Interruptible wait queues (signal-aware `wait_queue_sleep_interruptible`) | ✅ |
| `sigwaitinfo`/`sigtimedwait` syscalls (synchronous signal acceptance) | ✅ |
| `/proc/stat` with Linux-compatible user/system/idle columns | ✅ |
| `/proc/uptime` file (uptime + idle seconds) | ✅ |
| Boot ordering fix: `smp_init_bsp()` before `scheduler_init()` (GS_BASE crash) | ✅ |
| Wait queue with timeout: `wait_queue_sleep_timeout()` + interruptible variant | ✅ |
| SCHED_FIFO (run-to-completion) + SCHED_RR (round-robin) scheduling policies | ✅ |
| User/system CPU time separation (CS register check in timer ISR) | ✅ |
| O_CLOEXEC enforcement on `execve` (close-on-exec FD cleanup) | ✅ |

---

## Remaining Improvement Opportunities

### High Priority

1. **Code coverage tracking** — Instrument the kernel with GCOV or QEMU's `-plugin` coverage, run full test suite, report per-file coverage. Identifies dead code and blind spots.

2. **Expand in-kernel test coverage** — Key untested subsystems remain:
   | Subsystem | Lines | Risk | Test coverage |
   |-----------|-------|------|---------------|
   | TCP stack | ~540 | High | Minimal |
   | UDP stack | ~722 | Medium | Minimal |
   | VMM (paging) | ~447 | High | Partial |
   | ELF loader | ~300 | Medium | Partial |
   | FAT32 | ~1,148 | Medium | None in kernel tests |
   | DOS emulator | ~1,877 | High | None |
   | C compiler | ~4,500 | Medium | None |
   | Syscall dispatch | ~4,803 | High | Partial |
   | IPC (shm, mutex, semaphore) | ~350 | Medium | Minimal |
   | Socket layer | ~300 | Medium | None |

3. **Fuzz testing** — Boot kernel under QEMU fuzzing: inject random PCI config reads, random network packets, random keyboard/mouse input. Catch driver desync that deterministic tests miss.

### Medium Priority

4. **Network test infrastructure** — Add concurrent TCP connections, UDP fragmentation, ICMP echo with varied payloads, DNS edge cases (NXDOMAIN, CNAME), HTTP throughput benchmark.

5. **Memory-safety stress tests** — Targeted tests for heap OOM, fragmentation stress, VMM page fault recovery, COW semantics, pipe saturation with many processes.

6. **Kernel lock validator** — Runtime deadlock detection for spinlocks, mutexes, and RW-locks (akin to Lockdep). Essential for SMP correctness.

### Lower Priority

7. **Instrumentation-based tracing** — Static tracepoints in scheduler, syscall entry/exit, VFS operations, and network stack. Use QEMU's trace-events infrastructure for zero-overhead when disabled.

8. **Perf subsystem** — Basic `perf`-like counting of page faults, context switches, cache references, TLB flushes, IRQ counts. Expose via sysfs or a new syscall.

9. **Automated regression benchmarks** — Boot-time measurement, syscall latency, context-switch latency, TCP throughput. Track deltas across commits.
