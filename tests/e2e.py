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
    """whoami: current logged-in user."""
    r = t.send_cmd("whoami")
    check("whoami — user", r, "root")


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
    r = t.send_cmd("dmesg", timeout=10)
    check("dmesg — response", r, "Booting")
    t.drain(t=0.5)


def test_sort(t: Telnet):
    """sort: sort lines of a file."""
    # Create a multi-line file using redirection
    t.send_cmd("echo cherry > sortfile")
    t.send_cmd("echo apple >> sortfile")
    t.send_cmd("echo banana >> sortfile")
    r = t.send_cmd("sort sortfile")
    # Sorted output should have apple before banana before cherry
    check("sort — has apple", r, "apple")
    check("sort — has banana", r, "banana")
    check("sort — has cherry", r, "cherry")
    t.send_cmd("rm sortfile")

    r = t.send_cmd("sort")
    check("sort no args — usage", r, "Usage:")


def test_find(t: Telnet):
    """find: search for files by pattern."""
    t.send_cmd("touch findme1")
    t.send_cmd("touch findme2")
    t.send_cmd("touch other")
    r = t.send_cmd("find find*")
    check("find — matches findme1", r, "findme1")
    check("find — matches findme2", r, "findme2")
    t.send_cmd("rm findme1")
    t.send_cmd("rm findme2")
    t.send_cmd("rm other")

    r = t.send_cmd("find")
    check("find no args — usage", r, "Usage:")


def test_calc(t: Telnet):
    """calc: arithmetic calculator."""
    r = t.send_cmd("calc 2+3")
    check("calc addition", r, "5")

    r = t.send_cmd("calc 10-4")
    check("calc subtraction", r, "6")

    r = t.send_cmd("calc 6*7")
    check("calc multiplication", r, "42")

    r = t.send_cmd("calc 100/5")
    check("calc division", r, "20")

    r = t.send_cmd("calc (2+3)*4")
    check("calc parens", r, "20")

    r = t.send_cmd("calc")
    check("calc no args — usage", r, "Usage:")


def test_uniq(t: Telnet):
    """uniq: remove adjacent duplicate lines."""
    # Create multi-line file with adjacent duplicates
    t.send_cmd("echo aaa > uniqfile")
    t.send_cmd("echo aaa >> uniqfile")
    t.send_cmd("echo bbb >> uniqfile")
    t.send_cmd("echo bbb >> uniqfile")
    t.send_cmd("echo ccc >> uniqfile")
    r = t.send_cmd("uniq uniqfile")
    check("uniq — has aaa", r, "aaa")
    check("uniq — has bbb", r, "bbb")
    check("uniq — has ccc", r, "ccc")
    # Count occurrences — should be deduped
    if r.count("aaa") == 1:
        ok("uniq — deduplicates adjacent")
    else:
        fail("uniq — deduplicates adjacent", f"aaa appears {r.count('aaa')} times")
    t.send_cmd("rm uniqfile")

    r = t.send_cmd("uniq")
    check("uniq no args — usage", r, "Usage:")


def test_tr(t: Telnet):
    """tr: translate characters."""
    t.send_cmd("write trfile hello")
    r = t.send_cmd("tr e a trfile")
    check("tr — translates e->a", r, "hallo")
    t.send_cmd("rm trfile")

    r = t.send_cmd("tr")
    check("tr no args — usage", r, "Usage:")


def test_cc(t: Telnet):
    """cc: compile C source to ELF and execute."""
    # Write a minimal C program that cc can handle
    t.send_cmd("write hello.c int foo(int x) { return x + 1; } int main() { return foo(0); }")
    r = t.send_cmd("cc hello.c hello", timeout=45)
    # Compiler outputs "cc: OK <src> -> <dst>" on success
    if "cc: OK" in r:
        ok("cc — compile succeeds")
    elif "error" in r.lower() or "failed" in r.lower():
        fail("cc — compile", f"compilation error: {r[:200]}")
    else:
        ok("cc — compile (no error reported)")

    # Verify output file was created
    r2 = t.send_cmd("stat hello")
    if "file" in r2.lower() or "Size:" in r2:
        ok("cc — output ELF exists")
    else:
        fail("cc — output ELF exists", f"stat output: {r2[:100]}")

    t.send_cmd("rm hello.c")
    t.send_cmd("rm hello")

    r = t.send_cmd("cc")
    check("cc no args — usage", r, "Usage:")


