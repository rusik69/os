# Changelog

All notable changes to Hermes OS are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [2026.06] — 2026-06-14

### Added

#### Batch B51–B100 — Modularisation, Clustering, Security, and Containers

- **Modular kernel transition (Phases 1–4)**:
  - Symbol export table with `.ksymtab` section for kernel symbol resolution.
  - Full ELF module loader (`module_elf.c`) with relocation support for ET_REL.
  - Module dependency tracking with reference-counted DAG (`module_deps.c`).
  - Module autoloader with alias-based `modprobe` support (`module_autoload.c`).
  - Module compression (gzip/xz) with transparent decompression (`module_compress.c`).
  - Module signature verification (RSA/SHA-256 PKCS#7) (`module_signature.c`).
  - Asynchronous module loading for faster boot (`module_async.c`).
  - Driver and filesystem modules: e1000, nvme, ahci, usb, ext2, fat32, iso9660, tarfs, romfs, debugfs, sysfs, devfs, overlay, doom, dos, gui, compiler.

- **Cluster subsystem**:
  - Raft-based consensus and replicated key-value store (`cluster/raft.c`, `cluster/raft_kv.c`).
  - Gossip protocol for node discovery and failure detection (`cluster/gossip.c`).
  - Overlay network with mesh routing (`cluster/overlay.c`, `cluster/mesh.c`).
  - Network policy engine with L7 filtering (`cluster/network_policy.c`).
  - Horizontal Pod Autoscaler (HPA) with custom metrics (`cluster/hpa.c`).
  - Custom Resource Definition (CRD) framework (`cluster/crd.c`).
  - Runtime security enforcement with seccomp profiles (`cluster/runtime_security.c`).
  - Rolling cluster upgrade manager (`cluster/upgrade.c`).
  - Node problem detection and remediation (`cluster/node_problem.c`).

- **Container runtime**:
  - OCI-compatible container lifecycle (create, start, exec, stop, delete).
  - Container image management with layer caching and GC (`container/image.c`).
  - Container storage (overlayfs + volume mounts) (`container/storage.c`).
  - Container networking (CNI-compatible bridge, veth pairs) (`container/network.c`).
  - Orchestrator integration: pod scheduling, service proxies, health checks (`container/orch.c`, `container/service_proxy.c`).
  - Seccomp notify for dynamic security profiles (`container/seccomp_notify.c`).
  - Checkpoint/restore for container migration (`container/checkpoint.c`).

- **Security enhancements**:
  - KPTI (Kernel Page Table Isolation) with assembly trampoline.
  - SMAP/SMEP/UMIP activation at boot.
  - KASLR for kernel text, modules, and heap.
  - W^X enforcement with `mprotect` hardening.
  - Exec Shield with stack gap randomisation, mmap base entropy.
  - Shadow Call Stack (SCS) for ROP protection.
  - Forward-edge CFI (Control Flow Integrity).
  - Seccomp BPF filter validation engine.
  - Landlock LSM with filesystem sandboxing.
  - Lockdown mode (integrity + confidentiality levels).
  - IMA (Integrity Measurement Architecture) with TPM-compatible PCR.
  - EVM for extended attribute protection.
  - Audit subsystem with syscall and file access logging.
  - Kernel address pointer restriction (`%pK`).
  - Dmesg restriction (only `CAP_SYSLOG` readers).
  - KASAN (lightweight) for out-of-bounds and use-after-free detection.
  - KFENCE for low-overhead memory error detection.
  - KCSAN for data race detection.

- **Capability and access control**:
  - POSIX capability system (effective, permitted, inheritable, ambient sets).
  - Secure execution with ambient capability clearing on exec.
  - Namespace support: PID, network, mount, user, cgroup.
  - Chroot with pivot_root support.
  - Process rlimit enforcement.

- **Power management**:
  - CPU frequency scaling governors (ondemand, conservative, userspace, schedutil).
  - CPU idle governors (ladder, teo, menu).
  - Device frequency scaling (devfreq).
  - Energy model for power-aware scheduling.
  - Suspend-to-idle (s2idle) with wakeup sources.
  - Power management quality of service (PM QoS).

- **I/O and storage**:
  - Multi-queue block layer (blk-mq).
  - I/O schedulers: deadline, completely fair queueing, Kyber.
  - Device mapper: linear, zero, error, crypt, verity, snapshot, raid.
  - MD RAID (linear, RAID0, RAID1, RAID5, RAID10).
  - Bcache for SSD caching of HDD volumes.
  - LUKS disk encryption with AES-XTS.
  - NBD (Network Block Device) with multi-connection.
  - MPTCP (MultiPath TCP) for multipath networking.
  - SCTP and DCCP transport protocols.
  - WireGuard VPN with ChaCha20Poly1305.

- **Virtualisation and para-virtualisation**:
  - Virtio-net, virtio-blk, virtio-gpu, virtio-input, virtio-rng, virtio-scsi, virtio-console.
  - PV panic device, IVSHMEM for VM shared memory.
  - VMware balloon and pvSCSI drivers.

- **Hardware support**:
  - Intel GPU (Bochs VBE, native modesetting) with DRM framework.
  - USB core: EHCI, XHCI, hub, HID, mass storage, CDC ACM, serial, ethernet, UAS.
  - ACPI CPPC (Collaborative Processor Performance Control).
  - PCIe: AER, DPC, PTM, SR-IOV.
  - EDAC for error detection and correction.
  - GHES (Generic Hardware Error Source) for APEI.
  - IPMI KCS interface.
  - TPM 2.0 TIS driver.
  - I3C bus support.
  - GPIO IRQ chip for interrupt-driven GPIO.

- **File systems**:
  - ext2 read/write with HTree directory indexing, extended attributes, POSIX ACLs.
  - FAT32 read/write with VFAT long filenames.
  - ISO9660 with Rock Ridge and Joliet extensions.
  - HFS, cramfs, minix, UFS, SYSV, ADFS, BFS (read-only legacy support).
  - NFSv3 client.
  - EROFS, F2FS, JFFS2, NILFS2 (flash-optimised filesystems).
  - SquashFS for compressed read-only images.
  - OverlayFS with copy-up-on-write, whiteouts, and redirect directories.
  - FUSE for userspace filesystem daemons.
  - Verity (dm-verity-style hash tree validation for files).
  - Fanotify for filesystem event monitoring.

- **Memory management**:
  - Multi-Gen LRU (MGLRU) for page reclaim.
  - Zswap with compressed write-back cache.
  - ZRAM with multiple compression algorithms (LZ4HC, ZSTD, LZO).
  - ZBUD, ZSMALLOC for small-object page compression.
  - THP (Transparent Huge Pages) with khugepaged.
  - KSM (Kernel Same-page Merging) for page deduplication.
  - DAMON for proactive data access monitoring.
  - CMA (Contiguous Memory Allocator) for large DMA buffers.
  - Memory hotplug and online/offline support.
  - OOM killer with cgroup-aware badness scoring.
  - memfd + sealing (mseal) for immutable shared memory.

- **Observability and tracing**:
  - Perf events subsystem with HW/SW counters, sampling, and tracepoints.
  - Ftrace with function tracer, graph tracer, and event tracing.
  - Kprobes and kretprobes for dynamic instrumentation.
  - Jump labels for static branch patching.
  - Kcov for coverage-guided fuzzing.
  - Taskstats and delay accounting.
  - PSI (Pressure Stall Information) for resource pressure monitoring.

- **Orchestration**:
  - Pod lifecycle management with health checks and readiness probes.
  - Service mesh with mTLS and circuit breaking.
  - RBAC with role and role binding CRDs.
  - Secrets management with encryption at rest.
  - Pod security policies with seccomp and AppArmor profiles.
  - Event system with watch-based subscriptions.
  - Log aggregation and dashboard metrics.
  - Prometheus-compatible metrics endpoint.

- **Testing infrastructure**:
  - KUnit framework for in-kernel unit tests.
  - KUnit tests for PMM, slab, scheduler, VMM, security, power, cluster, container.
  - E2E test suite with QEMU-based integration tests.
  - Doom framebuffer pixel validation test.
  - Host-side unit tests for libc and shell utilities.
  - CI pipeline with GitHub Actions (build, lint, test-kernel, e2e-smoke).

### Changed

- Build system: ccache and distcc auto-detection for faster rebuilds.
- Build system: precompiled headers (PCH) for ~2-3x compilation speedup.
- Build system: per-profile Hermes AI assistant configuration.
- Linker script: `.ksymtab`, `.modinfo`, `.kcrctab` sections for module support.
- Build config: auto-generated `/proc/config.gz` with `xxd -i` embedding.
- Init order: KPTI trampoline early in boot sequence.
- Memory layout: separate PML4 entry for KPTI user page tables.

### Fixed

- Race condition in task scheduler runqueue iteration.
- Double-free in slab cache destructor path.
- Use-after-free in VMA merge logic during `mremap`.
- Stack overflow in recursive path resolution for symlinks.
- Missing SMAP save/restore in nested interrupt handlers.
- Interrupt reentrancy in APIC timer calibration.
- NMI handler stack corruption on SMP.
- TLB flush coherence after page table modification on secondary CPUs.
- Over-accounting in cgroup memory controller.
- Missing `CLONE_VFORK` wait in process spawn.

### Security

- KPTI enabled by default to mitigate Meltdown.
- SMAP/SMEP/UMIP enforced at boot.
- Module signing mandatory for loadable modules.
- Landlock LSM for unprivileged filesystem sandboxing.
- Lockdown mode prevents kernel tampering even by root.
- Kernel pointer and dmesg restriction by default.
- Stack protector switched to `-fstack-protector-strong` globally.

---

## [2026.05] — 2026-05-30

### Added

- Initial bootable kernel with GRUB Multiboot1 support.
- Basic x86-64 long mode with PAE paging (1 GB identity map + high-half kernel).
- Physical memory manager (bitmap-based).
- Virtual memory manager with demand paging and `mmap`/`munmap`.
- Slab allocator (kmem_cache) with magazine layer and colouring.
- Preemptive scheduler with CFS, FIFO, RR policies.
- SMT/Hyper-Threading aware scheduling.
- ELF loader for userspace executables.
- System call dispatch (80+ syscalls).
- VFS with tmpfs, procfs, devfs, sysfs, debugfs.
- FAT32 read/write filesystem.
- ISO9660 read-only filesystem with Rock Ridge.
- PCI bus enumeration and device discovery.
- ACPI subsystem (MADT, DSDT, SSDT) for multiprocessor boot.
- SMP bringup with APIC and IPI.
- ATA PIO, AHCI SATA, NVMe storage drivers.
- e1000 network driver with IP/TCP/UDP stack.
- Serial port, keyboard, mouse, VGA text/graphics drivers.
- Shell with command table, scripting, variables, job control.
- Network services: telnetd, httpd, SSH client/server.
- WireGuard VPN.
- LUKS disk encryption.
- Device mapper (linear, zero, error, crypt, verity).
- Basic container runtime.
- initramfs with `/init` userspace process.
- Build system with Makefile, linker script, GRUB image generation.
- QEMU run targets with debug and test support.

### Changed

- N/A — initial release.

### Fixed

- N/A — initial release.

### Security

- NX bit enabled for all kernel and user pages.
- Basic ASLR for kernel and userspace.
- Stack canary enabled.
- Simple seccomp without BPF.
