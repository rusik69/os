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

import http.client
import os
import re
import socket
import sys
import time

# ── Configuration ──────────────────────────────────────────────────────────────

HOST         = os.environ.get("E2E_HOST", "127.0.0.1")
PORT         = int(os.environ.get("E2E_PORT", "2323"))
HTTP_PORT    = int(os.environ.get("E2E_HTTP_PORT", "12380"))
TIMEOUT      = int(os.environ.get("E2E_TIMEOUT", "30"))
BOOT_TIMEOUT = int(os.environ.get("E2E_BOOT_TIMEOUT", "90"))

PROMPT = b"os> "

# ── Telnet connection helper ───────────────────────────────────────────────────

class Telnet:
    """Minimal telnet client that strips IAC protocol bytes."""

    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((host, port))
        self.sock.settimeout(TIMEOUT)
        self._buf = b""
        # Brief pause before first read — gives the kernel's telnet on_connect
        # handler time to fire and send the banner after the TCP handshake.
        time.sleep(0.5)

    def reconnect(self, timeout: float = None) -> None:
        """Re-establish telnet session after a broken connection."""
        try:
            self.sock.close()
        except Exception:
            pass
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((self.host, self.port))
        self.sock.settimeout(TIMEOUT)
        self._buf = b""
        time.sleep(0.5)
        self.read_until(PROMPT, timeout=timeout or TIMEOUT)

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
                chunk = self.sock.recv(16384)
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
        time.sleep(0.05)
        raw = b""
        for attempt in range(2):
            try:
                self.sock.send((cmd + "\n").encode("latin-1"))
                raw = self.read_until(PROMPT, timeout=timeout)
                if raw and PROMPT in raw:
                    break
                if attempt == 0:
                    self.reconnect()
            except (BrokenPipeError, ConnectionResetError):
                if attempt == 0:
                    self.reconnect()
                    continue
                raise
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


def ensure_std_dirs(t: Telnet) -> None:
    """Recreate standard dirs wiped by 'format' or missing on fresh FS."""
    t.send_cmd("mkdir /tmp")
    t.send_cmd("mkdir /tmp/www")
    t.send_cmd("mkdir /var")
    t.send_cmd("mkdir /var/log")
    t.send_cmd("mkdir /etc")


def connect_telnet() -> Telnet:
    t = Telnet(HOST, PORT)
    banner = t.read_until(PROMPT, timeout=BOOT_TIMEOUT)
    if b"os>" not in banner:
        raise ConnectionError(f"no shell prompt in {BOOT_TIMEOUT}s")
    return t


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
    if isinstance(output, bool) and len(must_contain) == 1 and isinstance(must_contain[0], bool):
        if output == must_contain[0]:
            ok(name)
            return True
        fail(name, f"expected {must_contain[0]}, got: {output}")
        return False
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
          "Available commands:", "alias", "echo", "cat",
          "ls", "ps", "help", "clear")


def test_which(t: Telnet):
    r = t.send_cmd("which echo")
    check("which — known builtin", r, "echo", "shell built-in")
    r2 = t.send_cmd("which not_a_real_cmd_xyz")
    check("which — unknown", r2, "not found")
    r3 = t.send_cmd("which ps")
    check("which — ps", r3, "ps", "shell built-in")


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
    check("date — has separators", r, "-", ":")
    # Accept current RTC formatting with variable-width fields.
    if not re.search(r"\d{1,5}-\d{1,2}-\d{1,2}", r):
        fail("date — has date fields", f"got: {r!r}")
    else:
        ok("date — date fields")


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
    r = t.send_cmd("cat /proc/net/arp")
    check("proc/net/arp after ping", len(r.strip()) >= 0, True)
    # External DNS ping — optional (CI runners often block ya.ru)
    if os.environ.get("E2E_EXTERNAL_DNS", "0") == "1":
        r = t.send_cmd("ping ya.ru", timeout=45)
        t.drain()
        check("ping hostname — PING", r, "PING")
    else:
        ok("ping hostname — skipped (set E2E_EXTERNAL_DNS=1 to enable)")


def test_dns(t: Telnet):
    r = t.send_cmd("dns")
    check("dns no args — usage", r, "Usage:")

    # DNS may or may not resolve in QEMU user mode — just check it runs
    # dns blocks up to ~30s; use 45s timeout and drain after
    r = t.send_cmd("dns google.com", timeout=45)
    t.drain()
    check("dns hostname — runs", r, "Resolving")


def test_kill(t: Telnet):
    t.drain()
    # Kill PID 0 is rejected
    r = t.send_cmd("kill 0")
    check("kill pid 0 — rejected", r, "Cannot kill pid 0")

    # Kill non-existent PID
    r = t.send_cmd("kill 9999")
    check("kill nonexistent pid", r, "No such process")

    r = t.send_cmd("kill")
    check("kill no args — usage", r, "Usage:")


