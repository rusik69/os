#!/usr/bin/env python3
"""
Generate a C source file with a kallsyms table for the kernel.

Usage: gen_kallsyms.py <kernel.elf> <output.c>

Extracts all GLOBAL defined symbols (T/t/D/d/B/b/R/r) from the kernel ELF
and emits them as struct ksym_entry entries in the .kallsyms section so the
module loader's find_ksym_all() fallback can resolve ANY kernel symbol.

The table is emitted sorted by name (the boot-time kallsyms_sort re-sorts
with qsort, which is cheap either way).

Address stability: the .kallsyms section is placed BEFORE .bss in the
linker script, i.e. after all code/data it references, so the symbol
addresses taken from the pass-1 link are identical in the final link
(only .bss and later move).
"""

import struct
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <kernel.elf> <output.c>")
        return 1

    elf = sys.argv[1]
    out = sys.argv[2]

    syms = subprocess.check_output(["nm", "-n", elf]).decode().splitlines()

    entries = []
    for line in syms:
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        addr, typ, name = parts[0], parts[1], parts[2]
        if typ not in "TtDdBbRr":
            continue
        # Skip our own generated table marker / GOT placeholder
        if name == "_GLOBAL_OFFSET_TABLE_" or name.startswith("__kallsym"):
            continue
        try:
            int(addr, 16)
        except ValueError:
            continue
        entries.append((addr, name))

    # Sort by name for binary search
    entries.sort(key=lambda e: e[1])

    with open(out, "w") as f:
        f.write('/* Auto-generated kallsyms table — do not edit. */\n')
        f.write('#include "export.h"\n')
        f.write('static const struct ksym_entry __attribute__((section(".kallsyms"), used, aligned(16))) kallsyms_gen[] = {\n')
        for addr, name in entries:
            # Escape quotes/backslashes in symbol names
            esc = name.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    {{ 0x{addr}ULL, "{esc}", 0, {{0, 0, 0}} }},\n')
        f.write('};\n')

    print(f"[kallsyms] generated {len(entries)} entries -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