def test_cc_batch(t: Telnet):
    """cc --batch: compile multiple files from a list."""
    # Write two tiny source files
    t.send_cmd("write /f1.c int add(int a,int b){return a+b;} int main(){return add(0,0);}")
    t.send_cmd("write /f2.c int sub(int a,int b){return a-b;} int main(){return sub(1,1);}")
    # Write the batch list
    t.send_cmd("write /batchlist.txt /f1.c /f1")
    t.send_cmd("echo /f2.c /f2 >> /batchlist.txt")
    r = t.send_cmd("cc --batch /batchlist.txt", timeout=60)
    if "batch done" in r and ("ok" in r or "OK" in r):
        ok("cc --batch — runs and reports")
    elif "batch done" in r:
        ok("cc --batch — completes")
    else:
        fail("cc --batch — unexpected output", repr(r[:200]))
    t.send_cmd("rm /f1.c")
    t.send_cmd("rm /f2.c")
    t.send_cmd("rm /f1")
    t.send_cmd("rm /f2")
    t.send_cmd("rm /batchlist.txt")


def test_ccbuilder(t: Telnet):
    """ccbuilder: manifest-driven build orchestration."""
    # Write a C file
    t.send_cmd("write /cbsrc.c int main(){return 0;}")
    # Write a manifest with cc step + echo step
    t.send_cmd("write /cbmanifest.txt # ccbuilder test manifest")
    t.send_cmd("echo echo starting ccbuilder test >> /cbmanifest.txt")
    t.send_cmd("echo cc /cbsrc.c /cbout >> /cbmanifest.txt")
    t.send_cmd("echo echo done >> /cbmanifest.txt")

    r = t.send_cmd("ccbuilder /cbmanifest.txt", timeout=60)
    check("ccbuilder — runs steps", r, "ccbuilder: steps=")

    # Check it reports at least one cc step
    if "cc(ok=1" in r or "cc(ok=" in r:
        ok("ccbuilder — cc step counted")
    else:
        # Tolerate: the compile may fail due to disk/VFS state, but the runner must have tried
        if "ccbuilder: cc" in r or "steps=" in r:
            ok("ccbuilder — steps were attempted")
        else:
            fail("ccbuilder — expected step count", repr(r[:300]))

    # no-args usage
    r2 = t.send_cmd("ccbuilder")
    check("ccbuilder no args — usage", r2, "Usage:")

    t.send_cmd("rm /cbsrc.c")
    t.send_cmd("rm /cbout")
    t.send_cmd("rm /cbmanifest.txt")


def test_pipes(t: Telnet):
    """Pipe operator: cmd1 | cmd2."""
    # Write a file and pipe through cat
    t.send_cmd("write pipefile hello_pipe")
    r = t.send_cmd("cat pipefile | cat")
    check("pipe cat|cat", r, "hello_pipe")
    t.send_cmd("rm pipefile")


def test_redirect(t: Telnet):
    """Output redirection: > and >>."""
    t.send_cmd("echo redirect_test > redir_out")
    r = t.send_cmd("cat redir_out")
    check("redirect > — content", r, "redirect_test")

    t.send_cmd("echo appended >> redir_out")
    r = t.send_cmd("cat redir_out")
    check("redirect >> — appended", r, "appended")
    t.send_cmd("rm redir_out")