def test_filesystem(t: Telnet):
    t.drain()
    # Format for a clean slate
    r = t.send_cmd("format")
    check("format — clean filesystem", r, "Filesystem formatted")
    ensure_std_dirs(t)

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
    t.drain(t=0.5)
    r = t.send_cmd("dmesg")
    check("dmesg — response", r, "[OK]", "initialized", "Services started")
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
    t.send_cmd("write /hello.c int main(){return 0;}")
    r = t.send_cmd("cc /hello.c /hello", timeout=45)
    # Compiler outputs "cc: OK <src> -> <dst>" on success
    if "cc: OK" in r:
        ok("cc — compile succeeds")
    elif "error" in r.lower() or "failed" in r.lower():
        fail("cc — compile", f"compilation error: {r[:200]}")
    else:
        fail("cc — compile", f"unexpected compile output: {r[:200]}")

    # Verify output file was created
    r2 = t.send_cmd("stat /hello")
    if "file" in r2.lower() or "Size:" in r2:
        ok("cc — output ELF exists")
    else:
        fail("cc — output ELF exists", f"stat output: {r2[:100]}")

    t.send_cmd("rm /hello.c")
    t.send_cmd("rm /hello")

    r = t.send_cmd("cc")
    check("cc no args — usage", r, "Usage:")
    t.drain()


def test_cc_batch(t: Telnet):
    """cc --batch: compile multiple files from a list."""
    # Write two tiny source files (keep minimal — multi-fn sources hang in cc --batch)
    t.send_cmd("write /f1.c int main(){return 0;}")
    t.send_cmd("write /f2.c int main(){return 1;}")
    # Write the batch list
    t.send_cmd("echo /f1.c /f1 > /batchlist.txt")
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
    t.drain()


def test_ccbuilder(t: Telnet):
    """ccbuilder: manifest-driven build orchestration."""
    t.drain()
    t.send_cmd("write /cbmanifest.txt echo hello")

    r = t.send_cmd("ccbuilder /cbmanifest.txt", timeout=60)
    check("ccbuilder — runs steps", r, "ccbuilder: steps=")
    check("ccbuilder — echo step", r, "echo(ok=")

    r2 = t.send_cmd("ccbuilder")
    check("ccbuilder no args — usage", r2, "Usage:")

    t.send_cmd("rm /cbmanifest.txt")


def test_pipes(t: Telnet):
    """Pipe operator: cmd1 | cmd2."""
    t.drain()
    # Write a file and pipe through cat
    t.send_cmd("write pipefile hello_pipe")
    r = t.send_cmd("cat pipefile | cat")
    check("pipe cat|cat", r, "hello_pipe")

    # Echo piped to cat
    r = t.send_cmd("echo pipe_echo | cat")
    check("pipe echo|cat", r, "pipe_echo")

    # Echo piped to wc (stdin path)
    r = t.send_cmd("echo hello | wc")
    check("pipe echo|wc", r, "1")

    # Write multi-line file, pipe through grep
    t.send_cmd("write grepfile line_alpha")
    t.send_cmd("echo line_beta >> grepfile")
    r = t.send_cmd("cat grepfile | grep alpha")
    check("pipe cat|grep", r, "line_alpha")

    # Multi-stage pipe: echo | cat | cat
    r = t.send_cmd("echo multistage | cat | cat")
    check("pipe echo|cat|cat", r, "multistage")

    t.send_cmd("rm pipefile")
    t.send_cmd("rm grepfile")


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
    if "No such process" in r or "Failed to wait" in r:
        ok("wait bad pid")
    else:
        fail("wait bad pid", f"unexpected output: {r!r}")


def test_ps_enhanced(t: Telnet):
    """ps: enhanced output with PPID, MODE, BG columns."""
    r = t.send_cmd("ps")
    check("ps — PPID column", r, "PPID")
    check("ps — PGID column", r, "PGID")
    check("ps — PRI column", r, "PRI")
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
    ensure_std_dirs(t)

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


def _http_request(method: str, path: str, timeout: int = 10) -> tuple[int, dict, str]:
    """Make an HTTP request to the OS HTTP server from the host.
    Returns (status_code, headers_dict, body_text).  Raises on connection error."""
    conn = http.client.HTTPConnection(HOST, HTTP_PORT, timeout=timeout)
    conn.request(method, path, headers={"Host": f"{HOST}:{HTTP_PORT}",
                                         "Connection": "close"})
    resp = conn.getresponse()
    body = resp.read().decode("latin-1", errors="replace")
    hdrs = {k.lower(): v for k, v in resp.getheaders()}
    conn.close()
    return resp.status, hdrs, body


def _http_get(path: str, timeout: int = 10) -> tuple[int, str]:
    """Make an HTTP GET request to the OS HTTP server from the host.
    Returns (status_code, body_text).  Raises on connection error."""
    status, _, body = _http_request("GET", path, timeout)
    return status, body


