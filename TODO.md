# Process Isolation TODO

**Goal:** Processes should be fully isolated from each other and from the kernel.
Processes should not be able to read each other's memory or kernel memory, and should
only communicate through controlled IPC (pipes, shared memory with explicit permissions,
signals, and syscalls).

## Current State

- ✅ SMEP — kernel cannot execute userspace code (CR4.SMEP set, page table U/S on user pages)
- ✅ SMAP — kernel cannot access userspace data without explicit `stac`/`clac` (`AC` flag)
- ✅ NXE — No-Execute bit enforced on page table entries
- ✅ UMIP — User-mode MSR/memory access prevention (CR4.UMIP set)
- ✅ Kernel stack guard pages — unmapped page after each kernel stack detects overflows
- ✅ User stack guard pages — per-process unmapped page under ring 3 stacks
- ✅ NX pages for user mappings — user ELF segments get appropriate NX/X permissions
- ⬜ Still WIP: Full identity map removal and per-process kernel page table switching

## Completed Items

### Phase 1: Memory Isolation (Kernel Page Table Hardening)

**1.1a SMEP/SMAP/NXE/UMIP — CPU-level isolation primitives** ✅
- SMEP (CR4.SMEP) prevents `ret2usr` style attacks — Ring 0 cannot execute Ring 3 pages
- SMAP (CR4.SMAP) prevents accidental kernel dereference of user pointers — AC flag must be cleared via `stac`/`clac` to access user memory
- NXE (IA32_EFER.NXE) enables No-Execute bit — `.data`/`.bss`/`.rodata` segments in user ELFs are mapped NX; only `.text` pages are executable
- UMIP (CR4.UMIP) blocks `SGDT`/`SIDT`/`SLDT`/`SMSW`/`STR` from user mode — prevents user-level discovery of kernel addresses

**1.1b Kernel stack guard pages** ✅
- Each process kernel stack has an unmapped guard page below it
- Stack overflow (wrapping down) hits the guard page → page fault
- `page_fault_handler` detects guard page fault pattern and reports fatal overflow

**1.1c User stack guard pages** ✅
- Each userspace process gets a guard page below its stack
- Stack overflow detection for ring 3 processes
- Requires ELF loader and process setup to reserve guard page region

**1.1d NX enforcement for user ELF segments** ✅
- ELF loader maps `.text` as executable-only, `.rodata` as read-only, `.data`/`.bss` as read-write non-executable
- VMM page flags handle X/W/R as specified by ELF `p_flags`

## Remaining Work

### Phase 1: Memory Isolation (continued)

**1.2 Remove identity map from kernel page tables**
- Currently `identity_map_first_gb()` maps physical addresses 0-1GB at the same
  virtual address. This lets Ring 0 code access any physical memory directly.
- **Task:** Remove or limit the identity map. Kernel code should only access memory
  through the high-half VMA mapping (`KERNEL_VMA_OFFSET = 0xFFFF800000000000`).
- **Risk:** Interrupt handlers may rely on identity-mapped addresses for ISR stacks.
  Audit `idt_asm.asm` and fault handlers.

**1.3 Per-process kernel page table switching on syscall entry**
- Currently all Ring 0 code shares the boot-time identity-mapped page table.
- **Task:** On syscall entry from user mode, switch to a per-process kernel page table
  that only maps the kernel image (high half) + the process's own user pages, without
  the 1 GB identity map.

**1.4 ASLR (Address Space Layout Randomization)**
- Process user stacks and program load addresses should be randomized.
- **Task:** Generate a random offset per process, apply to user stack base and
  program load address. Requires modifying `elf.c` and the process stack setup.

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

### Phase 3: Syscall Isolation

**3.1 Syscall permission model**
- Currently any process can call any syscall (fork, exec, kill, etc.).
- **Task:** Implement a capability system or per-process syscall permissions.
- **Minimum:**
  - `SYS_KILL`: only same uid or root can signal other processes.
  - `SYS_EXEC`: only same uid or root can exec on behalf of others.
  - `SYS_FORK`: should be allowed by all (but check resource limits).

### Phase 4: ELF Loader Validation

**4.1 Strengthen ELF parsing validation**
- Currently the ELF loader trusts the binary's segment headers.
- **Task:**
  - Validate `p_vaddr` and `p_memsz` against reasonable limits
  - Reject overlapping segments
  - Validate `e_phoff`, `e_phentsize`, `e_phnum` sanity
  - Check that `p_offset` lies within the file
  - Sanity-check entry point against mapped segment range
  - Enforce that the binary is statically linked (no interpreter dependency without PT_INTERP support)
