#!/usr/bin/env python3
"""
tests/e2e.py — E2E tests for the OS kernel shell via telnet.

Connects to the kernel's telnet daemon running in QEMU and exercises
every shell command.  Run via: make e2e   (or tests/e2e.sh directly).

Environment variables:
  E2E_HOST         telnet host  (default 127.0.0.1)
  E2E_PORT         telnet port  (default 2323)
  E2E_TIMEOUT      per-command timeout in seconds (default 30)
  E2E_BOOT_TIMEOUT initial banner timeout in seconds (default 90)
"""

import os
import re
import socket
import sys
import time

# ── Configuration ──────────────────────────────────────────────────────────────

HOST         = os.environ.get("E2E_HOST", "127.0.0.1")
PORT         = int(os.environ.get("E2E_PORT", "2323"))
TIMEOUT      = int(os.environ.get("E2E_TIMEOUT", "30"))
BOOT_TIMEOUT = int(os.environ.get("E2E_BOOT_TIMEOUT", "90"))

PROMPT = b"os> "

# ── Telnet connection helper ───────────────────────────────────────────────────

class Telnet:
    """Minimal telnet client that strips IAC protocol bytes."""

    def __init__(self, host: str, port: int):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((host, port))
        self.sock.settimeout(TIMEOUT)
        self._buf = b""
        # Brief pause before first read — gives the kernel's telnet on_connect
        # handler time to fire and send the banner after the TCP handshake.
        time.sleep(0.5)

    # Remove IAC command sequences (3 bytes: 0xFF cmd opt)
    @staticmethod
    def _strip_iac(data: bytes) -> bytes:
        out = bytearray()
        i = 0
        while i < len(data):
            b = data[i]
            if b == 0xFF and i + 2 < len(data):   # IAC
                i += 3
            else:
                out.append(b)
                i += 1
        return bytes(out)

    def read_until(self, marker: bytes, timeout: float = None) -> bytes:
        """Read bytes until *marker* appears; return everything received."""
        t_max = timeout or TIMEOUT
        deadline = time.monotonic() + t_max
        while marker not in self._buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            self.sock.settimeout(min(remaining, 2.0))
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                self._buf += self._strip_iac(chunk)
            except socket.timeout:
                pass   # keep looping until deadline
        # Return everything up to and including the marker;
        # discard the tail to avoid consuming stale coalesced data.
        idx = self._buf.find(marker)
        if idx >= 0:
            end = idx + len(marker)
            result = self._buf[:end]
            self._buf = b""
        else:
            result, self._buf = self._buf, b""
        return result

    def drain(self, t: float = 0.2) -> None:
        """Discard any stale data in the socket buffer and self._buf."""
        self._buf = b""
        self.sock.settimeout(t)
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
        except socket.timeout:
            pass
        self.sock.settimeout(TIMEOUT)

    def send_cmd(self, cmd: str, timeout: float = None) -> str:
        """Send command, wait for next prompt, return output as string."""
        # Brief pause so kernel fully processes the previous response
        # before we send the next command.
        time.sleep(0.05)
        self.sock.send((cmd + "\n").encode("latin-1"))
        raw = self.read_until(PROMPT, timeout=timeout)
        if os.environ.get("E2E_DEBUG"):
            print(f"DEBUG cmd={cmd!r:30} raw_tail={raw[-60:]!r}", flush=True)
        # Decode and strip the echoed command + trailing prompt
        text = raw.decode("latin-1", errors="replace")
        # Remove ANSI codes if any
        text = re.sub(r"\x1b\[[0-9;]*[mGKH]", "", text)
        # Strip echoed command line from the front
        lines = text.splitlines()
        if lines and cmd in lines[0]:
            lines = lines[1:]
        # Strip trailing "os> "
        result = "\n".join(lines).replace("os> ", "").strip()
        return result

    def close(self):
        try:
            self.sock.send(b"exit\n")
        except Exception:
            pass
        self.sock.close()


# ── Test framework ─────────────────────────────────────────────────────────────

_pass = 0
_fail = 0


def ok(name: str) -> None:
    global _pass
    print(f"[PASS] {name}", flush=True)
    _pass += 1


def fail(name: str, reason: str) -> None:
    global _fail
    print(f"[FAIL] {name} — {reason}", flush=True)
    _fail += 1


def check(name: str, output: str, *must_contain: str) -> bool:
    """Assert every string in must_contain appears in output."""
    for s in must_contain:
        if s not in output:
            fail(name, f"expected {repr(s)!r}, got: {repr(output[:200])}")
            return False
    ok(name)
    return True