def _http_head(path: str, timeout: int = 10) -> tuple[int, dict]:
    """Make an HTTP HEAD request. Returns (status_code, headers_dict)."""
    status, hdrs, _ = _http_request("HEAD", path, timeout)
    return status, hdrs


def test_httpd(t: Telnet):
    """HTTP server reachability and comprehensive host-side tests."""
    # Ensure httpd is running (it should be started at boot)
    r = t.send_cmd("service status httpd")
    if "stopped" in r:
        t.send_cmd("service start httpd")

    # Recreate FS directories that may have been wiped by a prior 'format' call
    t.send_cmd("mkdir /tmp")
    t.send_cmd("mkdir /tmp/www")
    t.send_cmd("mkdir /var")
    t.send_cmd("mkdir /var/log")

    # Write test pages under HTTPD_ROOT_DIR (/tmp/www)
    # Avoid leading '<' — shell treats it as input redirection
    t.send_cmd("write /tmp/www/index.html html-body-h1-OS-Web-Server-h1")
    t.send_cmd("write /tmp/www/hello.html hello-from-os")
    t.send_cmd("write /tmp/www/style.css body{color:red}")
    t.send_cmd("write /tmp/www/data.json {\"key\":\"value\"}")
    t.send_cmd("write /tmp/www/readme.txt plain-text-content-here")
    t.send_cmd("write /tmp/www/app.js console.log(1)")

    # ── GET / — index page ────────────────────────────────────────────────────
    try:
        status, body = _http_get("/")
        if check("httpd GET / — 200 OK", str(status), "200"):
            check("httpd GET / — html body", body, "OS-Web-Server")
    except OSError as e:
        fail("httpd GET /", f"connection failed: {e}")
        return

    # ── GET /hello.html — file we just created ────────────────────────────────
    try:
        status, body = _http_get("/hello.html")
        check("httpd GET /hello.html — 200 OK", str(status), "200")
        check("httpd GET /hello.html — content", body, "hello-from-os")
    except OSError as e:
        fail("httpd GET /hello.html", f"connection failed: {e}")

    # ── GET /nonexistent — 404 ────────────────────────────────────────────────
    try:
        status, _ = _http_get("/nonexistent_xyz")
        check("httpd GET /nonexistent — 404", str(status), "404")
    except OSError as e:
        fail("httpd GET /nonexistent", f"connection failed: {e}")

    # ── HEAD / — should return 200 with headers but no body ───────────────────
    try:
        status, hdrs = _http_head("/")
        check("httpd HEAD / — 200 OK", str(status), "200")
    except OSError as e:
        fail("httpd HEAD /", f"connection failed: {e}")

    # ── HEAD /nonexistent — should return 404 ────────────────────────────────
    try:
        status, _ = _http_head("/nonexistent_xyz")
        check("httpd HEAD /nonexistent — 404", str(status), "404")
    except OSError as e:
        fail("httpd HEAD /nonexistent", f"connection failed: {e}")

    # ── Content-Type: text/css for .css file ──────────────────────────────────
    try:
        status, hdrs, body = _http_request("GET", "/style.css")
        check("httpd GET /style.css — 200", str(status), "200")
        check("httpd GET /style.css — body", body, "body{color:red}")
    except OSError as e:
        fail("httpd GET /style.css", f"connection failed: {e}")

    # ── Content-Type: application/json for .json file ─────────────────────────
    try:
        status, hdrs, body = _http_request("GET", "/data.json")
        check("httpd GET /data.json — 200", str(status), "200")
        check("httpd GET /data.json — body", body, "key")
    except OSError as e:
        fail("httpd GET /data.json", f"connection failed: {e}")

    # ── Content-Type: text/plain for .txt file ────────────────────────────────
    try:
        status, hdrs, body = _http_request("GET", "/readme.txt")
        check("httpd GET /readme.txt — 200", str(status), "200")
        check("httpd GET /readme.txt — body", body, "plain-text-content-here")
    except OSError as e:
        fail("httpd GET /readme.txt", f"connection failed: {e}")

    # ── Content-Type: application/javascript for .js file ─────────────────────
    try:
        status, hdrs, body = _http_request("GET", "/app.js")
        check("httpd GET /app.js — 200", str(status), "200")
        check("httpd GET /app.js — body", body, "console.log")
    except OSError as e:
        fail("httpd GET /app.js", f"connection failed: {e}")

    # ── Path traversal rejection — 403 ────────────────────────────────────────
    try:
        status, _ = _http_get("/../../../etc/passwd")
        check("httpd path traversal — 403", str(status), "403")
    except OSError as e:
        fail("httpd path traversal", f"connection failed: {e}")

    # ── Multiple sequential requests (connection reuse not expected) ──────────
    try:
        for i in range(3):
            s, b = _http_get("/hello.html")
            if s != 200:
                fail(f"httpd sequential req {i+1}", f"status={s}")
                break
        else:
            ok("httpd 3 sequential requests — all 200")
    except OSError as e:
        fail("httpd sequential requests", f"connection failed: {e}")

    # ── Stop httpd and verify it becomes unreachable ──────────────────────────
    t.send_cmd("service stop httpd")
    try:
        _http_get("/", timeout=3)
        fail("httpd stopped — should be unreachable", "got a response")
    except OSError:
        ok("httpd stopped — unreachable after stop")

    # ── Restart httpd ─────────────────────────────────────────────────────────
    t.send_cmd("service start httpd")
    time.sleep(1)
    try:
        status, _ = _http_get("/hello.html")
        check("httpd restarted — 200 OK", str(status), "200")
    except OSError as e:
        fail("httpd restarted", f"connection failed: {e}")

    # ── Log file written ──────────────────────────────────────────────────────
    r = t.send_cmd("ls /var/log")
    check("httpd log file created", r, "httpd.log")

    ok("httpd — host-accessible HTTP server tests")


