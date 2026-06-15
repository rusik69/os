#!/usr/bin/env python3
"""
src/test/boot_test.py — QEMU boot test for x86-64 hobby OS.

Launches QEMU with kernel.bin, captures serial output, and waits for
a shell prompt or "init: starting" message to confirm the kernel boots
successfully to userspace.

Usage:
    ./src/test/boot_test.py [--kernel path/to/kernel.bin] [--disk path/to/disk.img]
                            [--timeout 30] [--qemu qemu-system-x86_64]

Returns 0 on success, 1 on failure.
Uses Python 3 standard library only.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(
        description="QEMU boot test for x86-64 hobby OS"
    )
    parser.add_argument(
        "--kernel",
        default=os.path.join("build", "kernel.bin"),
        help="Path to kernel.bin (default: build/kernel.bin)",
    )
    parser.add_argument(
        "--disk",
        default=os.path.join("build", "disk.img"),
        help="Path to disk.img (default: build/disk.img)",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=30,
        help="Timeout in seconds (default: 30)",
    )
    parser.add_argument(
        "--qemu",
        default="qemu-system-x86_64",
        help="QEMU binary (default: qemu-system-x86_64)",
    )
    args = parser.parse_args()

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

    # Patterns to look for — boot success indicators
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

    print(f"[boot_test] Launching QEMU (timeout={args.timeout}s)...")
    print(f"[boot_test] Kernel: {args.kernel}")
    print(f"[boot_test] Disk:   {args.disk}")

    fd, serial_log = tempfile.mkstemp(prefix="boot_test_", suffix=".txt")
    os.close(fd)

    try:
        # Start QEMU with serial output to a file for pattern matching
        proc = subprocess.Popen(
            [
                args.qemu,
                "-kernel", args.kernel,
                "-m", "256M",
                "-serial", f"file:{serial_log}",
                "-vga", "none",
                "-display", "none",
                "-drive", f"file={args.disk},format=raw,if=ide",
                "-netdev", "user,id=net0",
                "-device", "e1000,netdev=net0",
                "-no-reboot",
                "-append", "console=serial quiet",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        # Poll for expected patterns
        start_time = time.time()
        boot_success = False
        boot_failure = False

        while time.time() - start_time < args.timeout:
            if proc.poll() is not None:
                # QEMU exited early — check what we have
                break

            time.sleep(0.5)

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
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

        if boot_success:
            print(f"[boot_test] Boot completed in {elapsed:.1f}s")
            return 0
        elif boot_failure:
            print(f"[boot_test] Boot FAILED after {elapsed:.1f}s")
            print("--- Last 30 lines of serial output ---")
            lines = output.splitlines()
            for line in lines[-30:]:
                print(line)
            return 1
        else:
            print(f"[boot_test] TIMEOUT after {elapsed:.1f}s — expected boot pattern not found")
            print("--- Last 30 lines of serial output ---")
            lines = output.splitlines()
            for line in lines[-30:]:
                print(line)
            return 1

    finally:
        if os.path.exists(serial_log):
            os.unlink(serial_log)


if __name__ == "__main__":
    sys.exit(main())