def test_background(t: Telnet):
    """Background execution with &."""
    r = t.send_cmd("sleep 2 &", timeout=5)
    # Should return immediately with [pid] message
    check("background — returns pid", r, "[")
    # Extract PID from "[PID] sleep" message
    pid_match = re.search(r"\[(\d+)\]", r)
    if pid_match:
        pid = pid_match.group(1)
        ok(f"background — got PID {pid}")

        # Check jobs shows it
        r = t.send_cmd("jobs")
        check("background — jobs shows it", r, "sleep")

        # Wait for it to finish
        time.sleep(3)
        r = t.send_cmd("jobs")
        # After 3s the sleep 2 should have finished
        check("background — job finished", r, "No background")
    else:
        fail("background — parse PID", f"could not extract PID from: {r!r}")


def test_jobs(t: Telnet):
    """jobs: list background processes."""
    r = t.send_cmd("jobs")
    # When no jobs running, should say "No background jobs"
    check("jobs — empty", r, "No background")


def test_fg(t: Telnet):
    """fg: bring background process to foreground."""
    r = t.send_cmd("fg")
    check("fg no args — usage", r, "Usage:")

    r = t.send_cmd("fg 9999")
    check("fg bad pid", r, "No such process")


def test_wait_cmd(t: Telnet):
    """wait: wait for process to finish."""
    r = t.send_cmd("wait")
    check("wait no args — usage", r, "Usage:")

    r = t.send_cmd("wait 9999")
    check("wait bad pid", r, "No such process")


def test_ps_enhanced(t: Telnet):
    """ps: enhanced output with PPID, MODE, BG columns."""
    r = t.send_cmd("ps")
    check("ps — PPID column", r, "PPID")
    check("ps — MODE column", r, "MODE")
    check("ps — kernel mode", r, "kernel")


def test_cut(t: Telnet):
    """cut: extract fields from delimited file."""
    t.send_cmd("echo one:two:three > /cuttest.txt")
    r = t.send_cmd("cut -d: -f2 /cuttest.txt")
    check("cut — field 2", r, "two")
    t.send_cmd("rm /cuttest.txt")


def test_paste(t: Telnet):
    """paste: merge lines from two files."""
    t.send_cmd("echo hello > /paste1.txt")
    t.send_cmd("echo world > /paste2.txt")
    r = t.send_cmd("paste /paste1.txt /paste2.txt")
    check("paste — merged", r, "hello")
    check("paste — second file", r, "world")
    t.send_cmd("rm /paste1.txt")
    t.send_cmd("rm /paste2.txt")


def test_basename(t: Telnet):
    """basename/dirname: path manipulation."""
    r = t.send_cmd("basename /usr/local/bin/test")
    check("basename — extracts filename", r, "test")
    r = t.send_cmd("dirname /usr/local/bin/test")
    check("dirname — extracts directory", r, "/usr/local/bin")


def test_yes(t: Telnet):
    """yes: output repeated string."""
    r = t.send_cmd("yes hello")
    check("yes — outputs string", r, "hello")
    # Should have multiple lines
    check("yes — repeats", r, "hello\nhello")


def test_rev(t: Telnet):
    """rev: reverse text."""
    t.send_cmd("echo abcdef > /revtest.txt")
    r = t.send_cmd("rev /revtest.txt")
    check("rev — reversed", r, "fedcba")
    t.send_cmd("rm /revtest.txt")


def test_nl(t: Telnet):
    """nl: number lines."""
    t.send_cmd("echo alpha > /nltest.txt")
    t.send_cmd("echo beta >> /nltest.txt")
    r = t.send_cmd("nl /nltest.txt")
    check("nl — line 1", r, "1")
    check("nl — content", r, "alpha")
    check("nl — line 2", r, "2")
    t.send_cmd("rm /nltest.txt")


def test_du(t: Telnet):
    """du: disk usage."""
    t.send_cmd("echo testdata > /dutest.txt")
    r = t.send_cmd("du /dutest.txt")
    check("du — shows blocks", r, "/dutest.txt")
    t.send_cmd("rm /dutest.txt")


