# Hermes OS Security Architecture

Comprehensive overview of the security features, mechanisms, and architecture of Hermes OS. This document covers kernel-level protections, userspace isolation, the capability model, and the Linux Security Module (LSM) framework.

---

## Table of Contents

1. [Security Principles](#1-security-principles)
2. [Kernel-Userspace Isolation](#2-kernel-userspace-isolation)
3. [KPTI (Kernel Page Table Isolation)](#3-kpti-kernel-page-table-isolation)
4. [SMAP / SMEP / UMIP](#4-smap--smep--umip)
5. [KASLR and ASLR](#5-kaslr-and-aslr)
6. [W^X Enforcement](#6-wx-enforcement)
7. [Seccomp](#7-seccomp)
8. [Landlock LSM](#8-landlock-lsm)
9. [Lockdown Mode](#9-lockdown-mode)
10. [YAMA Security Module](#10-yama-security-module)
11. [Capability Model](#11-capability-model)
12. [LSM Framework](#12-lsm-framework)
13. [Integrity Measurement (IMA/EVM)](#13-integrity-measurement-imaevm)
14. [Signed Modules and Firmware](#14-signed-modules-and-firmware)
15. [Stack and Heap Protections](#15-stack-and-heap-protections)
16. [Exec Shield and CFI](#16-exec-shield-and-cfi)
17. [Audit and Logging](#17-audit-and-logging)
18. [Threat Model](#18-threat-model)

---

## 1. Security Principles

Hermes OS is designed with **defence in depth** as the core principle. No single mechanism bears the full burden of security; instead, multiple overlapping protections ensure that a failure in one layer is caught by another.

**Key tenets:**
- **Least privilege** — processes and modules run with the minimum capabilities needed.
- **Privilege separation** — kernel and userspace are strictly isolated with hardware-enforced boundaries.
- **Default deny** — access to sensitive operations requires explicit capability grants.
- **Fail secure** — if a security mechanism cannot determine a decision, it denies access.
- **Verifiable integrity** — code integrity is verified at load time via signatures and hashing.

---

## 2. Kernel-Userspace Isolation

The kernel occupies the **high half** of the 64-bit virtual address space (`0xFFFF800000000000+`) while userspace lives in the **low half** (`0x0000000000000000`–`0x00007FFFFFFFFFFF`). This separation is enforced by the MMU page tables:

```
Virtual Memory Layout (per-process):
  0x0000000000000000 - 0x00007FFFFFFFFFFF  : Userspace
  0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF  : Kernel (shared, read-only to users)
```

- Userspace code **cannot** access kernel pages (supervisor-only bit in page table entries).
- The kernel **can** access userspace pages (via `copy_from_user` / `copy_to_user` wrappers that respect SMAP).
- Syscall entry/exit flushes the GSBASE/FSBASE MSRs to prevent speculative leaks.

---

## 3. KPTI (Kernel Page Table Isolation)

KPTI mitigates Meltdown-type side-channel attacks by using separate page tables for kernel and userspace execution.

### How it works

- **User page tables** contain only the minimal kernel entries needed for syscall/interrupt entry (the trampoline pages).
- **Kernel page tables** contain the full kernel mapping.
- On syscall entry (`syscall` instruction or interrupt), the CPU switches to kernel page tables via a trampoline.
- On return to userspace, page tables are switched back, and userspace-accessible TLB entries are flushed.

### Implementation

The KPTI trampoline (`src/kernel/kpti_trampoline.asm`) is a flat binary embedded in the kernel image. It performs:

1. Switch CR3 to kernel page table root.
2. Adjust GS.base for kernel stack.
3. Jump to the real syscall/interrupt handler.

On exit, the trampoline:
1. Flushes non-global TLB entries.
2. Wipes sensitive CPU state.
3. Switches CR3 back to user page table root.
4. Returns to userspace (sysret/iretq).

KPTI is always enabled when `CONFIG_KPTI=y` (set by default).

---

## 4. SMAP / SMEP / UMIP

These are x86-64 hardware protection features enabled at boot time:

| Feature | CR4 Bit | Effect |
|---------|---------|--------|
| **SMAP** (Supervisor Mode Access Prevention) | `CR4.SMAP` (bit 21) | Prevents the kernel from accessing userspace memory unless explicitly allowed via the `AC` (Alignment Check) flag. The `copy_from_user`/`copy_to_user` functions temporarily clear `AC`. |
| **SMEP** (Supervisor Mode Execution Prevention) | `CR4.SMEP` (bit 20) | Prevents the kernel from executing userspace code. If the instruction pointer points to a user page while in ring 0, a page fault is raised. |
| **UMIP** (User Mode Instruction Prevention) | `CR4.UMIP` (bit 11) | Prevents userspace from executing privileged instructions (`SGDT`, `SIDT`, `SLDT`, `SMSW`, `STR`). |

These protections are set in `src/kernel/smap_smep_umip.c` and are activated early in `kernel_main`.

---

## 5. KASLR and ASLR

### Kernel ASLR (KASLR)

The kernel text, modules, and heap are loaded at random offsets:
- **Text base** — The kernel is linked at a fixed virtual address, but a random offset (`kaslr_offset`) is added during early boot before paging is fully enabled.
- **Module region** — Modules are loaded at a random virtual address within the module region.
- **Heap base** — `kmalloc` also randomises the initial arena offset.

### Userspace ASLR

Each process gets randomised:
- **Stack base** — Offset from the top of the user address space.
- **Heap break** — Randomised via `brk` / `mmap` base.
- **ELF base** — PIE executables are loaded at a random base address.
- **VDSO page** — Randomised per process.

ASLR entropy sources: `src/kernel/aslr.c` uses `rdrand` (when available) or a SHA-256-based PRNG seeded from hardware RNG.

---

## 6. W^X Enforcement

Write XOR eXecute (`W^X`) ensures that no memory page is simultaneously writable and executable. This prevents shellcode injection attacks:

- **Kernel pages**: Code sections (`.text`) are read+execute; data sections (`.data`, `.bss`) are read+write; both are enforced by the page table NX bit.
- **Userspace pages**: `mmap` with `PROT_WRITE | PROT_EXEC` returns `-EACCES`. JIT compilers must use the `mprotect` sequence: write (no exec) → exec (no write).
- **Module code**: Loadable `.ko` modules have their text section set to read+execute after relocation, with all other sections non-executable.

Enforcement is in `src/kernel/wx_enforce.c` and is always active when `CONFIG_NX_ENFORCE=y`.

---

## 7. Seccomp

Seccomp (Secure Computing Mode) allows a process to install a filter that restricts the syscalls it may invoke. Hermes OS supports three modes:

| Mode | Behaviour |
|------|-----------|
| **SECCOMP_SET_MODE_STRICT** | Only `read`, `write`, `exit`, `return` (sigreturn) are allowed. Any other syscall kills the process. |
| **SECCOMP_SET_MODE_FILTER** | Userspace provides a BPF program that is evaluated on every syscall. The BPF program can allow, deny, kill, or trap. |
| **SECCOMP_RET_LOG** | Log the syscall but allow it (for auditing/development). |
| **SECCOMP_RET_KILL_PROCESS** | Immediately kill the thread group (default action for denied calls). |
| **SECCOMP_RET_TRAP** | Raise a `SIGSYS` with `siginfo` describing the denied syscall. |

Seccomp is implemented in `src/kernel/seccomp.c` and `src/kernel/seccomp_bpf.c`. The BPF filter is validated to ensure termination (§ 4 of BPF ISA), and `CONFIG_SECCOMP_BPF=y` enables the full `seccomp_data` reporting.

---

## 8. Landlock LSM

Landlock is an LSM that enables unprivileged processes to create fine-grained access-control sandboxes. Unlike seccomp which restricts syscalls, Landlock restricts **filesystem access** — which files, directories, and hierarchies a process can read/write/execute.

### Key concepts

- **Ruleset** — A collection of access rules (e.g., "allow read on /home/user").
- **Handled accesses** — The set of access types the ruleset controls (e.g., `LANDLOCK_ACCESS_FS_READ_FILE`, `LANDLOCK_ACCESS_FS_WRITE_FILE`, `LANDLOCK_ACCESS_FS_EXECUTE`).
- **Enforcement** — Once a ruleset is applied to the current process with `landlock_restrict_self()`, it cannot be removed except by `execve` (if the new program has no Landlock restriction).

Landlock is implemented in `src/kernel/landlock.c` and is always built when `CONFIG_SECURITY_LANDLOCK=y`.

---

## 9. Lockdown Mode

Lockdown mode restricts access to kernel features that could be used to bypass other security mechanisms. It has two levels:

| Level | Effect |
|-------|--------|
| **integrity** | Modules must be signed; `/dev/mem`, `/dev/kmem`, `kprobes`, `kexec`, and hibernation are blocked; MSR writes and ACPI table overrides are forbidden. |
| **confidentiality** | All integrity restrictions **plus** `dmesg` restriction (privileged users only), `/proc/kcore` disabled, kernel addresses hidden. |

Lockdown is enforced in `src/kernel/lockdown.c`. A locked-down kernel cannot be unlocked without a reboot. `CONFIG_LOCKDOWN=y` enables this feature; the default level is `integrity`.

---

## 10. YAMA Security Module

YAMA is a Linux Security Module that extends `ptrace` restrictions:

| `kernel.yama.ptrace_scope` | Behaviour |
|:---:|---|
| 0 | No restrictions — any process can ptrace any other process. |
| 1 (**default**) | Only parent-to-child ptrace is allowed (the tracer must be the tracee's direct ancestor). |
| 2 | Only `CAP_SYS_PTRACE` processes or explicit `PR_SET_PTRACER` allow ptrace. |
| 3 | No ptrace at all. Hardened — cannot be lowered without reboot. |

YAMA is in `src/kernel/yama.c`. `CONFIG_SECURITY_YAMA=y` enables it with default scope level 1.

---

## 11. Capability Model

Hermes OS implements a POSIX-like capability system where the effective set, permitted set, and inheritable set are tracked per-process:

```
struct cred {
    uint64_t    uid;          /* real user ID */
    uint64_t    gid;          /* real group ID */
    uint64_t    euid;         /* effective user ID */
    uint64_t    egid;         /* effective group ID */
    cap_t       effective;    /* currently active capabilities */
    cap_t       permitted;    /* capabilities this process may use */
    cap_t       inheritable;  /* capabilities preserved across exec */
    cap_t       ambient;      /* capabilities inherited without setuid */
};
```

Key capabilities include:

| Capability | Description |
|------------|-------------|
| `CAP_CHOWN` | Change file ownership |
| `CAP_DAC_OVERRIDE` | Bypass discretionary access checks |
| `CAP_NET_RAW` | Open raw sockets |
| `CAP_SYS_ADMIN` | Broad administrative privileges |
| `CAP_SYS_MODULE` | Load/unload kernel modules |
| `CAP_SYS_PTRACE` | Trace arbitrary processes |
| `CAP_SYS_BOOT` | Trigger reboot/hibernate |
| `CAP_NET_BIND_SERVICE` | Bind to privileged ports (<1024) |

Capabilities are inherited securely: a process without `CAP_SETPCAP` cannot gain new capabilities, and the ambient set is cleared on `execve` unless explicitly preserved.

---

## 12. LSM Framework

The Linux Security Module (LSM) framework provides a hook-based approach to security policy enforcement. Hermes OS uses a stacked LSM architecture:

```
Syscall entry → LSM hook chain → operation permitted/denied
                                     ↓
                              LSM returns 0 (allow)
                              LSM returns -EPERM (deny)
```

### Implemented LSMs

| LSM | File | Purpose |
|-----|------|---------|
| YAMA | `src/kernel/yama.c` | ptrace scope restrictions |
| Landlock | `src/kernel/landlock.c` | Filesystem sandboxing |
| IMA | `src/kernel/ima.c` | Integrity measurement |
| Lockdown | `src/kernel/lockdown.c` | Restrict kernel tampering |

### LSM hook points

The framework hooks into ~50 syscall and kernel event points, including:

- `file_permission` — Check file open/read/write/execute
- `file_ioctl` — Validate ioctl commands
- `task_ptrace` — Control ptrace relationships
- `bprm_check_security` — Executable validation
- `mmap_file` — Memory mapping checks
- `socket_create` — Socket creation control
- `module_load` — Module loading restrictions
- `kernel_read_file` — Firmware and kernel image verification

---

## 13. Integrity Measurement (IMA/EVM)

**IMA** (Integrity Measurement Architecture) measures file content hashes before access and stores them in a runtime measurement list:

- Hash algorithm: SHA-256
- Template format: `ima-ng` (name + hash)
- PCR extension: Compatible with TPM 2.0

**EVM** (Extended Verification Module) protects extended attributes (xattrs) containing security metadata:

```
security.evm → HMAC-SHA256(key, {uid, gid, mode, capabilities})
```

IMA is implemented in `src/kernel/ima.c`, `src/kernel/ima_policy.c`, and `src/kernel/ima_appraise.c`. EVM is in `src/kernel/evm.c`.

---

## 14. Signed Modules and Firmware

All loadable kernel modules must be signed if `CONFIG_MODULE_SIG=y`:

- Signature format: PKCS#7 detached signature appended to `.ko`.
- Signing key: RSA 4096-bit key pair generated at build time.
- Verification: Kernel's built-in public key validates the signature before calling `module_init`.

Firmware loading (`src/kernel/firmware.c`) similarly checks the `CONFIG_FW_LOADER` path for signature verification of firmware blobs.

---

## 15. Stack and Heap Protections

| Protection | Mechanism | Enabled By |
|------------|-----------|------------|
| Stack canary | `-fstack-protector-strong` places a random guard value before local arrays; checked on function return. | `CONFIG_STACKPROTECTOR` |
| Stack guard page | Guard page below each kernel stack to detect overflows (2 pages available, 1 guard). | `CONFIG_STACK_GUARD` |
| Heap redzone | `SLAB_REDZONE` places redzone markers around slab allocations. | `CONFIG_SLAB_REDZONE` |
| Freelist hardening | `SLAB_FREELIST_HARDENED` adds pointer mangling to slab freelists. | `CONFIG_SLAB_FREELIST_HARDENED` |
| Page poisoning | Free pages are filled with `0xDC` poison pattern; use-after-free reads detect the pattern. | `CONFIG_PAGE_POISONING` |
| KASAN | Compiler-based out-of-bounds and use-after-free detection (lightweight variant). | `CONFIG_KASAN` |
| Exec Shield | Randomised placement of stack, mmap, and heap; NX enforcement. | `CONFIG_EXECSHIELD` |
| SCS (Shadow Call Stack) | Hardware-independent shadow stack for return address protection. | `CONFIG_SCS` |
| CFI (Control Flow Integrity) | Forward-edge CFI via compiler instrumentation. | `CONFIG_CFI` |

---

## 16. Exec Shield and CFI

**Exec Shield** (`src/kernel/execshield.c`) is a comprehensive memory safety subsystem combining:
- Randomised mmap base (256 possible positions).
- Stack gap randomisation (random padding between stack regions).
- NX enforcement at the `mmap`/`mprotect` syscall level.
- `mprotect` hardening: `W|X` transitions force an intermediate `R` (read-only) step.

**CFI** (Control Flow Integrity) compiler instrumentation checks indirect call targets against a whitelist of valid function entry points. A CFI violation triggers a kernel panic or `SIGSEGV` depending on context.

---

## 17. Audit and Logging

Hermes OS includes a comprehensive audit subsystem (`src/kernel/audit.c`):

- **Audit log**: Records security-relevant events (syscall invocation, file access, module load, login/logout).
- **DMESG restrict**: When `CONFIG_DMESG_RESTRICT=y`, only `CAP_SYSLOG` processes (or root) can read the kernel log buffer.
- **KPTR restrict**: When `CONFIG_KPTR_RESTRICT=y`, `%pK` format specifier hides kernel pointers from unprivileged readers.
- **Dynamic log control**: `CONFIG_DYNAMIC_DEBUG=y` enables selective kernel-message printing without a rebuild.

---

## 18. Threat Model

| Threat | Mitigation |
|--------|-----------|
| Userspace kernel memory read (Meltdown) | KPTI |
| Userspace kernel memory write | SMAP + NX + W^X |
| Userspace code execution in kernel | SMEP |
| Privilege escalation via module loading | Module signing + Lockdown |
| Arbitrary syscall by sandboxed process | Seccomp BPF |
| Filesystem sandbox escape | Landlock LSM |
| Backdoored kernel image | IMA + module signing |
| Stack buffer overflow | Stack canary + SCS + CFI |
| Heap use-after-free | KASAN + redzones + freelist hardening |
| Return-oriented programming (ROP) | SCS + CFI + W^X |
| Side-channel (Spectre v1/v2) | LFENCE serialisation + retpolines |
| Side-channel (L1TF / Foreshadow) | L1D cache flush on VM entry |

---

## Configuration Reference

Key security config options:

```kconfig
CONFIG_SECURITY=y              # Enable LSM framework
CONFIG_SECCOMP=y               # Seccomp syscall filtering
CONFIG_SECCOMP_BPF=y           # BPF-based seccomp filters
CONFIG_LANDLOCK=y              # Landlock LSM
CONFIG_LOCKDOWN=y              # Lockdown mode
CONFIG_YAMA=y                  # YAMA ptrace scope
CONFIG_INTEGRITY=y             # IMA/EVM integrity
CONFIG_IMA=y                   # Integrity Measurement Architecture
CONFIG_KPTI=y                  # Kernel Page Table Isolation
CONFIG_SMAP=y                  # Supervisor Mode Access Prevention
CONFIG_SMEP=y                  # Supervisor Mode Execution Prevention
CONFIG_UMIP=y                  # User Mode Instruction Prevention
CONFIG_ASLR=y                  # Kernel + userspace ASLR
CONFIG_NX_ENFORCE=y            # W^X enforcement
CONFIG_STACKPROTECTOR=y        # Stack canary
CONFIG_KPTR_RESTRICT=y         # Hide kernel pointers
CONFIG_DMESG_RESTRICT=y        # Restrict kernel log access
CONFIG_MODULE_SIG=y            # Signed modules
CONFIG_CFI=y                   # Control Flow Integrity
CONFIG_SCS=y                   # Shadow Call Stack
CONFIG_EXECSHIELD=y            # Exec Shield randomisation
```

---

## References

- `src/kernel/smap_smep_umip.c` — SMAP/SMEP/UMIP setup
- `src/kernel/kpti.c` — KPTI implementation
- `src/kernel/seccomp.c` — Seccomp core
- `src/kernel/seccomp_bpf.c` — BPF filter engine
- `src/kernel/landlock.c` — Landlock LSM
- `src/kernel/lockdown.c` — Lockdown mode
- `src/kernel/yama.c` — YAMA ptrace scope
- `src/kernel/ima.c` — Integrity Measurement Architecture
- `src/kernel/execshield.c` — Exec Shield
- `src/kernel/cfi.c` — Control Flow Integrity
- `src/kernel/scs.c` — Shadow Call Stack
- `src/kernel/audit.c` — Audit subsystem
- `src/kernel/caps.c` — Capability management
