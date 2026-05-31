# ─── OS Kernel GDB Init ─────────────────────────────────────────────
# Source this file automatically when GDB starts from the repo root.
# The kernel is linked at KERNEL_VMA_OFFSET = 0xFFFF800000000000.
#
# Usage:
#   cd /home/ubuntu/os
#   gdb build/kernel.elf
#   (gdb) connect
#
# ============================================================

set confirm off
set pagination off
set architecture i386:x86-64

# ─── KASLR offset awareness ───────────────────────────────────
# Kernel is always linked at 0xFFFF800000000000 (no KASLR in this kernel).
# If KASLR is ever added, adjust via the 'kaslr' command below.
set $KERNEL_VMA = 0xFFFF800000000000

# ─── Connect to QEMU GDB stub ─────────────────────────────────
define connect
    target remote :1234
    symbol-file build/kernel.elf
    printf "Connected to QEMU (port 1234). Kernel VMA base = 0x%llx\n", $KERNEL_VMA
end
document connect
    Connect to QEMU's GDB stub (started via 'make debug').
    Equivalent to: target remote :1234
end

# ─── Quick restart: reset QEMU and reconnect ──────────────────
define restart
    disconnect
    target remote :1234
    symbol-file build/kernel.elf
end
document restart
    Disconnect and reconnect to QEMU (useful after 'system_reset' in monitor).
end

# ─── Hex dump helper ──────────────────────────────────────────
define hexdump
    # Usage: hexdump ADDR [COUNT]
    # Dumps COUNT (default 64) bytes at ADDR in hex + ASCII.
    if $argc < 2
        set $count = 64
    else
        set $count = $arg1
    end
    if $count > 1024
        set $count = 1024
    end
    set $addr = $arg0
    set $i = 0
    while $i < $count
        # Check if we're at a 16-byte boundary (start of line)
        set $offset = $i % 16
        if $offset == 0
            printf "\n%016llx  ", $addr + $i
        end
        # Read byte
        set $b = *(unsigned char*)($addr + $i)
        printf "%02x ", $b
        set $i = $i + 1
        if $offset == 7
            printf " "
        end
    end
    # Print ASCII section for last line
    set $i = 0
    printf "\n                 "
    while $i < 16
        set $pos = ($count / 16) * 16 + $i
        if $pos < $count
            set $b = *(unsigned char*)($addr + $pos)
            if $b >= 32 && $b <= 126
                printf "%c", (char)$b
            else
                printf "."
            end
        end
        set $i = $i + 1
    end
    printf "\n"
end
document hexdump
    Dump memory in hex + ASCII format.
    Usage: hexdump ADDR [COUNT]
    ADDR: virtual address to dump from
    COUNT: number of bytes (default 64, max 1024)
    Examples:
      hexdump &idt       # dump IDT table
      hexdump 0xFFFF800000000000 128   # dump kernel entry point
end

# ─── Register pretty-printers ──────────────────────────────────
define regs
    printf "─── General Purpose Registers ───\n"
    printf "RAX: 0x%016llx    RBX: 0x%016llx\n", $rax, $rbx
    printf "RCX: 0x%016llx    RDX: 0x%016llx\n", $rcx, $rdx
    printf "RSI: 0x%016llx    RDI: 0x%016llx\n", $rsi, $rdi
    printf "RBP: 0x%016llx    RSP: 0x%016llx\n", $rbp, $rsp
    printf "R8:  0x%016llx    R9:  0x%016llx\n", $r8,  $r9
    printf "R10: 0x%016llx    R11: 0x%016llx\n", $r10, $r11
    printf "R12: 0x%016llx    R13: 0x%016llx\n", $r12, $r13
    printf "R14: 0x%016llx    R15: 0x%016llx\n", $r14, $r15
    printf "\n─── Control Registers ───\n"
    printf "CR0: 0x%016llx\n", $cr0
    printf "CR2: 0x%016llx  (fault address)\n", $cr2
    printf "CR3: 0x%016llx  (PML4 physical)\n", $cr3
    printf "CR4: 0x%016llx\n", $cr4
    printf "\n─── Segment Registers ───\n"
    printf "CS:  0x%04x        SS:  0x%04x\n", $cs, $ss
    printf "DS:  0x%04x        ES:  0x%04x\n", $ds, $es
    printf "FS:  0x%04x        GS:  0x%04x\n", $fs, $gs