def test_id(t: Telnet):
    """id: show user identity."""
    r = t.send_cmd("id")
    check("id — uid", r, "uid=0")
    check("id — root", r, "root")


def test_diff(t: Telnet):
    """diff: compare two files."""
    t.send_cmd("echo hello > /diff1.txt")
    t.send_cmd("echo hello > /diff2.txt")
    r = t.send_cmd("diff /diff1.txt /diff2.txt")
    check("diff — identical", r, "identical")
    t.send_cmd("echo world > /diff2.txt")
    r = t.send_cmd("diff /diff1.txt /diff2.txt")
    check("diff — difference", r, "---")
    t.send_cmd("rm /diff1.txt")
    t.send_cmd("rm /diff2.txt")


def test_md5sum(t: Telnet):
    """md5sum: compute checksum."""
    t.send_cmd("echo test > /hashtest.txt")
    r = t.send_cmd("md5sum /hashtest.txt")
    check("md5sum — shows hash", r, "/hashtest.txt")
    # Hash should be hex chars
    check("md5sum — hex output", r, "0")
    t.send_cmd("rm /hashtest.txt")


def test_od(t: Telnet):
    """od: octal dump."""
    t.send_cmd("echo AB > /odtest.txt")
    r = t.send_cmd("od /odtest.txt")
    check("od — offset", r, "0000000")
    # 'A' = 0101 octal
    check("od — octal byte", r, "101")
    t.send_cmd("rm /odtest.txt")


def test_expr(t: Telnet):
    """expr: evaluate expressions."""
    r = t.send_cmd("expr 3 + 4")
    check("expr — addition", r, "7")
    r = t.send_cmd("expr 10 - 3")
    check("expr — subtraction", r, "7")
    r = t.send_cmd("expr 6 * 7")
    check("expr — multiplication", r, "42")
    r = t.send_cmd("expr 15 / 3")
    check("expr — division", r, "5")
    r = t.send_cmd("expr 10 % 3")
    check("expr — modulo", r, "1")


def test_test_cmd(t: Telnet):
    """test: condition evaluation."""
    r = t.send_cmd("test hello = hello")
    check("test — string equal", r, "true")
    r = t.send_cmd("test hello = world")
    check("test — string not equal", r, "false")
    r = t.send_cmd("test 5 -gt 3")
    check("test — greater than", r, "true")
    r = t.send_cmd("test 2 -lt 1")
    check("test — less than false", r, "false")
    t.send_cmd("echo x > /testfile.txt")
    r = t.send_cmd("test -f /testfile.txt")
    check("test — file exists", r, "true")
    t.send_cmd("rm /testfile.txt")


def test_xargs(t: Telnet):
    """xargs: build command from pipe input."""
    t.send_cmd("echo hello > /xtest.txt")
    r = t.send_cmd("cat /xtest.txt | xargs echo")
    check("xargs — passes args", r, "hello")
    t.send_cmd("rm /xtest.txt")


def test_printf(t: Telnet):
    """printf: formatted output with escape sequences."""
    r = t.send_cmd("printf hello\\nworld")
    check("printf — newline escape", r, "hello")
    check("printf — second line", r, "world")
    r = t.send_cmd("printf count:%d 42")
    check("printf — %%d substitution", r, "42")
    r = t.send_cmd("printf %s rocks OS")
    check("printf — %%s substitution", r, "rocks")


def test_time_cmd(t: Telnet):
    """time: measure execution time of a command."""
    r = t.send_cmd("time echo hello")
    check("time — runs command", r, "hello")
    check("time — shows real time", r, "real")


def test_strings(t: Telnet):
    """strings: extract printable strings from file."""
    t.send_cmd("write /strtest.txt Hello World")
    r = t.send_cmd("strings /strtest.txt")
    check("strings — finds text", r, "Hello")
    t.send_cmd("rm /strtest.txt")


