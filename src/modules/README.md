# Modules — Loadable kernel module (.ko) loading, symbols, dependencies

Provides infrastructure for loading and unloading kernel modules (`.ko` files). Manages ELF relocations, symbol resolution, module dependency tracking, and entry-point invocation (init/cleanup).

## Key Files

- **test_module.c** — Minimal test module for build verification: logs a message on load and unload. Demonstrates `init_module`/`cleanup_module` entry points and `MODULE_LICENSE`/`MODULE_AUTHOR`/`MODULE_DESCRIPTION`/`MODULE_VERSION` metadata macros.

## Architecture

Modules are relocatable ELF64 objects parsed by the kernel's ELF loader. The module loader resolves symbols against the kernel's exported symbol table, applies relocations, maps the module into kernel space, and calls `init_module()`. Dependencies between modules are tracked to enforce correct load/unload ordering. Module metadata (license, author, description, version) is embedded via macros that produce ELF section annotations.

The design follows the Linux kernel module model: modules are built externally with `make modules`, loaded via `insmod`, and removed via `rmmod`. Export symbols with `EXPORT_SYMBOL` to make them available to other modules.

## Cross-References

- **compiler/** — The in-kernel C compiler can produce code that is linked into module-compatible ELF objects.
- **elf/** — Shares ELF parsing and relocation logic.
- **container/** — Container runtime can load security or observability modules.