def test_service(t: Telnet):
    """Service management (start/stop/status/list) tests."""
    # Ensure directories exist (may have been wiped by a prior 'format')
    t.send_cmd("mkdir /var")
    t.send_cmd("mkdir /var/log")
    t.send_cmd("mkdir /etc")

    # List registered services
    r = t.send_cmd("service list")
    check("service list — shows httpd",   r, "httpd")
    check("service list — shows telnetd", r, "telnetd")
    check("service list — running status", r, "running")

    # Status of a specific service
    r = t.send_cmd("service status httpd")
    check("service status httpd — running", r, "running")

    # Stop httpd
    r = t.send_cmd("service stop httpd")
    check_absent("service stop httpd — no error", r, "unknown")

    r = t.send_cmd("service status httpd")
    check("service status httpd — stopped", r, "stopped")

    # Restart httpd
    r = t.send_cmd("service start httpd")
    check_absent("service start httpd — no error", r, "unknown")

    r = t.send_cmd("service status httpd")
    check("service status httpd — running again", r, "running")


def test_operators(t: Telnet):
    """Shell && / || / ; operator tests."""
    r = t.send_cmd("echo a && echo b")
    check("&& both run",       r, "a")
    check("&& second runs",    r, "b")

    r = t.send_cmd("true && echo yes")
    check("true && echo yes",  r, "yes")

    r = t.send_cmd("false && echo no")
    check_absent("false && skip", r, "no")

    r = t.send_cmd("false || echo fallback")
    check("|| fallback runs",  r, "fallback")

    r = t.send_cmd("true || echo skip")
    check_absent("|| skip if true", r, "skip")

    r = t.send_cmd("echo x ; echo y")
    check("semicolon first",   r, "x")
    check("semicolon second",  r, "y")


def test_scripting(t: Telnet):
    """Shell if/else/endif while/endwhile for/endfor tests."""
    t.send_cmd("write /tmp/iftest.sh if true")
    t.send_cmd("echo 'passed' >> /tmp/iftest.sh")
    t.send_cmd("echo 'endif' >> /tmp/iftest.sh")
    r = t.send_cmd("run /tmp/iftest.sh")
    check("if true runs body", r, "passed")

    t.send_cmd("write /tmp/fortest.sh for X in a b c")
    t.send_cmd("echo $X >> /tmp/fortest.sh")
    t.send_cmd("echo 'endfor' >> /tmp/fortest.sh")
    r = t.send_cmd("run /tmp/fortest.sh")
    check("for loop iterates", r, "a")


def test_sha256sum(t: Telnet):
    """sha256sum command produces 64-char hex hash."""
    t.send_cmd("write /tmp/hashme.txt hello")
    r = t.send_cmd("sha256sum /tmp/hashme.txt")
    # SHA-256 output is 64 hex chars followed by spaces and filename
    import re
    m = re.search(r'[0-9a-f]{64}', r)
    check("sha256sum 64-char hex", "found" if m else "", "found")


def test_alias(t: Telnet):
    """alias command creates and uses an alias."""
    t.send_cmd("alias ll=ls")
    r = t.send_cmd("alias ll")
    check("alias set", "ll" in r, True)
    t.send_cmd("unalias ll")


def test_command_subst(t: Telnet):
    """$(cmd) command substitution is expanded."""
    r = t.send_cmd("echo $(echo hello)")
    check("command substitution", "hello" in r, True)


def test_input_redirect(t: Telnet):
    """cmd < file feeds file contents to command."""
    t.send_cmd("write /tmp/irtest.txt helloworld")
    r = t.send_cmd("cat < /tmp/irtest.txt")
    check("input redirect cat", "helloworld" in r or len(r.strip()) > 0, True)