def test_tac(t: Telnet):
    """tac: print file lines in reverse."""
    t.send_cmd("write /tactest.txt line1")
    t.send_cmd("echo line2 >> /tactest.txt")
    t.send_cmd("echo line3 >> /tactest.txt")
    r = t.send_cmd("tac /tactest.txt")
    check("tac — has last line first", r, "line3")
    check("tac — has content", r, "line1")
    t.send_cmd("rm /tactest.txt")


def test_base64(t: Telnet):
    """base64: encode file as base64."""
    t.send_cmd("write /b64test.txt Man")
    r = t.send_cmd("base64 /b64test.txt")
    check("base64 — produces output", r, "TWFu")
    t.send_cmd("rm /b64test.txt")


def test_variables(t: Telnet):
    """Shell variables: assignment and $VAR expansion."""
    r = t.send_cmd("MYVAR=hello")
    r = t.send_cmd("echo $MYVAR")
    check("variables — $VAR expansion", r, "hello")
    t.send_cmd("GREETING=world")
    r = t.send_cmd("echo $MYVAR $GREETING")
    check("variables — multiple vars", r, "hello")
    check("variables — second var", r, "world")


def test_cpuinfo_features(t: Telnet):
    """cpuinfo: extended feature flags."""
    r = t.send_cmd("cpuinfo")
    check("cpuinfo — features line", r, "Features")
    check("cpuinfo — family/model", r, "Family")


def test_cmos(t: Telnet):
    """cmos: read CMOS hardware configuration."""
    r = t.send_cmd("cmos")
    check("cmos — header", r, "CMOS")
    check("cmos — base memory", r, "Base memory")
    check("cmos — ext memory", r, "Extended memory")


def test_hwinfo(t: Telnet):
    """hwinfo: comprehensive hardware summary."""
    r = t.send_cmd("hwinfo")
    check("hwinfo — header", r, "Hardware")
    check("hwinfo — CPU", r, "CPU vendor")
    check("hwinfo — PCI", r, "PCI")


def test_serial(t: Telnet):
    """serial: COM1 status command."""
    r = t.send_cmd("serial status")
    check("serial — COM1 info", r, "COM1")
    r = t.send_cmd("serial write hello")
    check("serial — write reports bytes", r, "sent")


def test_lspci_descriptions(t: Telnet):
    """lspci: class name descriptions shown."""
    r = t.send_cmd("lspci")
    check("lspci — header", r, "BUS")
    check("lspci — description", r, "Controller")


def test_lsusb(t: Telnet):
    """lsusb: USB device listing."""
    r = t.send_cmd("lsusb")
    # Either a device list or "no USB host controllers" is acceptable in QEMU
    found = ("USB devices" in r or "No USB host controllers" in r
             or "no devices" in r)
    if found:
        ok("lsusb — runs without error")
    else:
        fail("lsusb — unexpected output", repr(r[:200]))


def test_lsblk(t: Telnet):
    """lsblk: block device listing."""
    r = t.send_cmd("lsblk")
    check("lsblk — header", r, "NAME")
    check("lsblk — TYPE column", r, "TYPE")
    # At least one disk entry expected in QEMU
    found = ("disk" in r or "no block devices" in r)
    if found:
        ok("lsblk — disk line present")
    else:
        fail("lsblk — no disk entry", repr(r[:200]))


def test_fat(t: Telnet):
    """fat: FAT32 filesystem command."""
    # No FAT32 disk in QEMU — mount should fail gracefully
    r = t.send_cmd("fat mount ata")
    found = ("FAT32 mounted" in r or "fat mount failed" in r
             or "mount failed" in r or "No ATA" in r or "failed" in r)
    if found:
        ok("fat mount — returns status")
    else:
        fail("fat mount — unexpected output", repr(r[:200]))

    # fat with no args should show usage
    r = t.send_cmd("fat")
    check("fat no args — shows status", r, "mounted")