def check_absent(name: str, output: str, must_not: str) -> bool:
    """Assert must_not does NOT appear in output."""
    if must_not in output:
        fail(name, f"unexpected {repr(must_not)} in: {repr(output[:200])}")
        return False
    ok(name)
    return True


# ── Individual command tests ───────────────────────────────────────────────────

def test_help(t: Telnet):
    r = t.send_cmd("help")
    check("help — lists commands", r,
          "Available commands:", "echo", "meminfo", "ps",
          "uptime", "ls", "cat", "write", "ifconfig")


def test_echo(t: Telnet):
    r = t.send_cmd("echo hello e2e world")
    check("echo", r, "hello e2e world")

    r = t.send_cmd("echo")
    check_absent("echo empty — no error", r, "Unknown")


def test_meminfo(t: Telnet):
    r = t.send_cmd("meminfo")
    check("meminfo — physical memory", r, "Physical memory:", "Total:", "Used:", "Free:")


def test_ps(t: Telnet):
    r = t.send_cmd("ps")
    check("ps — process table", r, "PID", "STATE", "NAME")
    # 'shell' or 'idle' process must be visible
    check("ps — shows known process", r, "idle")


def test_uptime(t: Telnet):
    r = t.send_cmd("uptime")
    check("uptime", r, "Uptime:")
    # ticks must be a digit
    if not any(c.isdigit() for c in r):
        fail("uptime — tick count", "no digits in output")


def test_date(t: Telnet):
    r = t.send_cmd("date")
    check("date — RTC year 202x", r, "202")
    # expect format YYYY-MM-DD HH:MM:SS
    if not re.search(r"\d{4}-\d{2}-\d{2}", r):
        fail("date — format YYYY-MM-DD", f"got: {r!r}")
    else:
        ok("date — date format")


def test_cpuinfo(t: Telnet):
    r = t.send_cmd("cpuinfo")
    check("cpuinfo — vendor", r, "CPU Vendor:")
    check("cpuinfo — brand", r, "CPU Brand:")


def test_history(t: Telnet):
    # Send an echo first so it's definitely in history
    t.send_cmd("echo history-prime")
    r = t.send_cmd("history")
    # history must contain commands sent earlier in this session
    check("history — shows prior commands", r, "echo")


def test_color(t: Telnet):
    r = t.send_cmd("color 7 0")
    check("color 7 0", r, "Color set to")

    r = t.send_cmd("color 15")
    check("color single arg", r, "Color set to")

    r = t.send_cmd("color")
    check("color no args — usage", r, "Usage:")


def test_hexdump(t: Telnet):
    r = t.send_cmd("hexdump 0x100000 32")
    check("hexdump — hex lines", r, "100000")

    r = t.send_cmd("hexdump")
    check("hexdump no args — usage", r, "Usage:")


def test_mouse(t: Telnet):
    r = t.send_cmd("mouse")
    check("mouse — status", r, "Mouse:", "x=", "y=", "buttons=")


def test_ifconfig(t: Telnet):
    r = t.send_cmd("ifconfig")
    check("ifconfig — eth0", r, "eth0:", "MAC:", "IP:", "Mask:")


def test_beep(t: Telnet):
    r = t.send_cmd("beep 500 5")
    check_absent("beep — no error output", r, "Usage:")
    check_absent("beep — no unknown cmd", r, "Unknown")
    # beep busy-waits briefly; allow kernel to settle
    t.drain(t=0.2)


def test_play(t: Telnet):
    r = t.send_cmd("play C4 E4 G4")
    check_absent("play — no error", r, "Usage:")

    r = t.send_cmd("play")
    check("play no args — usage", r, "Usage:")
    # play busy-waits for ~600ms; kernel may have processed a buffered command
    # during that time.  Drain any extra responses before the next test group.
    t.drain(t=0.3)


def test_udpsend(t: Telnet):
    r = t.send_cmd("udpsend 10.0.2.2 9999 e2etest")
    check("udpsend", r, "UDP sent")

    r = t.send_cmd("udpsend")
    check("udpsend no args — usage", r, "Usage:")
    # Drain any stale echo bytes before the next test group
    t.drain(t=0.2)


def test_ping(t: Telnet):
    # ping blocks up to ~30s in the kernel; use 45s timeout so we get the prompt
    r = t.send_cmd("ping 10.0.2.2", timeout=45)
    t.drain()   # flush any stale bytes before next command
    check("ping gateway", r, "PING")
    # Accept either a reply or a timeout; the IP must appear
    check("ping — shows IP", r, "10.0.2.2")
    # ping by hostname (DNS resolve + ICMP)
    r = t.send_cmd("ping ya.ru", timeout=45)
    t.drain()
    check("ping hostname — PING", r, "PING")


