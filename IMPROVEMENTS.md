# OS — Improvement Plan

Based on thorough codebase analysis. 38,468 LOC across ~60 subsystems, 113 E2E tests, ~30 in-kernel tests, 3 CI workflows. Clean codebase (zero TODO/FIXME comments found). Here are the highest-impact improvements, grouped by effort.

---

## Quick Wins (1-2 hours each)

### 1. CI: Add dependency caching
Current CI installs `gcc-x86-64-linux-gnu qemu nasm python3` every run (~30s overhead). Add `apt` caching and `ccache` to shave 30-60s off each build.

### 2. CI: Parallel test jobs
Currently `test` (kernel unit tests) and `e2e` run sequentially. They don't depend on each other — split into parallel jobs. Also add a virtio-net + USB/FAT smoke job to the main CI matrix instead of separate workflow files.

### 3. Build: Enable parallel compilation
Makefile runs single-threaded. Add `-j$(nproc)` to the default build or detect available cores.

### 4. CI: Build with `-Werror`
Add `-Werror` to CI builds (or a separate job) so new warnings break the build instead of silently accumulating.

---

## Medium Effort (half-day each)

### 5. CI: Static analysis (clang-tidy / cppcheck)
Add a CI job running `clang-tidy` or `cppcheck` on the C sources. The kernel has its own libc with no `printf` format checking — static analysis catches buffer overflows, null derefs, memory leaks, and format-string mismatches.

### 6. Expand in-kernel test coverage
Current tests cover: string, PMM, heap, timer, RTC, process, scheduler, FS, VFS, pipes, speaker, mouse, signals, procfs, fork. **Untested subsystems:**
| Subsystem | Lines | Risk |
|-----------|-------|------|
| ELF loader | ~300 | Medium |
| Syscall dispatch | ~1,751 | High |
| TCP stack | ~540 | High |
| UDP stack | ~722 | Medium |
| IPC (shm) | ~150 | Medium |
| Mutex / Semaphore | ~200 | Medium |
| VMM (paging) | ~447 | High |
| ATA/AHCI block | ~680 | Medium |
| FAT32 | ~1,148 | Medium |
| DOS emulator | ~1,877 | High |
| C compiler | ~4,500 | Medium |
| Signal delivery (edge cases) | ~300 | Medium |

### 7. Add memory-safety tests
Write targeted tests for:
- Heap: OOM handling, fragmentation stress, large allocations
- VMM: page fault recovery, COW semantics, unmapped access
- Stack: kernel stack overflow detection
- Pipe: blocking write/read with multiple processes, saturation

### 8. Host-side unit tests (no QEMU)
Extract pure-logic modules (string, stdlib, printf, math) into host-compilable test files. Run them natively in CI — instant feedback, no boot. This catches regressions in libc/utility code in under a second.

---

## Larger Projects (1-3 days each)

### 9. Architecture documentation
The README is comprehensive (1,276 lines) but flat. Add:
- **ARCHITECTURE.md** — subsystem map with dependency graph, boot flow, memory layout
- A rendered architecture diagram (we already have one!) committed to the repo
- **CONTRIBUTING.md** — coding conventions, test expectations, PR workflow
- API docs for the libc/header surface

### 10. Code coverage tracking
Instrument the kernel build with GCOV or similar, run the full test suite in QEMU, and report coverage. This tells you exactly which code paths the tests **don't** exercise — far more valuable than raw test counts.

### 11. Network test infrastructure
Current E2E uses QEMU user-mode networking (`-netdev user`). Add:
- DHCP renewal stress tests
- TCP concurrent connection tests (multiple simultaneous sessions)
- UDP packet fragmentation
- ICMP echo with payload sizes
- DNS resolution edge cases (NXDOMAIN, CNAME chains, IPv6 records)
- HTTP server throughput benchmark

### 12. Fuzz testing
Boot the kernel with a synthetic device model (or use QEMU's fuzzing capabilities) that injects random PCI config space reads, random network packets, random keyboard/mouse input. Catches driver bugs and desync issues that deterministic tests never find.

### 13. Build system modernization
- Add `ccache` support (stamp the Makefile to cache .o files)
- Add `make debug` and `make release` build profiles
- Add `make format` target using clang-format
- Add `make lint` target wrapping cppcheck + grep for suspicious patterns
- Track build timestamps per subsystem

### 14. SMP / Multi-core bootstrap
This is a major kernel feature — AP bringup, IPI handling, per-CPU data structures, spinlocks, TLB shootdown. The payoff: actual parallelism for the scheduler and real concurrency testing. Start with a `smp.c` that parses ACPI MADT and brings up one AP.

### 15. GDB debugging improvements
- Add a `.gdbinit` with KASLR offset detection, process list pretty-printers, page table walk helpers
- Document the debug workflow (QEMU `-s -S`, `target remote :1234`) in a DEBUGGING.md
- Add `make gdb` target that launches QEMU in debug-wait mode + GDB

---

## Maintenance / Hygiene

### 16. Y2038 audit
Check all time-related code for `uint32_t` epoch timestamps. The RTC and FS code should use 64-bit time.

### 17. PCI BAR handling audit
Current PCI code may not handle 64-bit BARs correctly (common on modern QEMU devices). Add a test that enumerates all PCI devices and validates BAR assignment.

### 18. Sort and deduplicate header includes
Run `include-what-you-use` or a simple script to check for unused includes and missing explicit includes. Reduces build time and prevents silent API drift.

### 19. CI: Automate release artifacts
Add a CI job that builds the kernel and attaches `kernel.bin` + `disk.img` as release artifacts on tag pushes. Makes it trivial for anyone to grab a build.

---

## Summary Priority Matrix

| Priority | Effort | Item |
|----------|--------|------|
| 🔴 P0 | Quick | Parallel CI + -Werror + ccache |
| 🔴 P0 | Medium | Expand test coverage (ELF, TCP, VMM, syscalls, DOS) |
| 🔴 P0 | Medium | Host-side unit tests for libc |
| 🟡 P1 | Quick | Static analysis in CI |
| 🟡 P1 | Medium | Architecture docs + diagram |
| 🟡 P1 | Large | Code coverage tracking |
| 🟢 P2 | Medium | Network test infra improvements |
| 🟢 P2 | Large | SMP bootstrap |
| 🟢 P2 | Medium | GDB debugging support |
| ⚪ P3 | Quick | Y2038 audit |
| ⚪ P3 | Medium | Release artifacts in CI |
| ⚪ P3 | Medium | PCI BAR audit |
