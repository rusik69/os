#!/usr/bin/env python3
"""
src/test/boot_test.py — QEMU boot test for x86-64 hobby OS.

Launches QEMU with kernel.bin, captures serial output, and waits for
a shell prompt or boot-complete message to confirm the kernel boots
successfully to userspace.

Supports isa-debug-exit for clean PASS/FAIL exit codes.

Usage:
    ./src/test/boot_test.py [--kernel path/to/kernel.bin] [--disk path/to/disk.img]
                            [--timeout 30] [--qemu qemu-system-x86_64] [--mem 256M]

Environment variable overrides (take precedence over defaults, CLI args win):
    KERNEL  - path to kernel.bin
    DISK    - path to disk.img
    TIMEOUT - boot timeout in seconds
    QEMU    - QEMU binary path
    MEM     - QEMU memory size (e.g., 256M, 512M)
    QEMU_EXTRA_ARGS - additional QEMU arguments
    DEBUG   - set to 1 for verbose output

Returns 0 on success, 1 on failure, 124 on timeout.
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import time


def main():
    # Parse CLI args, with env var fallbacks
    parser = argparse.ArgumentParser(
        description="QEMU boot test for x86-64 hobby OS"
    )
    parser.add_argument(
        "--kernel",
        default=os.environ.get("KERNEL", os.path.join("build", "kernel.bin")),
        help="Path to kernel.bin (default: build/kernel.bin)",
    )
    parser.add_argument(
        "--disk",
        default=os.environ.get("DISK", os.path.join("build", "disk.img")),
        help="Path to disk.img (default: build/disk.img)",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=int(os.environ.get("TIMEOUT", "30")),
        help="Timeout in seconds (default: 30)",
    )
    parser.add_argument(
        "--qemu",
        default=os.environ.get("QEMU", "qemu-system-x86_64"),
        help="QEMU binary (default: qemu-system-x86_64)",
    )
    parser.add_argument(
        "--mem",
        default=os.environ.get("MEM", "256M"),
        help="QEMU memory size (default: 256M)",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        default=os.environ.get("DEBUG", "0") == "1",
        help="Verbose output",
    )
    args = parser.parse_args()

    debug = args.verbose

    # Verify prerequisites
    for path, label in [(args.kernel, "kernel"), (args.disk, "disk image")]:
        if not os.path.isfile(path):
            print(f"ERROR: {label} not found at {path}")
            return 1

    try:
        subprocess.run([args.qemu, "--version"], capture_output=True, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print(f"ERROR: {args.qemu} not found or not executable")
        return 1

    # Boot success indicators — checked in order
    success_patterns = [
        r"init: starting",
        r"shell\s*#",
        r"#\s*$",
        r"\$\s*$",
        r"init complete",
        r"boot complete",
        r"Services started",
        r"\[OK\] Processes created",
        r"Welcome",
        r"Hermes OS",
    ]

    # Patterns that indicate boot failure
    failure_patterns = [
        r"^=== KERNEL PANIC ===",
        r"^Kernel Panic",
        r"^BUG:",
        r"^Fatal",
        r"triple fault",
        r"ERROR.*init",
        r"^=== SYSTEM HALTED ===",
    ]

    print(f"[boot_test] Launching QEMU (timeout={args.timeout}s, mem={args.mem})...")
    print(f"[boot_test] Kernel: {args.kernel}")
    print(f"[boot_test] Disk:   {args.disk}")
    if debug:
        print(f"[boot_test] QEMU:   {args.qemu}")

    fd, serial_log = tempfile.mkstemp(prefix="boot_test_", suffix=".txt")
    os.close(fd)

    # Build QEMU command
    qemu_cmd = [
        args.qemu,
        "-kernel", args.kernel,
        "-m", args.mem,
        "-serial", f"file:{serial_log}",
        "-vga", "none",
        "-display", "none",
        "-drive", f"file={args.disk},format=raw,if=ide",
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0",
        "-no-reboot",
        "-no-shutdown",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-append", "console=serial quiet",
    ]

    # Append extra QEMU args from environment
    extra = os.environ.get("QEMU_EXTRA_ARGS", "")
    if extra:
        qemu_cmd.extend(extra.split())

    try:
        if debug:
            print(f"[boot_test] QEMU command: {' '.join(qemu_cmd)}")

        proc = subprocess.Popen(
            qemu_cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        # Poll for expected patterns
        start_time = time.time()
        boot_success = False
        boot_failure = False

        while time.time() - start_time < args.timeout:
            ret = proc.poll()
            if ret is not None:
                # QEMU exited — could be isa-debug-exit or crash
                if ret == 33:  # isa-debug-exit: PASS
                    print(f"[boot_test] PASS: Kernel booted successfully (isa-debug-exit code 33)")
                    return 0
                elif ret == 16:  # isa-debug-exit: FAIL
                    print(f"[boot_test] FAIL: Test failure (isa-debug-exit code 16)")
                    boot_failure = True
                    break
                elif debug:
                    print(f"[boot_test] QEMU exited with code {ret}")

            time.sleep(0.25)

            try:
                with open(serial_log, "r", errors="replace") as f:
                    output = f.read()
            except (IOError, OSError):
                continue

            # Check for failure patterns first
            for pat in failure_patterns:
                if re.search(pat, output, re.IGNORECASE):
                    print(f"[boot_test] FAIL: Boot failure detected (matched '{pat}')")
                    boot_failure = True
                    break

            if boot_failure:
                break

            # Check for success patterns
            for pat in success_patterns:
                if re.search(pat, output, re.IGNORECASE):
                    print(f"[boot_test] PASS: Kernel booted successfully (matched '{pat}')")
                    boot_success = True
                    break

            if boot_success:
                break

        elapsed = time.time() - start_time

        # Read final output
        try:
            with open(serial_log, "r", errors="replace") as f:
                output = f.read()
        except (IOError, OSError):
            output = ""

        # Terminate QEMU
        _terminate_qemu(proc)

        if boot_success:
            print(f"[boot_test] Boot completed in {elapsed:.1f}s")
            return 0
        elif boot_failure:
            print(f"[boot_test] Boot FAILED after {elapsed:.1f}s")
            _print_last_lines(output, 30)
            return 1
        else:
            print(f"[boot_test] TIMEOUT after {elapsed:.1f}s — expected boot pattern not found")
            _print_last_lines(output, 30)
            return 124

    finally:
        if os.path.exists(serial_log):
            os.unlink(serial_log)


def _terminate_qemu(proc):
    """Gracefully terminate QEMU, escalating to kill if needed."""
    try:
        proc.terminate()
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2)
    except Exception:
        proc.kill()


def _print_last_lines(output, n):
    """Print the last n lines of QEMU serial output."""
    print(f"--- Last {n} lines of serial output ---")
    lines = output.splitlines()
    for line in lines[-n:]:
        print(line)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[boot_test] Interrupted")
        sys.exit(130)