def test_dns(t: Telnet):
    r = t.send_cmd("dns")
    check("dns no args — usage", r, "Usage:")

    # DNS may or may not resolve in QEMU user mode — just check it runs
    # dns blocks up to ~30s; use 45s timeout and drain after
    r = t.send_cmd("dns google.com", timeout=45)
    t.drain()
    check("dns hostname — runs", r, "Resolving")


def test_kill(t: Telnet):
    # Kill PID 0 is rejected
    r = t.send_cmd("kill 0")
    check("kill pid 0 — rejected", r, "Cannot kill pid 0")

    # Kill non-existent PID
    r = t.send_cmd("kill 9999")
    check("kill nonexistent pid", r, "No such process")

    r = t.send_cmd("kill")
    check("kill no args — usage", r, "Usage:")


def test_filesystem(t: Telnet):
    # Format for a clean slate
    r = t.send_cmd("format")
    check("format — clean filesystem", r, "Filesystem formatted")

    # ls on empty fs
    r = t.send_cmd("ls")
    check_absent("ls empty fs — no error msg", r, "Cannot")
    check_absent("ls empty fs — no crash", r, "Fault")

    # mkdir
    r = t.send_cmd("mkdir e2edir")
    check_absent("mkdir — no error", r, "Cannot create")

    # stat directory
    r = t.send_cmd("stat e2edir")
    check("stat dir — type", r, "directory")

    # touch file
    r = t.send_cmd("touch e2efile")
    check_absent("touch — no error", r, "Cannot create")

    # write content
    r = t.send_cmd("write e2efile content-e2e-test-data")
    check("write file — bytes written", r, "Written")

    # cat the file
    r = t.send_cmd("cat e2efile")
    check("cat file — content", r, "content-e2e-test-data")

    # stat the file
    r = t.send_cmd("stat e2efile")
    check("stat file — type", r, "file")
    check("stat file — size", r, "Size:")

    # ls should list both
    r = t.send_cmd("ls")
    check("ls — lists file", r, "e2efile")

    # second write (overwrite)
    r = t.send_cmd("write e2efile overwritten-content")
    check("write overwrite — bytes written", r, "Written")
    r = t.send_cmd("cat e2efile")
    check("cat overwritten file", r, "overwritten-content")

    # rm file
    r = t.send_cmd("rm e2efile")
    check_absent("rm file — no error", r, "Cannot")

    # stat removed file should fail
    r = t.send_cmd("stat e2efile")
    check("stat after rm — not found", r, "Not found")

    # rm directory
    r = t.send_cmd("rm e2edir")
    check_absent("rm dir — no error", r, "Cannot")

    # Multiple files
    for i in range(4):
        fname = f"mf{i}"
        r = t.send_cmd(f"touch {fname}")
        check_absent(f"multi touch {fname}", r, "Cannot")
        r = t.send_cmd(f"write {fname} data{i}")
        check(f"multi write {fname}", r, "Written")
        r = t.send_cmd(f"cat {fname}")
        check(f"multi cat {fname}", r, f"data{i}")

    # rm all multi files
    for i in range(4):
        t.send_cmd(f"rm mf{i}")

    ok("filesystem — multi file create/write/read/delete")


def test_run_script(t: Telnet):
    """Write a script to disk and execute it with 'run'."""
    # Write a script that echos a sentinel
    r = t.send_cmd("write myscript echo script-output-e2e")
    check("write script file", r, "Written")

    r = t.send_cmd("run myscript")
    check("run script — executes echo", r, "script-output-e2e")

    t.send_cmd("rm myscript")
    ok("run script — cleanup")


def test_error_cases(t: Telnet):
    """Unknown command should produce an error message."""
    r = t.send_cmd("thisdoesnotexist")
    check("unknown command — error", r, "Unknown command")

    r = t.send_cmd("cat")
    check("cat no args — usage", r, "Usage:")

    r = t.send_cmd("write")
    check("write no args — usage", r, "Usage:")

    r = t.send_cmd("touch")
    check("touch no args — usage", r, "Usage:")

    r = t.send_cmd("rm")
    check("rm no args — usage", r, "Usage:")

    r = t.send_cmd("mkdir")
    check("mkdir no args — usage", r, "Usage:")

    r = t.send_cmd("stat")
    check("stat no args — usage", r, "Usage:")

    r = t.send_cmd("hexdump")
    check("hexdump no args — usage", r, "Usage:")

    r = t.send_cmd("exec")
    check("exec no args — usage", r, "Usage:")

    r = t.send_cmd("run")
    check("run no args — usage", r, "Usage:")