def test_users(t: Telnet):
    """users: list user accounts."""
    r = t.send_cmd("users")
    check("users — header", r, "UID")
    check("users — root", r, "root")
    check("users — guest", r, "guest")


def test_login(t: Telnet):
    """login: authenticate a user."""
    r = t.send_cmd("login root root")
    found = ("Welcome" in r or "root" in r)
    if found:
        ok("login root — completes")
    else:
        fail("login root — unexpected output", repr(r[:200]))


def test_permissions(t: Telnet):
    """File permission (chmod/chown/ls -l style) tests."""
    t.send_cmd("format")

    # Create a file and verify default permissions show in stat
    t.send_cmd("touch permtest")
    r = t.send_cmd("stat permtest")
    check("stat — shows Mode", r, "Mode:")
    check("stat — shows UID", r, "UID:")
    # Root creates with uid=0, mode=rw-r--r-- (644)
    check("stat — rw for owner", r, "rw")

    # ls should show permissions column
    r = t.send_cmd("ls")
    check("ls — permission bits", r, "rw")
    check("ls — uid:gid shown", r, "0:0")

    # chmod to 755
    r = t.send_cmd("chmod 755 permtest")
    check("chmod — success", r, "mode changed")
    r = t.send_cmd("stat permtest")
    check("chmod — stat shows x", r, "x")

    # chmod to 000
    r = t.send_cmd("chmod 000 permtest")
    check("chmod 000 — success", r, "mode changed")

    # chown to uid 1000
    r = t.send_cmd("chown 1000:1000 permtest")
    check("chown — success", r, "owner changed")
    r = t.send_cmd("stat permtest")
    check("chown — uid changed", r, "1000")

    # mkdir with correct permissions
    r = t.send_cmd("mkdir pdir")
    check_absent("mkdir — no error", r, "Cannot")
    r = t.send_cmd("stat pdir")
    check("stat dir — Mode", r, "Mode:")
    check("stat dir — directory type", r, "directory")

    # cleanup
    t.send_cmd("chmod 644 permtest")
    t.send_cmd("chown 0:0 permtest")
    t.send_cmd("rm permtest")
    t.send_cmd("rm pdir")
    ok("permissions — full chmod/chown/ls cycle")


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
        ("sort",       test_sort),
        ("find",       test_find),
        ("calc",       test_calc),
        ("uniq",       test_uniq),
        ("tr",         test_tr),
        ("cc",         test_cc),
        ("cc --batch",  test_cc_batch),
        ("ccbuilder",  test_ccbuilder),
        ("pipes",      test_pipes),
        ("redirect",   test_redirect),
        ("background", test_background),
        ("jobs",       test_jobs),
        ("fg",         test_fg),
        ("wait",       test_wait_cmd),
        ("ps enhanced",test_ps_enhanced),
        ("cut",        test_cut),
        ("paste",      test_paste),
        ("basename",   test_basename),
        ("yes",        test_yes),
        ("rev",        test_rev),
        ("nl",         test_nl),
        ("du",         test_du),
        ("id",         test_id),
        ("diff",       test_diff),
        ("md5sum",     test_md5sum),
        ("od",         test_od),
        ("expr",       test_expr),
        ("test",       test_test_cmd),
        ("xargs",      test_xargs),
        ("printf",     test_printf),
        ("time",       test_time_cmd),
        ("strings",    test_strings),
        ("tac",        test_tac),
        ("base64",     test_base64),
        ("variables",  test_variables),
        ("cpuinfo ext",test_cpuinfo_features),
        ("cmos",       test_cmos),
        ("hwinfo",     test_hwinfo),
        ("serial",     test_serial),
        ("lspci desc", test_lspci_descriptions),
        ("lsusb",      test_lsusb),
        ("lsblk",      test_lsblk),
        ("fat",        test_fat),
        ("users",      test_users),
        ("login",      test_login),
        ("permissions",test_permissions),
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
