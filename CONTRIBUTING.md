# Contributing to Hermes OS

Thank you for your interest in contributing to Hermes OS! This document describes the development workflow, coding standards, and processes we follow.

## Table of Contents

- [Development Environment](#development-environment)
- [Build Process](#build-process)
- [Running](#running)
- [Coding Style](#coding-style)
- [Testing](#testing)
- [Pull Request Process](#pull-request-process)
- [Git Commit Conventions](#git-commit-conventions)
- [Continuous Integration](#continuous-integration)

---

## Development Environment

### Required Toolchain

| Tool                    | Purpose                      |
|-------------------------|------------------------------|
| `x86_64-elf-gcc`        | Cross-compiler (GCC 12+)     |
| `nasm`                  | x86-64 assembler             |
| `qemu-system-x86_64`    | Emulator for testing         |
| `make`                  | Build system                 |
| `python3`               | Test scripts and utilities   |
| `clang-format`          | Code formatting (optional)   |
| `cppcheck`              | Static analysis (optional)   |
| `clang-tidy`            | Linting (optional)           |
| `ccache`                | Build caching (optional)     |

### Quick Start

```bash
# Clone the repository
git clone https://github.com/rusik69/os.git
cd os

# Build the kernel and disk image
make -j$(nproc)

# Boot in QEMU
make run
```

---

## Build Process

### Standard Builds

| Command               | Description                                      |
|-----------------------|--------------------------------------------------|
| `make`                | Debug build (default, no optimisation)           |
| `make -j$(nproc)`     | Parallel build using all CPU cores               |
| `make build/arch/release/kernel.elf` | Release build (optimised)            |
| `make check`          | Strict build (`-Werror`) + tests + E2E smoke     |
| `make clean`          | Remove build artifacts                           |
| `make clean-all`      | Remove artifacts and clear ccache                |

### Build Optimisations

The build system auto-detects `ccache` and `distcc` when installed, speeding up incremental builds. Precompiled headers (PCH) provide ~2–3× compilation speedup and are enabled automatically.

### Cross-Compilation

The kernel is always cross-compiled with `x86_64-elf-gcc`. The Makefile sets the target triple; do not use the host system's `gcc` for kernel code.

---

## Running

```bash
# Standard QEMU (IDE disk + e1000 NIC)
make run

# SMP with 4 CPUs
make run-smp

# Debug with GDB stub
make run-gdb     # or: make debug

# UEFI boot
make run-uefi

# Headless serial console on TCP port 4444
make test-serial
```

After booting, the kernel presents a built-in shell with 356+ commands. Type `help` for a list.

---

## Coding Style

### Language Standard

The kernel is written in **C17** (ISO/IEC 9899:2018) with GNU extensions (`-std=gnu17`). Assembly is in NASM syntax.

### Kernel Conventions

- **Indentation**: 4 spaces (no tabs)
- **Line length**: 100 characters maximum
- **Braces**: K&R style — opening brace on the same line for functions, statements, and control flow
- **Naming**:
  - Functions: `snake_case`
  - Types: `snake_case_t`
  - Macros: `UPPER_SNAKE_CASE`
  - Global variables: `snake_case` with descriptive names
- **Headers**: Each subsystem has a corresponding header in `src/include/`
- **Comments**: Prefer `/* */` block comments; `//` line comments are acceptable for short annotations
- **Error handling**: Return negative `errno`-style codes from kernel functions
- **Memory allocation**: Use `kmalloc`/`kfree` (kernel heap), `pmm_alloc`/`pmm_free` (physical pages)
- **Locking**: Use spinlocks, mutexes, or RCU as appropriate; document lock ordering

### Automated Formatting

```bash
# Format all C sources
make format

# Check format compliance
make format-check
```

The `.clang-format` file at the repository root defines the canonical style. CI enforces formatting compliance on every PR.

### Static Analysis

```bash
# Run cppcheck + clang-tidy
make lint

# GCC static analysis (-fanalyzer)
make analyze
```

---

## Testing

The project has a multi-layered test strategy:

### In-Kernel Tests (KUnit)

The kernel includes a **KUnit** framework for in-kernel unit tests. Tests live in `src/test/` and cover:

- Physical Memory Manager (PMM)
- Slab allocator
- Scheduler (CFS/EEVDF)
- Virtual Memory Manager (VMM)
- Security subsystems
- Power management
- IPC primitives

To run KUnit tests at runtime from the shell:

```bash
echo 1 > /sys/kernel/debug/kunit/run_all
cat /sys/kernel/debug/kunit/results
```

### Automated Test Suite

```bash
# Full test suite (build test kernel + QEMU boot + KUnit)
make test

# Build test kernel only
make test-kernel

# Host-side libc unit tests
cd tests/host_libc && make && ./test_libc

# E2E QEMU smoke test
./tests/e2e.sh

# Strict build + all tests + E2E
make check

# Verify DOOM framebuffer rendering
make doom-test
```

The kernel has **200+ built-in tests** plus expandable KUnit suites. Tests run in QEMU and report PASS/FAIL via serial output.

### Test Mode

Building with `make test` compiles the kernel with `-DTEST_MODE`, enabling additional testing infrastructure and test harnesses. The test kernel binary is built separately in `build_test/`.

---

## Pull Request Process

1. **Fork and branch**: Create a feature branch from `main` or `master`
2. **Code**: Follow the coding style above
3. **Test**: Run `make check` locally — all tests must pass
4. **Format**: Run `make format-check` to verify style compliance
5. **Commit**: Use the [commit conventions](#git-commit-conventions) below
6. **PR**: Open a pull request against the `main` branch with a clear description
7. **CI**: A GitHub Actions runner executes the full pipeline automatically
8. **Review**: At least one maintainer review is required before merging
9. **Merge**: Squash-merge into `main` with a clean commit message

### PR Guidelines

- Keep changes focused — one logical change per PR
- Include test coverage for new features
- Update documentation (README, ARCHITECTURE, MODULARITY) when behaviour changes
- Do not introduce new compiler warnings
- All CI checks must pass before review

---

## Git Commit Conventions

We follow a structured commit message format inspired by [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <short summary>

<optional body — explain what and why, not how>

<optional footer — breaking changes, issue references>
```

### Types

| Type       | Usage                                     |
|------------|-------------------------------------------|
| `feat`     | New feature                               |
| `fix`      | Bug fix                                   |
| `perf`     | Performance improvement                   |
| `refactor` | Code restructuring without behaviour change |
| `docs`     | Documentation only                        |
| `test`     | Adding or fixing tests                    |
| `style`    | Formatting, whitespace, lint fixes        |
| `build`    | Build system, Makefile, CI                |
| `sec`      | Security fix or hardening                 |
| `chore`    | Maintenance, tooling, dependencies        |

### Scopes (examples)

`pmm`, `vmm`, `sched`, `vfs`, `tcp`, `ext2`, `nvme`, `acpi`, `drivers`, `shell`, `userspace`, `build`, `docs`, `test`, `container`, `cluster`, `kunit`, `security`, `kpti`, `ebpf`, `compiler`

### Examples

```
feat(pmm): add per-CPU page cache for lockless allocation

Implement a per-CPU hot cache for physical page allocation, mirroring
Linux's pcp (per-CPU pages) design. This reduces contention on the
main PMM bitmap lock under heavy allocation workloads.

Closes: #142
```

```
fix(tcp): correct window scale option negotiation

The TCP window scale option was being applied before the three-way
handshake completed, leading to incorrect window advertisements.

Fixes: #87
```

```
sec(kpti): enable kernel page table isolation by default

Mitigates Meltdown-style speculative execution attacks by isolating
kernel page tables from userspace.

Breaking: requires recompilation of all modules
```

---

## Continuous Integration

The project runs a **GitHub Actions** CI pipeline that executes on every push and pull request:

1. **Kernel build** — compiles the kernel and disk image
2. **Userspace build** — compiles userspace programs and initramfs
3. **Lint** — cppcheck + clang-tidy static analysis
4. **Unit tests** — host-side libc unit tests
5. **QEMU boot test** — boots the test kernel in QEMU and validates output
6. **KUnit runner** — parses serial log for KUnit results
7. **E2E smoke test** — boots to shell and runs a command sequence

### Self-Hosted Runner Requirement

The project uses **self-hosted GitHub Actions runners** for the full test pipeline. These runners provide the necessary hardware acceleration and device access for QEMU-based integration tests. All PRs from external forks require maintainer approval before they are executed on the self-hosted runner infrastructure.

If you are setting up a local development environment, the `make test` and `make check` targets run the same test suite without needing CI access.

---

## Getting Help

- **Issues**: Open a GitHub issue for bugs, feature requests, or questions
- **Discussions**: Use GitHub Discussions for design proposals and general questions
- **Maintainer**: Contact **@rusik69** on GitHub

---

*Thank you for contributing to Hermes OS!*