end
document regs
    Pretty-print all CPU registers that matter for kernel debugging.
    Includes general-purpose, control (CR0-CR4), and segment registers.
    CR2 is especially important for page fault debugging.
end

# ─── Page table walker ─────────────────────────────────────────
define walk_page
    # Usage: walk_page VIRTUAL_ADDR
    # Walks the 4-level page table hierarchy for the given virtual address.
    if $argc < 1
        printf "Usage: walk_page VIRTUAL_ADDR\n"
    else
        set $virt = $arg0
        set $pml4_idx = ($virt >> 39) & 0x1FF
        set $pdpt_idx = ($virt >> 30) & 0x1FF
        set $pd_idx   = ($virt >> 21) & 0x1FF
        set $pt_idx   = ($virt >> 12) & 0x1FF
        set $offset   = $virt & 0xFFF

        printf "Virtual: 0x%016llx → PML4[0x%03lx] PDPT[0x%03lx] PD[0x%03lx] PT[0x%03lx] +0x%03lx\n", \
               $virt, $pml4_idx, $pdpt_idx, $pd_idx, $pt_idx, $offset

        # PML4 lives at CR3 (physical). Since kernel identity-maps the first 1GB,
        # PHYS_TO_VIRT(CR3) = CR3 + KERNEL_VMA_OFFSET — but CR3 may point above 1GB
        # if PML4 was allocated later by pmm_alloc_frame. Try PHYS_TO_VIRT first:
        set $KERNEL_VMA = 0xFFFF800000000000
        set $cr3_phys = $cr3 & 0x000FFFFFFFFFF000
        set $pml4_virt = $cr3_phys + $KERNEL_VMA

        # Read PML4 entry
        set $pml4e = *(unsigned long long*)$pml4_virt + $pml4_idx
        # Actually need to dereference the pointer. use x/gx instead.
        # GDB can do: set $pml4e = *(unsigned long long*)($pml4_virt + $pml4_idx*8)
        set $pml4e = *(unsigned long long*)($pml4_virt + $pml4_idx * 8)
        printf "PML4E[0x%03lx] = 0x%016llx", $pml4_idx, $pml4e
        if $pml4e & 1
            printf "  Present\n"
            set $pdpt_phys = $pml4e & 0x000FFFFFFFFFF000
            set $pdpt_virt = $pdpt_phys + $KERNEL_VMA
            set $pdpte = *(unsigned long long*)($pdpt_virt + $pdpt_idx * 8)
            printf "PDPTE[0x%03lx] = 0x%016llx", $pdpt_idx, $pdpte
            if $pdpte & 1
                printf "  Present"
                if $pdpte & 0x80
                    printf "  (1GB huge page!)\n"
                    set $phys = ($pdpte & 0x000FFFFFC0000000) | ($virt & 0x3FFFFFFF)
                    printf "  → Phys: 0x%016llx\n", $phys
                else
                    printf "\n"
                    set $pd_phys = $pdpte & 0x000FFFFFFFFFF000
                    set $pd_virt = $pd_phys + $KERNEL_VMA
                    set $pde = *(unsigned long long*)($pd_virt + $pd_idx * 8)
                    printf "  PDE[0x%03lx]  = 0x%016llx", $pd_idx, $pde
                    if $pde & 1
                        printf "  Present"
                        if $pde & 0x80
                            printf "  (2MB huge page!)\n"
                            set $phys = ($pde & 0x000FFFFFFFE00000) | ($virt & 0x1FFFFF)
                            printf "  → Phys: 0x%016llx\n", $phys
                        else
                            printf "\n"
                            set $pt_phys = $pde & 0x000FFFFFFFFFF000
                            set $pt_virt = $pt_phys + $KERNEL_VMA
                            set $pte = *(unsigned long long*)($pt_virt + $pt_idx * 8)
                            printf "    PTE[0x%03lx] = 0x%016llx", $pt_idx, $pte
                            if $pte & 1
                                printf "  Present\n"
                                set $phys = ($pte & 0x000FFFFFFFFFF000)
                                printf "    → Phys: 0x%016llx\n", $phys
                            else
                                printf "  Not Present\n"
                            end
                        end
                    else
                        printf "  Not Present\n"
                    end
                end
            else
                printf "  Not Present\n"
            end
        else
            printf "  Not Present\n"
        end
    end
