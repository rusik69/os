# Boot — Bootloader stage, entry point, GDT/IDT setup, mode transitions

Provides the initial entry point from the bootloader (Multiboot or UEFI), transitions from real mode → protected mode → long mode, and early hardware enumeration (ACPI, SMBIOS).

## Key Files

- **boot.asm** — Multiboot1-compliant entry point; sets up page tables (PML4/PDPT), enables PAE/long mode, installs GDT/IDT, and jumps to `kernel_main`.
- **multiboot.c** — Saves and parses the multiboot info structure; provides helpers for memory map, boot modules, and command-line access.
- **acpi.c** — Walks the ACPI RSDP/XSDT/RSDT tables at boot time to find tables by signature; validates checksums via direct physical memory access.
- **smbios.c** — Scans for the SMBIOS entry point and walks the structure table to retrieve BIOS/firmware information.
- **uefi_runtime.c** — Stubs for UEFI Runtime Services (GetTime, SetTime, GetVariable, SetVariable, ResetSystem).
- **uefi_gop.c** — Reads the UEFI GOP framebuffer info from the bootloader and registers it with the fbcon subsystem.
- **efi_menu.c** — UEFI boot menu with SimpleTextInput polling, configurable timeout, and boot entry selection.

## Architecture

Single-pass linear boot: the assembler stub (boot.asm) performs CPU initialisation (GDT/IDT, page tables, long-mode entry), then calls into C code for ACPI/SMBIOS enumeration. Later-stage C files handle Multiboot info parsing and UEFI runtime integration. The design favours simplicity and minimal dependency — no dynamic allocation or complex data structures in the early boot path.

## Cross-References

- **memory/ (pmm, vmm)** — Page tables set up here are handed off to the virtual memory manager.
- **drivers/fbcon** — GOP framebuffer is consumed by the framebuffer console.
- **power/** — ACPI table walking provides the foundation for later ACPI sleep state support.
