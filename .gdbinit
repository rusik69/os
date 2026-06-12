# ── Hermes OS GDB Init ────────────────────────────────────────────────
# Auto-load safe: add "set auto-load safe-path /" to ~/.gdbinit
#
# Usage:
#   make debug          # starts QEMU waiting on :1234
#   gdb build/kernel.elf
#   (gdb) target remote :1234
#   (gdb) continue

set architecture i386:x86-64
set pagination off

# ── KASLR helper ──────────────────────────────────────────────────────
# KASLR slides the kernel from its link address. After connecting,
# read the kernel's actual load address from the Multiboot info or
# the kernel's own symbol `_start` in RAM.
python
import gdb

class KASLRInfo(gdb.Command):
    """Print the KASLR slide offset and adjust breakpoints."""
    def __init__(self):
        super().__init__("kaslr", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        try:
            linked = int(gdb.parse_and_eval("&_start"))
            # Read from QEMU's phys memory at the linked address
            actual = int(gdb.parse_and_eval("$rip"))
            slide = actual - linked if actual > linked else 0
            gdb.write(f"Linked _start: 0x{linked:016x}\n")
            gdb.write(f"Actual RIP:    0x{actual:016x}\n")
            if slide:
                gdb.write(f"KASLR slide:   0x{slide:016x} ({slide} bytes)\n")
                gdb.write("Breakpoints at linked addresses will miss; "
                          "use 'break *($rip + offset)' instead.\n")
            else:
                gdb.write("KASLR appears disabled or not yet relocated.\n")
        except Exception as e:
            gdb.write(f"Couldn't determine KASLR: {e}\n")

KASLRInfo()
end

# ── Process list pretty-printer ──────────────────────────────────────
python
class ProcessListPrinter:
    def __init__(self):
        self.enabled = False

    def show(self):
        try:
            max_proc = int(gdb.parse_and_eval("PROCESS_MAX"))
            table = gdb.parse_and_eval("process_table")
            gdb.write(f"{'PID':>5} {'STATE':>10} {'UID':>5} {'CMD':>20}\n")
            gdb.write("-" * 50 + "\n")
            for i in range(max_proc):
                p = table[i]
                state = int(p["state"])
                if state == 0:  # PROCESS_UNUSED
                    continue
                pid = int(p["pid"])
                uid = int(p["uid"])
                name = p["name"].string()
                states = ["UNUSED", "READY", "RUNNING", "BLOCKED", "ZOMBIE", "SLEEPING"]
                state_str = states[state] if state < len(states) else f"UNK({state})"
                gdb.write(f"{pid:>5} {state_str:>10} {uid:>5} {name:>20}\n")
        except Exception as e:
            gdb.write(f"Can't read process table: {e}\n")

_proc_printer = ProcessListPrinter()

class ShowProcesses(gdb.Command):
    """List all processes in the kernel process table."""
    def __init__(self):
        super().__init__("ps", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        _proc_printer.show()

ShowProcesses()
end

# ── Page table walk helper ────────────────────────────────────────────
python
class PageTableWalk(gdb.Command):
    """Walk the 4-level page table for a given virtual address.
    Usage: pt <virtual-addr> [cr3-value]"""
    def __init__(self):
        super().__init__("pt", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        args = arg.split()
        if not args:
            gdb.write("Usage: pt <virt-addr> [cr3-value]\n")
            return
        try:
            vaddr = int(args[0], 0)
            if len(args) > 1:
                cr3 = int(args[1], 0)
            else:
                cr3 = int(gdb.parse_and_eval("$cr3"))
            gdb.write(f"Walking VA 0x{vaddr:016x} with CR3=0x{cr3:016x}\n")
            # PML4 (bit 39-47): index = (vaddr >> 39) & 0x1ff
            # PDPT (bit 30-38): index = (vaddr >> 30) & 0x1ff
            # PD   (bit 21-29): index = (vaddr >> 21) & 0x1ff
            # PT   (bit 12-20): index = (vaddr >> 12) & 0x1ff
            pml4_idx = (vaddr >> 39) & 0x1ff
            pdpt_idx = (vaddr >> 30) & 0x1ff
            pd_idx   = (vaddr >> 21) & 0x1ff
            pt_idx   = (vaddr >> 12) & 0x1ff
            offset   = vaddr & 0xfff

            tables = []
            pml4_base = cr3 & ~0xfff
            pml4_entry = int(gdb.parse_and_eval(f"*(uint64_t*)({pml4_base} + {pml4_idx*8})"))
            tables.append(("PML4", pml4_idx, pml4_entry))
            if not pml4_entry & 1:
                gdb.write(f"  PML4[{pml4_idx}] = {pml4_entry:#016x} — NOT PRESENT\n")
                return
            gdb.write(f"  PML4[{pml4_idx}] = {pml4_entry:#016x} (present)\n")

            pdpt_base = pml4_entry & ~0xfff & ~0x1f  # clear flags
            pdpt_entry = int(gdb.parse_and_eval(f"*(uint64_t*)({pdpt_base} + {pdpt_idx*8})"))
            tables.append(("PDPT", pdpt_idx, pdpt_entry))
            if not pdpt_entry & 1:
                gdb.write(f"  PDPT[{pdpt_idx}] = {pdpt_entry:#016x} — NOT PRESENT\n")
                return
            gdb.write(f"  PDPT[{pdpt_idx}] = {pdpt_entry:#016x} (present)\n")
            if pdpt_entry & (1 << 7):  # huge page (1GB)
                phys = (pdpt_entry & 0xfffffc0000000) | (vaddr & 0x3fffffff)
                gdb.write(f"  → 1G huge page, phys = {phys:#016x}\n")
                return

            pd_base = pdpt_entry & ~0xfff & ~0x1f
            pd_entry = int(gdb.parse_and_eval(f"*(uint64_t*)({pd_base} + {pd_idx*8})"))
            tables.append(("PD", pd_idx, pd_entry))
            if not pd_entry & 1:
                gdb.write(f"  PD[{pd_idx}] = {pd_entry:#016x} — NOT PRESENT\n")
                return
            gdb.write(f"  PD[{pd_idx}] = {pd_entry:#016x} (present)\n")
            if pd_entry & (1 << 7):  # huge page (2MB)
                phys = (pd_entry & 0xfffffffffe00000) | (vaddr & 0x1fffff)
                gdb.write(f"  → 2M huge page, phys = {phys:#016x}\n")
                return

            pt_base = pd_entry & ~0xfff & ~0x1f
            pt_entry = int(gdb.parse_and_eval(f"*(uint64_t*)({pt_base} + {pt_idx*8})"))
            tables.append(("PT", pt_idx, pt_entry))
            if not pt_entry & 1:
                gdb.write(f"  PT[{pt_idx}] = {pt_entry:#016x} — NOT PRESENT\n")
                return
            phys = (pt_entry & 0xffffffffff000) | offset
            gdb.write(f"  PT[{pt_idx}] = {pt_entry:#016x} (present)\n")
            gdb.write(f"  → 4K page, phys = {phys:#016x}\n")
        except Exception as e:
            gdb.write(f"Error: {e}\n")

PageTableWalk()
end

# ── Useful one-liners ────────────────────────────────────────────────
define kstack
    p/x $rsp
    p/x (uint64_t*)((unsigned long)$rsp & ~0xfff)
end
document kstack
    Print current kernel stack top and page-aligned base.
end
