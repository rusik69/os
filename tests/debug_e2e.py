#!/usr/bin/env python3
import socket, time, re, os, sys

HOST = os.environ.get("E2E_HOST", "127.0.0.1")
PORT = int(os.environ.get("E2E_PORT", "12323"))
TIMEOUT = 10
PROMPT = b"os> "

class Telnet:
    def __init__(self, host, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((host, port))
        self.sock.settimeout(TIMEOUT)
        self._buf = b""
        print(f"Connected to {host}:{port}", flush=True)
        time.sleep(0.5)
        print("Sleep done, calling read_until...", flush=True)

    @staticmethod
    def _strip_iac(data):
        out = bytearray()
        i = 0
        while i < len(data):
            b = data[i]
            if b == 0xFF and i + 2 < len(data):
                i += 3
            else:
                out.append(b)
                i += 1
        return bytes(out)

    def read_until(self, marker, timeout=None):
        t_max = timeout or TIMEOUT
        deadline = time.monotonic() + t_max
        iters = 0
        while marker not in self._buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                print(f"  TIMEOUT after {iters} iters, buf={self._buf[:50]!r}", flush=True)
                break
            self.sock.settimeout(min(remaining, 2.0))
            try:
                chunk = self.sock.recv(4096)
                iters += 1
                print(f"  recv[{iters}] len={len(chunk)} hex={chunk[:20].hex()}", flush=True)
                if not chunk:
                    break
                self._buf += self._strip_iac(chunk)
                print(f"  buf now: {self._buf[:60]!r}", flush=True)
            except socket.timeout:
                iters += 1
                print(f"  timeout[{iters}] remaining={remaining:.1f}", flush=True)
        idx = self._buf.find(marker)
        if idx >= 0:
            end = idx + len(marker)
            result = self._buf[:end]
            self._buf = b""
        else:
            result, self._buf = self._buf, b""
        return result

t = Telnet(HOST, PORT)
print("Calling read_until for banner...", flush=True)
banner = t.read_until(PROMPT, timeout=15)
print(f"Banner result: {banner!r}", flush=True)