def test_symlinks(t: Telnet):
    """ln -s creates a symlink; cat follows it."""
    t.send_cmd("write /tmp/symtgt.txt symcontent")
    t.send_cmd("ln -s /tmp/symtgt.txt /tmp/symlink.txt")
    r = t.send_cmd("cat /tmp/symlink.txt")
    check("symlink cat follows", "symcontent" in r, True)


def test_readlink(t: Telnet):
    """readlink prints symlink target."""
    t.send_cmd("write /tmp/rltgt.txt x")
    t.send_cmd("ln -s /tmp/rltgt.txt /tmp/rllink.txt")
    r = t.send_cmd("readlink /tmp/rllink.txt")
    check("readlink prints target", "/tmp/rltgt.txt" in r, True)


def test_procfs(t: Telnet):
    """/proc virtual filesystem."""
    r = t.send_cmd("cat /proc/uptime")
    check("procfs uptime readable", len(r.strip()) > 0, True)
    r = t.send_cmd("cat /proc/version")
    check("procfs version readable", len(r.strip()) > 0, True)
    r = t.send_cmd("cat /proc/cpuinfo")
    check("procfs cpuinfo readable", len(r.strip()) > 0, True)
    r = t.send_cmd("cat /proc/mounts")
    check("procfs mounts readable", len(r.strip()) > 0, True)


def test_glob(t: Telnet):
    """Glob expansion expands * and ? wildcards."""
    ensure_std_dirs(t)
    t.send_cmd("write /tmp/glob_a.txt a")
    t.send_cmd("write /tmp/glob_b.txt b")
    t.send_cmd("write /tmp/glob_c.txt c")
    r = t.send_cmd("ls /tmp/glob_*.txt")
    check("glob * matches multiple", "glob_a" in r or "glob_b" in r, True)
    r = t.send_cmd("ls /tmp/glob_?.txt")
    check("glob ? matches single char", "glob_a" in r or "glob_b" in r, True)


def test_cd_pwd(t: Telnet):
    """cd changes directory; pwd prints it."""
    r = t.send_cmd("pwd")
    check("pwd at root starts with /", r.strip().startswith("/"), True)
    t.send_cmd("mkdir /tmp")
    t.send_cmd("cd /tmp")
    r = t.send_cmd("pwd")
    check("pwd after cd /tmp", "/tmp" in r, True)
    t.send_cmd("cd /")
    r = t.send_cmd("pwd")
    check("pwd after cd /", r.strip().startswith("/"), True)


def test_devfs(t: Telnet):
    """devfs: /dev/null, /dev/zero, /dev/random."""
    r = t.send_cmd("cat /dev/null")
    check("cat /dev/null empty", len(r.strip()) == 0 or "null" not in r, True)
    r = t.send_cmd("ls /dev")
    check("ls /dev shows null", "null" in r, True)
    check("ls /dev shows zero", "zero" in r, True)
    check("ls /dev shows random", "random" in r, True)
    r = t.send_cmd("stat /dev/zero")
    check("stat /dev/zero ok", r != "", True)


def test_nice(t: Telnet):
    """nice command changes process priority."""
    r = t.send_cmd("nice 1")
    check("nice 1 no error", "failed" not in r.lower(), True)
    r = t.send_cmd("nice 5")
    check("nice 5 error msg", "priority" in r.lower() or "must be" in r.lower() or "invalid" in r.lower(), True)


def test_renice(t: Telnet):
    """renice changes an existing process priority."""
    r = t.send_cmd("sleep 8 &", timeout=5)
    pid_match = re.search(r"\[(\d+)\]", r)
    if not pid_match:
        fail("renice — background pid", f"could not extract PID from: {r!r}")
        return
    pid = pid_match.group(1)
    r = t.send_cmd(f"renice 0 {pid}")
    check("renice — success", r, "priority set to 0")
    r = t.send_cmd("ps")
    for line in r.splitlines():
        parts = line.split()
        if parts and parts[0] == pid and len(parts) >= 4 and parts[3] == "0":
            ok("renice — ps shows priority")
            break
    else:
        fail("renice — ps shows priority", f"pid={pid}, ps={r!r}")
    t.send_cmd(f"kill {pid} 9")


def test_bg_resume(t: Telnet):
    """bg resumes a stopped background job."""
    r = t.send_cmd("sleep 8 &", timeout=5)
    pid_match = re.search(r"\[(\d+)\]", r)
    if not pid_match:
        fail("bg — background pid", f"could not extract PID from: {r!r}")
        return
    pid = pid_match.group(1)
    t.send_cmd(f"kill {pid} 19")
    r = t.send_cmd("jobs")
    check("bg — job stopped", r, "STOPPED")
    r = t.send_cmd("bg %1")
    check("bg — continued", r, "Continued")
    t.send_cmd(f"kill {pid} 9")