end
document walk_page
    Walk the 4-level x86-64 page table hierarchy for a given virtual address.
    Shows PML4E → PDPTE → PDE → PTE → physical frame.
    Handles 2MB and 1GB huge pages.
    Usage: walk_page VIRTUAL_ADDR
    Example: walk_page 0xFFFF800001234000
end

# ─── Process info helper ───────────────────────────────────────
define proc
    if $argc < 1
        printf "Usage: proc PID or proc current\n"
    else
        if strcmp($arg0, "current") == 0
            set $pid = process_get_current()
            if $pid != 0
                printf "Current process:\n"
                print *((struct process*)$pid)
            else
                printf "No current process\n"
            end
        else
            set $pid = process_get_by_pid($arg0)
            if $pid != 0
                printf "Process PID=%d:\n", $arg0
                print *((struct process*)$pid)
            else
                printf "Process PID=%d not found\n", $arg0
            end
        end
    end
end
document proc
    Display process info.
    Usage: proc PID      — show process by PID
           proc current  — show current (running) process
    Shows kernel_stack, stack_top, guard_page, and other fields.
    Example: proc 2
end

# ─── List all processes (ps) ───────────────────────────────────
define ps
    set $tbl = process_get_table()
    if $tbl != 0
        printf "PID  STATE  NAME                         KERNEL_STACK         STACK_TOP            GUARD_PAGE           PML4\n"
        printf "───  ─────  ────                         ────────────         ──────────           ──────────           ────\n"
        set $i = 0
        while $i < 256
            set $p = $tbl + $i * (sizeof "struct process")
            # GDB's sizeof works differently — use pointer arithmetic on struct pointer
            # Actually, cast $tbl to (struct process *) and index it
            if 1
                set $proc = ((struct process *)$tbl)[$i]
                if $proc.state != 0  # PROCESS_UNUSED = 0
                    printf "%3d  ", $proc.pid
                    if $proc.state == 1
                        printf "READY  "
                    end
                    if $proc.state == 2
                        printf "RUNNING"
                    end
                    if $proc.state == 3
                        printf "BLOCKED"
                    end
                    if $proc.state == 4
                        printf "ZOMBIE "
                    end
                    printf "  %-28s 0x%016llx 0x%016llx 0x%016llx ", \
                           $proc.name, $proc.kernel_stack, $proc.stack_top, $proc.guard_page
                    if $proc.pml4 != 0
                        printf "%s\n", "user"
                    else
                        printf "%s\n", "kernel"
                    end
                end
            end
            set $i = $i + 1
        end
    else
        printf "Process table not available\n"
    end
end
document ps
    List all processes with PID, state, name, kernel stack range, guard page, and page table type.
    Shows kernel threads (pml4=NULL) and user processes (pml4=user).
end

# ─── Object dump ───────────────────────────────────────────────
define dump_idt
    # Dump the first 32 IDT entries (exception vectors)
    printf "─── IDT Exception Vectors (0-31) ───\n"
    set $i = 0
    while $i < 32
        set $entry = (struct idt_entry *)&idt
        set $off_low  = $entry[$i].offset_low
        set $off_mid  = $entry[$i].offset_mid
        set $off_high = $entry[$i].offset_high
        set $handler  = $off_high
        set $handler  = $handler << 16
        set $handler  = $handler | $off_mid
        set $handler  = $handler << 16
        set $handler  = $handler | $off_low
        printf "#%02d: sel=0x%04x type=0x%02x handler=0x%016llx\n", \
               $i, $entry[$i].selector, $entry[$i].type_attr, $handler
        set $i = $i + 1
    end
end
document dump_idt
    Dump the first 32 IDT entries (CPU exception vectors 0-31).
    Shows selector, type/attributes, and handler address for each.
end

# ─── Convenience aliases ───────────────────────────────────────
alias c = continue
alias s = stepi
alias n = nexti
alias f = finish

# ─── On connect: print banner ──────────────────────────────────
printf "\n"
printf "╔══════════════════════════════════════════════════════════╗\n"
printf "║   OS Kernel Debugger                                    ║\n"
printf "║   Type 'connect' to attach to QEMU (port 1234)          ║\n"
printf "║   Type 'help' for GDB help                              ║\n"
printf "║   Type 'regs' for register dump                         ║\n"
printf "║   Type 'hexdump ADDR [COUNT]' for memory dump           ║\n"
printf "╚══════════════════════════════════════════════════════════╝\n"
printf "\n"
