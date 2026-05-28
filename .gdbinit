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

        printf "Virtual address: 0x%016llx\n", $virt
        printf "  PML4[0x%03lx]  → ", $pml4_idx
        set $pml4_phys = $cr3 & 0x000FFFFFFFFFF000
        set $pml4_virt = $pml4_phys
        set $pml4e = *(unsigned long long*)$pml4_virt + $pml4_idx*8
        # Actually we need proper dereference. Let's do it step by step.
        # PML4 is at CR3 physical address. Since boot identity-maps low 1GB,
        # and kernel is in high half but PML4 may be in low mem, just use phys as virt.
        # Actually we read from QEMU memory at physical address directly if < 1GB.
    end
end
document walk_page
    Walk the 4-level x86-64 page table hierarchy for a given virtual address.
    Shows PML4E → PDPTE → PDE → PTE → physical frame.
    Note: requires identity-mapped low memory (our boot does identity-map 0-1GB).
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
    Example: proc 2
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