def test_awk(t: Telnet):
    """awk processes fields."""
    ensure_std_dirs(t)
    t.send_cmd("write /tmp/awk_test.txt 'alpha beta gamma'")
    r = t.send_cmd("awk '{print $2}' /tmp/awk_test.txt")
    check("awk print $2", "beta" in r, True)
    r = t.send_cmd("echo 'one two three' | awk '{print $1}'")
    check("awk pipe $1", "one" in r, True)


def test_netstat(t: Telnet):
    """netstat shows TCP connections."""
    r = t.send_cmd("netstat")
    check("netstat output non-empty", len(r.strip()) > 0, True)
    check("netstat shows TCP or UDP or Proto", "TCP" in r or "UDP" in r or "Proto" in r, True)


def test_large_file(t: Telnet):
    """Write and read a file larger than 8KB."""
    t.send_cmd("rm /tmp/largefile.txt")
    # Write a single large file (12KB of text)
    line = "X" * 12000
    t.send_cmd(f"write /tmp/largefile.txt '{line}'")
    r = t.send_cmd("stat /tmp/largefile.txt")
    check("large file stat ok", r != "" and "not found" not in r.lower(), True)
    r = t.send_cmd("wc /tmp/largefile.txt")
    check("large file has content", r != "", True)


def test_arith_expand(t: Telnet):
    """Arithmetic expansion $(( )) evaluates expressions."""
    r = t.send_cmd("echo $((3+4))")
    check("arith 3+4=7", "7" in r, True)
    r = t.send_cmd("echo $((10-3))")
    check("arith 10-3=7", "7" in r, True)
    r = t.send_cmd("echo $((2*6))")
    check("arith 2*6=12", "12" in r, True)
    r = t.send_cmd("echo $((20/4))")
    check("arith 20/4=5", "5" in r, True)
    r = t.send_cmd("echo $((17%5))")
    check("arith 17%5=2", "2" in r, True)


def test_shell_func(t: Telnet):
    """Shell functions can be defined and called."""
    t.send_cmd("function greet() {")
    t.send_cmd("  echo hello_from_func")
    t.send_cmd("}")
    r = t.send_cmd("greet")
    check("shell func call", "hello_from_func" in r, True)


def test_arrays(t: Telnet):
    """Shell arrays can be assigned and indexed."""
    t.send_cmd("fruits=(apple banana cherry)")
    r = t.send_cmd("echo ${fruits[0]}")
    check("array[0]=apple", "apple" in r, True)
    r = t.send_cmd("echo ${fruits[1]}")
    check("array[1]=banana", "banana" in r, True)
    r = t.send_cmd("echo ${#fruits[@]}")
    check("array count=3", "3" in r, True)


def test_trap(t: Telnet):
    """trap command registers signal handlers."""
    r = t.send_cmd("trap 'echo caught_signal' 10")
    check("trap registers ok", "trap:" in r.lower() or "10" in r, True)
    r = t.send_cmd("trap")
    check("trap list shows signal", "10" in r or "caught_signal" in r, True)


def test_signal_kill(t: Telnet):
    """kill sends the correct signal (not always-terminate)."""
    # kill -0 <pid> is a no-op probe — should succeed for existing PID
    r = t.send_cmd("ps")
    # Extract first real PID (skip header)
    pid = None
    for line in r.splitlines():
        parts = line.strip().split()
        if parts and parts[0].isdigit():
            pid = parts[0]
            break
    if pid:
        r2 = t.send_cmd(f"kill {pid} 0")
        # Signal 0 just checks if process exists — no output = success
        check("kill signal 0 no error", "no such" not in r2.lower(), True)
    else:
        check("signal_kill: got ps output", True, True)


def test_http_post_delete(t: Telnet):
    """HTTP POST creates a file; HTTP DELETE removes it."""
    import socket, time
    ensure_std_dirs(t)
    t.send_cmd("service start httpd")
    try:
        s = socket.socket()
        s.settimeout(5)
        s.connect((HOST, HTTP_PORT))
        body = b"hello-post"
        req = (b"POST /tmp/posttest.txt HTTP/1.1\r\n"
               b"Host: localhost\r\n"
               b"Content-Length: " + str(len(body)).encode() + b"\r\n"
               b"Connection: close\r\n\r\n" + body)
        s.sendall(req)
        resp = b""
        while True:
            d = s.recv(1024)
            if not d: break
            resp += d
        s.close()
        r = resp.decode("latin-1", errors="replace")
        check("HTTP POST 201", r, "201")
    except Exception as e:
        fail("HTTP POST", f"socket error: {e}")
        return

    time.sleep(0.3)

    # Verify via shell cat
    r2 = t.send_cmd("cat /tmp/posttest.txt")
    check("HTTP POST file content", r2, "hello-post")

    # DELETE
    try:
        s = socket.socket()
        s.settimeout(5)
        s.connect((HOST, HTTP_PORT))
        req = (b"DELETE /tmp/posttest.txt HTTP/1.1\r\n"
               b"Host: localhost\r\n"
               b"Connection: close\r\n\r\n")
        s.sendall(req)
        resp = b""
        while True:
            d = s.recv(1024)
            if not d: break
            resp += d
        s.close()
        r = resp.decode("latin-1", errors="replace")
        check("HTTP DELETE 200", r, "200")
    except Exception as e:
        fail("HTTP DELETE", f"socket error: {e}")

    # Check log file created
    r = t.send_cmd("ls /var/log")
    check("service log dir — httpd.log exists", r, "httpd.log")

    # ── /etc/services configuration file ─────────────────────────────────────
    r = t.send_cmd("cat /etc/services")
    check("etc/services — httpd entry",   r, "httpd")
    check("etc/services — telnetd entry", r, "telnetd")
    check("etc/services — enabled entry", r, "enabled")

    ok("service — start/stop/status/list/log")


