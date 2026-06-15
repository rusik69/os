---
name: Bug Report
about: Report a bug to help us improve Hermes OS
title: '[BUG] '
labels: bug
assignees: rusik69
---

## Describe the Bug
A clear and concise description of what the bug is.

## To Reproduce
Steps to reproduce the behavior:
1. Boot with '...'
2. Run command '...'
3. Trigger condition '...'
4. See error

## Expected Behavior
A clear and concise description of what you expected to happen.

## Screenshots / Logs
If applicable, add serial console output, QEMU logs, or screenshots to help explain the problem.

## Environment

- **Host OS:** (e.g., Ubuntu 22.04, macOS 14)
- **QEMU version:** (e.g., 8.2.0)
- **Build toolchain:** (e.g., x86_64-elf-gcc 13.2.0, NASM 2.16)
- **Commit/Branch:** (e.g., `main` @ `abc1234`)

**Provision (check all that apply):**
- [ ] QEMU (`make run`)
- [ ] Real hardware (please describe)
- [ ] UEFI boot
- [ ] Legacy BIOS boot

## Kernel Log
If the kernel produced any output before the issue, please include it here:
```
(paste serial output here)
```

## Additional Context
Add any other context about the problem here. For example:
- Does the issue reproduce on a different build configuration?
- Is it related to a specific driver or subsystem?
- Any recent commits that might have introduced the issue?
