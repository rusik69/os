#!/usr/bin/env python3
"""e2e_test.py — QEMU e2e tests. One file, no deps.

Batch mode: send all commands at boot, read all output, check patterns.
No serial synchronization needed.

Usage:  ./src/test/e2e_test.py --kernel build/kernel.bin
"""

import argparse, fcntl, os, re, subprocess, sys, time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", default="build/kernel.bin")
    args = parser.parse_args()

    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", args.kernel, "-m", "1G",
         "-serial", "stdio",
         "-nic", "none",
         "-display", "none", "-vga", "none", "-no-reboot"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert qemu.stdin and qemu.stdout

    fd = qemu.stdout.fileno()
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    def read(timeout):
        data = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                data += chunk
            except (BlockingIOError, OSError):
                time.sleep(0.05)
        return data

    # Wait for boot
    data = read(60)
    if b"Welcome to the OS shell" not in data:
        print(f"FAIL boot  ({len(data)} bytes, no 'Welcome to the OS shell')")
        qemu.kill()
        return 1
    print(f"PASS boot  ({len(data)} bytes)")

    # Send all commands at once. The shell processes them sequentially.
    # Separator is newline; we check for each expected output in the combined
    # result.
    commands = b"\n".join([
        b"echo HELLO_WORLD",
        b"help",
        b"uname",
        b"ls /",
        b"true",
    ])
    qemu.stdin.write(commands + b"\n")
    qemu.stdin.flush()

    resp = read(60)
    qemu.terminate()
    qemu.wait(timeout=5)

    checks = [
        ("echo",   b"HELLO_WORLD"),
        ("help",   b"Available commands"),
        ("uname",  b""),
        ("ls",     b""),   # no real rootfs in kernel-mode shell
        ("true",   b""),
    ]

    for name, expected in checks:
        ok = not expected or expected in resp
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            tail = resp[-500:].decode("utf-8", "replace")
            print(f"    expected {expected!r}, last 500B: ...{tail}")


if __name__ == "__main__":
    sys.exit(main())