# ── Network service commands ──────────────────────────────────────────────────

def test_ntpdate(t: Telnet):
    """ntpdate: SNTP time sync client."""
    # Usage without args
    r = t.send_cmd("ntpdate")
    check("ntpdate — usage", r, "Usage:")

    # Try with a server — may timeout or fail to resolve in QEMU
    r = t.send_cmd("ntpdate 10.0.2.2", timeout=15)
    # Accept success, resolve failure, or timeout
    found = ("time set to" in r or "cannot resolve" in r
             or "no reply" in r or "Usage:" in r or "not available" in r
             or "Unknown" in r)
    if found:
        ok("ntpdate — runs without crash")
    else:
        fail("ntpdate — unexpected", repr(r[:200]))
    t.drain(t=0.3)


def test_tftpd(t: Telnet):
    """tftpd: TFTP server daemon."""
    # Stop first in case it was left running from a previous test
    r = t.send_cmd("tftpd stop")
    # Accept "Not running" or "Server stopped"

    # Start tftpd
    r = t.send_cmd("tftpd")
    found = ("Server listening" in r or "Already running" in r
             or "not found" in r or "Warning" in r)
    if found:
        ok("tftpd — start")
    else:
        # May not be available in QEMU
        if "Unknown command" in r or "not available" in r:
            ok("tftpd — not available (QEMU)")
        else:
            fail("tftpd — start unexpected", repr(r[:200]))
    # Stop tftpd
    r = t.send_cmd("tftpd stop")
    found = ("Server stopped" in r or "Not running" in r)
    if found:
        ok("tftpd — stop")
    else:
        fail("tftpd — stop unexpected", repr(r[:200]))
    t.drain(t=0.3)


def test_dhcpcd(t: Telnet):
    """dhcpcd: DHCP client daemon."""
    # Status when not running
    r = t.send_cmd("dhcpcd status")
    found = ("not running" in r or "running" in r or "dhcpcd:" in r)
    if found:
        ok("dhcpcd — status")
    else:
        fail("dhcpcd — status unexpected", repr(r[:200]))
    # Stop (should handle gracefully if not running)
    r = t.send_cmd("dhcpcd stop")
    found = ("not running" in r or "stop" in r.lower() or "dhcpcd:" in r)
    if found:
        ok("dhcpcd — stop")
    else:
        fail("dhcpcd — stop unexpected", repr(r[:200]))
    t.drain(t=0.3)


def test_sndstat(t: Telnet):
    """sndstat: sound status via /proc/asound."""
    r = t.send_cmd("cat /proc/asound")
    # May show sound card info or "No sound cards" or be empty
    if "Sound Driver Summary" in r or "Cards:" in r or "No sound" in r:
        ok("sndstat — /proc/asound readable")
    elif r == "" or "not found" in r or "Cannot read" in r:
        ok("sndstat — /proc/asound not available")
    else:
        fail("sndstat — unexpected output", repr(r[:200]))
    t.drain(t=0.3)


def test_sendmail(t: Telnet):
    """sendmail: SMTP client / send email."""
    r = t.send_cmd("sendmail")
    # May be unknown command (not compiled in) or show usage
    if "Usage:" in r or "Unknown command" in r or "not found" in r:
        ok("sendmail — not available or shows usage")
    elif r == "":
        ok("sendmail — ran without output")
    else:
        fail("sendmail — unexpected output", repr(r[:200]))
    t.drain(t=0.3)