def test_wc(t: Telnet):
    """wc: count lines, words, bytes."""
    t.send_cmd("write wcfile hello world")
    r = t.send_cmd("wc wcfile")
    # "hello world" = 0 lines (no newline), 2 words, 11 bytes
    check("wc — word count", r, "wcfile")
    t.send_cmd("rm wcfile")

    r = t.send_cmd("wc")
    check("wc no args — usage", r, "Usage:")


def test_head(t: Telnet):
    """head: first N lines of a file."""
    # Create a multi-line file using a script
    t.send_cmd("write headfile line1")
    r = t.send_cmd("head headfile")
    check("head — shows content", r, "line1")

    r = t.send_cmd("head")
    check("head no args — usage", r, "Usage:")
    t.send_cmd("rm headfile")


def test_tail(t: Telnet):
    """tail: last N lines of a file."""
    t.send_cmd("write tailfile lastline")
    r = t.send_cmd("tail tailfile")
    check("tail — shows content", r, "lastline")

    r = t.send_cmd("tail")
    check("tail no args — usage", r, "Usage:")
    t.send_cmd("rm tailfile")


def test_cp(t: Telnet):
    """cp: copy files."""
    t.send_cmd("write cpsrc copydata")
    r = t.send_cmd("cp cpsrc cpdst")
    check("cp — success", r, "Copied")
    r = t.send_cmd("cat cpdst")
    check("cp — dst content", r, "copydata")
    t.send_cmd("rm cpsrc")
    t.send_cmd("rm cpdst")

    r = t.send_cmd("cp")
    check("cp no args — usage", r, "Usage:")


def test_mv(t: Telnet):
    """mv: move/rename files."""
    t.send_cmd("write mvsrc movedata")
    r = t.send_cmd("mv mvsrc mvdst")
    check("mv — success", r, "Moved")
    r = t.send_cmd("cat mvdst")
    check("mv — dst content", r, "movedata")
    r = t.send_cmd("cat mvsrc")
    check("mv — src removed", r, "Cannot read")
    t.send_cmd("rm mvdst")

    r = t.send_cmd("mv")
    check("mv no args — usage", r, "Usage:")


def test_grep(t: Telnet):
    """grep: search text in file."""
    t.send_cmd("write grepfile hello world")
    r = t.send_cmd("grep hello grepfile")
    check("grep — match found", r, "hello")
    r = t.send_cmd("grep nothere grepfile")
    check("grep — no match", r, "No matches")
    t.send_cmd("rm grepfile")

    r = t.send_cmd("grep")
    check("grep no args — usage", r, "Usage:")


def test_df(t: Telnet):
    """df: disk usage."""
    r = t.send_cmd("df")
    check("df — header", r, "Filesystem")
    check("df — device", r, "/dev/hda")


def test_free(t: Telnet):
    """free: memory usage."""
    r = t.send_cmd("free")
    check("free — header", r, "total")
    check("free — mem line", r, "Mem:")
    check("free — frames", r, "Frames:")


def test_whoami(t: Telnet):
    """whoami: current process."""
    r = t.send_cmd("whoami")
    check("whoami — PID", r, "PID")


def test_hostname(t: Telnet):
    """hostname: system hostname."""
    r = t.send_cmd("hostname")
    check("hostname", r, "os-kernel")


def test_env(t: Telnet):
    """env: environment info."""
    r = t.send_cmd("env")
    check("env — PID", r, "PID=")
    check("env — IP", r, "IP=")
    check("env — HOSTNAME", r, "HOSTNAME=os-kernel")


def test_xxd(t: Telnet):
    """xxd: hex dump file."""
    t.send_cmd("write xxdfile ABCD")
    r = t.send_cmd("xxd xxdfile")
    check("xxd — offset", r, "00000000:")
    # 'A' = 0x41
    check("xxd — hex bytes", r, "41")
    t.send_cmd("rm xxdfile")

    r = t.send_cmd("xxd")
    check("xxd no args — usage", r, "Usage:")


def test_sleep(t: Telnet):
    """sleep: wait N seconds."""
    r = t.send_cmd("sleep 1", timeout=10)
    check("sleep — slept", r, "Slept 1 seconds")

    r = t.send_cmd("sleep")
    check("sleep no args — usage", r, "Usage:")


