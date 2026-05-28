# Process Isolation TODO

**Goal:** Processes should be fully isolated from each other and from the kernel.
Processes should not be able to read each other's memory or kernel memory, and should
only communicate through controlled IPC (pipes, shared memory with explicit permissions,
signals, and syscalls).

## Current State

- Processes can access each other's page tables via kernel-managed per-process page
  tables, but Ring 3/4 separation is incomplete.
- Kernel memory is identity-mapped in the lower 1 GB; no kernel memory isolation.
- No address space randomization (ASLR).
- No stack overflow detection for kernel stacks.
- Shared memory has no permission checks (all processes can attach all segments).

## Implementation Phases

### Phase 1: Memory Isolation (Kernel Page Table Hardening)

**1.1 Remove identity map from kernel page tables**
- Currently `identity_map_first_gb()` maps physical addresses 0-1GB at the same
  virtual address. This lets Ring 0 code access any physical memory directly.
- **Task:** Remove or limit the identity map. Kernel code should only access memory
  through the high-half VMA mapping (`KERNEL_VMA_OFFSET = 0xFFFF800000000000`).
- **Risk:** Interrupt handlers may rely on identity-mapped addresses for ISR stacks.
  Audit `idt_asm.asm` and fault handlers.

**1.2 Disable Ring 3/4 access to kernel pages**
- When switching to Ring 3 (user mode), the kernel should update the CR3 to a
  page table that only has:
  - The process's own user pages
  - The process's user-mode stack
- **Task:** `process_create()` should set up a dedicated page table for each process
  that maps only the process's pages. On syscall entry, swap to kernel page table.
- **Reference:** `src/process/process.c` line 167 (`process_create()`), line 269
  (`process_fork()`).

**1.3 ASLR (Address Space Layout Randomization)**
- Process user stacks and program load addresses should be randomized.
- **Task:** Generate a random offset per process, apply to user stack base and
  program load address. Requires modifying `elf.c` and the process stack setup.
- **Note:** Not critical for MVP but improves defense-in-depth.

### Phase 2: Shared Memory Permission System

**2.1 Add permission flags to shared memory segments**
- `shm_create()` and `shm_attach()` should enforce access control.
- **Task:**
  - Add `uid`, `gid`, and permission bits (rwxrwxrwx) to `shm_header`.
  - `shm_create()`: set owner to current process uid/gid.
  - `shm_attach()`: check owner/group match, or permission bits allow attach.
  - `shm_delete()`: only owner can delete.

**2.2 Process name isolation**
- `process_get_by_name()` currently searches all process lists.
- Processes should not be able to access other processes by PID without
  appropriate permission (e.g., same uid, or root).
- **Task:** Add PID visibility checks to `process_get_by_pid()` in Ring 3 context.

### Phase 3: Kernel Stack Protection

**3.1 Guard pages for kernel stacks**
- Each process has a 4096-byte kernel stack in `process_struct`.
- A stack overflow could corrupt adjacent heap memory.
- **Task:** Add a guard page (unmapped page) after each kernel stack.
  Monitor for page fault on the guard page and report stack overflow.

### Phase 4: Syscall Isolation

**4.1 Syscall permission model**
- Currently any process can call any syscall (fork, exec, kill, etc.).
- **Task:** Implement a capability system or per-process syscall permissions.
- **Minimum:**
  - `SYS_KILL`: only same uid or root can signal other processes.
  - `SYS_EXEC`: only same uid or root can exec on behalf of others.
  - `SYS_FORK`: should be allowed by all (but check resource limits).

### Phase 5: ELF Loader Isolation

**5.1 Validate ELF segments before mapping**
- Currently `elf_load()` trusts the ELF file's segment addresses.
- A malicious ELF could specify user segments that overlap kernel memory.
- **Task:**
  - Check that all user segments map to addresses >= 0x1000.
  - Check that no user segment maps to kernel VMA range
    (>= `KERNEL_VMA_OFFSET`).
  - Check that segment physical addresses don't overlap kernel pages.

**5.2 Stack overflow detection**
- `sys_execve()` allocates 16KB user stack at `0x7FFFFFFF0000`.
- **Task:** Add a guard page below the user stack.

---

## Testing Strategy

- **Unit tests:** Add kernel-side tests for each isolation feature (access
  checks, permission enforcement, stack overflow detection).
- **E2E tests:** Test that processes cannot read each other's memory,
  cannot attach unauthorized shared memory, and cannot access kernel memory.
- **Fuzzing:** Generate random ELF files and syscalls to verify isolation
  holds under adversarial input.

## Priority

| Priority | Phase | Estimated Effort |
|----------|-------|------------------|
| P0 | 1.1 - Identity map removal | 1-2 days |
| P0 | 1.2 - Ring 3 page table isolation | 2-3 days |
| P1 | 2.1 - SHM permissions | 1 day |
| P1 | 4.1 - Syscall permissions | 1-2 days |
| P1 | 5.1 - ELF validation | 1 day |
| P2 | 3.1 - Kernel stack guard pages | 1 day |
| P2 | 5.2 - User stack guard page | 0.5 days |
| P2 | 1.3 - ASLR | 1-2 days |
| P3 | 2.2 - Process name isolation | 0.5 days |
| P3 | 4.1 - Full capability system | 3-5 days |

## Open Questions

1. **What is the security model?** Single-user (root only) or multi-user with
   privilege separation? Current kernel has `users.c` with `useradd`, `login`,
   etc., suggesting multi-user.
2. **Ring 3 vs Ring 4 distinction?** Currently `ring0_to_ring3()` jumps to Ring 3.
   Should we add a Ring 4 for user-space processes with reduced privileges?
3. **Should `fork()` preserve isolation?** The child inherits the parent's
   address space. On `exec()`, the old address space is destroyed. This is
   standard Unix behavior, but should we add `COW` (Copy-on-Write) for
   efficiency?