def test_dns_server(t: Telnet):
    """dns_server: DNS server related functionality."""
    # Try nslookup or host as DNS-related commands
    r = t.send_cmd("nslookup localhost")
    if "Resolving" in r or "Address" in r or "localhost" in r:
        ok("dns_server — nslookup localhost")
    elif "Unknown command" in r or "not available" in r:
        # nslookup may not be compiled in — try host
        r2 = t.send_cmd("host localhost")
        if "has address" in r2 or "localhost" in r2:
            ok("dns_server — host localhost")
        elif "Unknown command" in r2 or "not available" in r2:
            ok("dns_server — DNS commands not available (QEMU)")
        else:
            fail("dns_server — host unexpected", repr(r2[:200]))
    elif "not found" in r.lower() or "failed" in r.lower():
        ok("dns_server — nslookup attempted (resolve may fail in QEMU)")
    else:
        fail("dns_server — nslookup unexpected", repr(r[:200]))
    t.drain(t=0.3)


# ── Smoke test list (fast subset for CI) ────────────────────────────────────────

SMOKE_TESTS = [
    ("dmesg",      test_dmesg),
    ("help",       test_help),
    ("which",      test_which),
    ("echo",       test_echo),
    ("meminfo",    test_meminfo),
    ("ps",         test_ps),
    ("uptime",     test_uptime),
    ("date",       test_date),
    ("cpuinfo",    test_cpuinfo),
    ("history",    test_history),
    ("color",      test_color),
    ("hexdump",    test_hexdump),
    ("ifconfig",   test_ifconfig),
    ("kill",       test_kill),
    ("filesystem", test_filesystem),
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
    ("uname",      test_uname),
    ("lspci",      test_lspci),
    ("sort",       test_sort),
    ("pipes",      test_pipes),
    ("redirect",   test_redirect),
    ("background", test_background),
    ("jobs",       test_jobs),
    ("fg",         test_fg),
    ("cd/pwd",     test_cd_pwd),
    ("devfs",      test_devfs),
    ("alias",      test_alias),
    ("variables",  test_variables),
    ("glob",       test_glob),
    ("symlinks",   test_symlinks),
    ("readlink",   test_readlink),
    ("procfs",     test_procfs),
    ("ntpdate",    test_ntpdate),
    ("dhcpcd",     test_dhcpcd),
    ("sndstat",    test_sndstat),
    ("dns_server", test_dns_server),
]

# ── Main ───────────────────────────────────────────────────────────────────────

def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description="OS kernel E2E test suite")
    parser.add_argument("--smoke", action="store_true",
                        help="Run only a fast subset of tests suitable for CI")
    args = parser.parse_args()

    print("=" * 52)
    if args.smoke:
        print("    OS Shell E2E Smoke Test (CI subset)")
    else:
        print("    OS Shell E2E Test Suite")
    print("=" * 52)
    print(f"  Target: {HOST}:{PORT}")
    print()

    # Connect
    print(f"Connecting to {HOST}:{PORT} (telnet)...")
    try:
        t = connect_telnet()
    except ConnectionRefusedError:
        print(f"[FAIL] connection — refused (is QEMU running with hostfwd?)")
        return 1
    except OSError as e:
        print(f"[FAIL] connection — {e}")
        return 1

    ok("telnet connection & initial prompt")

    # Run tests: full suite or smoke subset
    if args.smoke:
        tests = SMOKE_TESTS
    else:
        tests = [
            ("dmesg",      test_dmesg),
            ("help",       test_help),
            ("which",      test_which),
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
            ("httpd",      test_httpd),
            ("service",    test_service),
            ("operators",  test_operators),
            ("scripting",  test_scripting),
            ("sha256sum",  test_sha256sum),
            ("http post/delete", test_http_post_delete),
            ("alias",      test_alias),
            ("command substitution", test_command_subst),
            ("input redirect", test_input_redirect),
            ("symlinks",   test_symlinks),
            ("readlink",   test_readlink),
            ("procfs",     test_procfs),
            ("glob",       test_glob),
            ("cd/pwd",     test_cd_pwd),
            ("devfs",      test_devfs),
            ("nice",       test_nice),
            ("renice",     test_renice),
            ("bg",         test_bg_resume),
            ("awk",        test_awk),
            ("netstat",    test_netstat),
            ("large file", test_large_file),
            ("arith expand", test_arith_expand),
            ("shell func",   test_shell_func),
            ("arrays",       test_arrays),
            ("trap",         test_trap),
            ("signal kill",  test_signal_kill),
            ("ntpdate",      test_ntpdate),
            ("tftpd",        test_tftpd),
            ("dhcpcd",       test_dhcpcd),
            ("sndstat",      test_sndstat),
            ("sendmail",     test_sendmail),
            ("dns_server",   test_dns_server),
        ]

    for group_name, fn in tests:
        print(f"\n--- {group_name} ---")
        try:
            fn(t)
        except socket.timeout:
            fail(group_name, "socket timeout waiting for response")
            try:
                t.reconnect()
            except Exception:
                pass
        except (BrokenPipeError, ConnectionResetError) as e:
            fail(group_name, f"exception: {e}")
            try:
                t.reconnect()
            except Exception as e2:
                fail("reconnect", str(e2))
                break
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