def test_seq(t: Telnet):
    """seq: number sequence."""
    r = t.send_cmd("seq 5")
    check("seq — has 1", r, "1")
    check("seq — has 5", r, "5")
    r = t.send_cmd("seq 3 5")
    check("seq range — has 3", r, "3")
    check("seq range — has 5", r, "5")

    r = t.send_cmd("seq")
    check("seq no args — usage", r, "Usage:")


def test_arp(t: Telnet):
    """arp: show ARP cache."""
    r = t.send_cmd("arp")
    check("arp — header", r, "ARP cache")
    check("arp — entries", r, "Entries:")


def test_route(t: Telnet):
    """route: show routing table."""
    r = t.send_cmd("route")
    check("route — header", r, "Routing table")
    check("route — default", r, "default")


def test_uname(t: Telnet):
    """uname: system info."""
    r = t.send_cmd("uname")
    check("uname", r, "OS kernel x86_64")


def test_lspci(t: Telnet):
    """lspci: list PCI devices."""
    r = t.send_cmd("lspci")
    check("lspci — header", r, "BUS SLOT VID:DID")


def test_dmesg(t: Telnet):
    """dmesg: boot log."""
    r = t.send_cmd("dmesg")
    check("dmesg — response", r, "Boot log")


# ── Main ───────────────────────────────────────────────────────────────────────

def main() -> int:
    print("=" * 52)
    print("    OS Shell E2E Test Suite")
    print("=" * 52)
    print(f"  Target: {HOST}:{PORT}")
    print()

    # Connect
    print(f"Connecting to {HOST}:{PORT} (telnet)...")
    try:
        t = Telnet(HOST, PORT)
    except ConnectionRefusedError:
        print(f"[FAIL] connection — refused (is QEMU running with hostfwd?)")
        return 1
    except OSError as e:
        print(f"[FAIL] connection — {e}")
        return 1

    # Wait for initial shell prompt
    banner_raw = t.read_until(PROMPT, timeout=BOOT_TIMEOUT)
    banner = banner_raw.decode("latin-1", errors="replace")
    if "os>" not in banner:
        print(f"[FAIL] initial prompt — did not receive 'os>' in {BOOT_TIMEOUT}s")
        print(f"       banner: {banner!r}")
        return 1
    ok("telnet connection & initial prompt")

    # Run all test groups
    tests = [
        ("help",       test_help),
        ("echo",       test_echo),
        ("meminfo",    test_meminfo),
        ("ps",         test_ps),
        ("uptime",     test_uptime),
        ("date",       test_date),
        ("cpuinfo",    test_cpuinfo),
        ("history",    test_history),
        ("color",      test_color),
        ("hexdump",    test_hexdump),
        ("mouse",      test_mouse),
        ("ifconfig",   test_ifconfig),
        ("beep",       test_beep),
        ("play",       test_play),
        ("udpsend",    test_udpsend),
        ("ping",       test_ping),
        ("dns",        test_dns),
        ("kill",       test_kill),
        ("filesystem", test_filesystem),
        ("run/script", test_run_script),
        ("error cases",test_error_cases),
        ("wc",         test_wc),
        ("head",       test_head),
        ("tail",       test_tail),
        ("cp",         test_cp),
        ("mv",         test_mv),
        ("grep",       test_grep),
        ("df",         test_df),
        ("free",       test_free),
        ("whoami",     test_whoami),
        ("hostname",   test_hostname),
        ("env",        test_env),
        ("xxd",        test_xxd),
        ("sleep",      test_sleep),
        ("seq",        test_seq),
        ("arp",        test_arp),
        ("route",      test_route),
        ("uname",      test_uname),
        ("lspci",      test_lspci),
        ("dmesg",      test_dmesg),
    ]

    for group_name, fn in tests:
        print(f"\n--- {group_name} ---")
        try:
            fn(t)
        except socket.timeout:
            fail(group_name, "socket timeout waiting for response")
        except Exception as e:
            fail(group_name, f"exception: {e}")

    t.close()
    ok("exit & disconnect")

    # ── Summary ───────────────────────────────────────────
    print()
    print("-" * 52)
    total = _pass + _fail
    print(f"Results: {_pass} passed, {_fail} failed  (total {total})")
    print()
    if _fail == 0:
        print("ALL E2E TESTS PASSED")
        return 0
    else:
        print("SOME E2E TESTS FAILED")
        return 1


if __name__ == "__main__":
    sys.exit(main())
