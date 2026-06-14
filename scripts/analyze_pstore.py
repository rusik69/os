#!/usr/bin/env python3
"""
scripts/analyze_pstore.py — Kernel crash dump analysis for Hermes OS

Parses pstore (persistent storage) output from kernel crash logs.
Extracts register state, backtrace, panic message, and pretty-prints
for debugging.

The pstore region is a fixed physical memory area that survives reboots.
On the next boot, the kernel can dump the pstore content via debugfs
or serial.  This script parses that output.

Input formats supported:
  --raw       Raw hex dump of pstore region (physical memory dump)
  --log       Kernel boot log with pstore records (default)
  --file      Path to file containing pstore data

Usage:
  ./scripts/analyze_pstore.py --log serial.log
  ./scripts/analyze_pstore.py --raw pstore_dump.bin
  cat crash.log | ./scripts/analyze_pstore.py

Output:
  - Panic message
  - Register state (RIP, RSP, RAX, RBX, ...)
  - Backtrace / call stack
  - Summary of all pstore records

Uses Python 3 standard library only.
"""

import argparse
import re
import struct
import sys


# ── Pstore record format (must match kernel's pstore.h) ──────────────

PSTORE_RECORD_MAGIC = 0x5053544F  # "PSTO"
PSTORE_MAX_DATA_LEN = 2048

# Record types
PSTORE_TYPE_PANIC   = 1
PSTORE_TYPE_OOPS    = 2
PSTORE_TYPE_BUG     = 3
PSTORE_TYPE_REGISTERS = 4
PSTORE_TYPE_BACKTRACE = 5
PSTORE_TYPE_DMESG   = 6

RECORD_TYPE_NAMES = {
    PSTORE_TYPE_PANIC:     "PANIC",
    PSTORE_TYPE_OOPS:      "OOPS",
    PSTORE_TYPE_BUG:       "BUG",
    PSTORE_TYPE_REGISTERS: "REGISTERS",
    PSTORE_TYPE_BACKTRACE: "BACKTRACE",
    PSTORE_TYPE_DMESG:     "DMESG",
}

# ── Register names in order (x86-64, as stored by kernel) ────────────

X86_REGS = [
    "RIP", "RSP", "RAX", "RBX", "RCX", "RDX",
    "RSI", "RDI", "RBP", "R8",  "R9",  "R10",
    "R11", "R12", "R13", "R14", "R15",
    "CS",  "SS",  "RFLAGS", "CR0", "CR2", "CR3", "CR4",
]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Analyze kernel pstore crash dumps"
    )
    parser.add_argument(
        "--log", metavar="FILE",
        help="Kernel boot log containing pstore records"
    )
    parser.add_argument(
        "--raw", metavar="FILE",
        help="Raw binary pstore region dump"
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true",
        help="Show hex dump of each record"
    )
    return parser.parse_args()


def read_input(args):
    """Read data from the specified source."""
    if args.log:
        with open(args.log, "r", errors="replace") as f:
            return f.read()
    elif args.raw:
        with open(args.raw, "rb") as f:
            return f.read()
    else:
        # stdin
        if not sys.stdin.isatty():
            return sys.stdin.read()
        else:
            print("ERROR: No input source specified. Use --log, --raw, or pipe data.", file=sys.stderr)
            sys.exit(1)


def parse_pstore_header(data):
    """Parse the pstore region header (16 bytes)."""
    if len(data) < 16:
        return None
    magic, version, record_count, hdr_flags = struct.unpack_from("<IIII", data, 0)
    return {
        "magic": magic,
        "version": version,
        "record_count": record_count,
        "flags": hdr_flags,
    }


def parse_pstore_record(data, offset):
    """Parse a single pstore record at the given offset.
    Returns (record_dict, next_offset) or (None, offset) on failure.
    """
    if offset + 16 > len(data):
        return None, offset

    magic, rec_type, timestamp, length = struct.unpack_from("<IIII", data, offset)
    if magic != PSTORE_RECORD_MAGIC:
        return None, offset

    if length > PSTORE_MAX_DATA_LEN:
        length = PSTORE_MAX_DATA_LEN

    payload_offset = offset + 16
    payload = data[payload_offset:payload_offset + length]

    return {
        "magic": magic,
        "type": rec_type,
        "type_name": RECORD_TYPE_NAMES.get(rec_type, f"UNKNOWN({rec_type})"),
        "timestamp": timestamp,
        "length": length,
        "payload": payload,
    }, offset + 16 + length


def parse_registers(payload):
    """Parse register dump payload (binary format: 8 bytes per reg, x86-64 order)."""
    regs = {}
    num_regs = len(X86_REGS)
    expected_len = num_regs * 8

    if len(payload) < expected_len:
        # Maybe partial
        num = len(payload) // 8
        regs_partial = X86_REGS[:num]
        for i, name in enumerate(regs_partial):
            val = struct.unpack_from("<Q", payload, i * 8)[0]
            regs[name] = val
        return regs

    for i, name in enumerate(X86_REGS):
        val = struct.unpack_from("<Q", payload, i * 8)[0]
        regs[name] = val
    return regs


def parse_backtrace(payload):
    """Parse backtrace payload — array of uint64 return addresses."""
    bt = []
    count = len(payload) // 8
    for i in range(count):
        addr = struct.unpack_from("<Q", payload, i * 8)[0]
        if addr != 0:
            bt.append(addr)
    return bt


def parse_text_pstore(lines):
    """Parse pstore records from text/kernel log format.
    Looks for lines like:
      [PSTORE] Record: type=PANIC, timestamp=12345, length=256
      [PSTORE] Data: <hex bytes...>
    Or raw KUnit/debugfs output.
    """
    records = []
    current_rec = None
    hex_data = ""

    for line in lines:
        line = line.strip()

        # Match: [PSTORE] Record: type=<name>, timestamp=<num>, length=<num>
        m = re.match(
            r'\[PSTORE\]\s+Record:\s+type=(\w+),\s+timestamp=(\d+),'
            r'\s+length=(\d+)',
            line
        )
        if m:
            if current_rec:
                if hex_data:
                    try:
                        current_rec["payload"] = bytes.fromhex(hex_data)
                    except ValueError:
                        current_rec["payload"] = hex_data.encode()
                records.append(current_rec)
            current_rec = {
                "type_name": m.group(1),
                "timestamp": int(m.group(2)),
                "length": int(m.group(3)),
                "type": None,
            }
            hex_data = ""
            continue

        # Match: [PSTORE] Data: <hex...>
        m2 = re.match(r'\[PSTORE\]\s+Data:\s+(.+)$', line)
        if m2 and current_rec:
            hex_data += m2.group(1).strip()
            continue

        # Match hex-only lines after a record header (indented)
        if current_rec and re.match(r'^[0-9a-fA-F\s]+$', line) and len(line) > 10:
            hex_data += line.replace(" ", "")

    # Don't forget the last record
    if current_rec:
        if hex_data:
            try:
                current_rec["payload"] = bytes.fromhex(hex_data)
            except ValueError:
                current_rec["payload"] = hex_data.encode()
        records.append(current_rec)

    return records


def analyze_binary_pstore(data):
    """Analyze raw binary pstore region dump."""
    result = {
        "header": None,
        "records": [],
    }

    if len(data) < 16:
        print("[!] Data too short for pstore header")
        return result

    hdr = parse_pstore_header(data)
    result["header"] = hdr

    if not hdr or hdr["magic"] != PSTORE_RECORD_MAGIC:
        print("[!] Invalid pstore magic (not a pstore dump or wrong format)")
        # Try parsing as raw text
        return result

    offset = 16  # skip header
    for i in range(min(hdr["record_count"], 64)):
        rec, offset = parse_pstore_record(data, offset)
        if rec is None:
            break
        result["records"].append(rec)
        if offset >= len(data):
            break

    return result


def format_analysis(result, verbose=False):
    """Pretty-print the analysis results."""
    lines = []

    lines.append("=" * 60)
    lines.append("  PStore Crash Dump Analysis")
    lines.append("=" * 60)
    lines.append("")

    hdr = result.get("header")
    if hdr:
        lines.append(f"  Region header:")
        lines.append(f"    Magic:        0x{hdr['magic']:08X}")
        lines.append(f"    Version:      {hdr['version']}")
        lines.append(f"    Records:      {hdr['record_count']}")
        lines.append(f"    Flags:        0x{hdr['flags']:08X}")
        lines.append("")

    records = result.get("records", [])
    if not records:
        # Try text-based extraction
        return None

    lines.append(f"  Records: {len(records)}")
    lines.append("")

    for idx, rec in enumerate(records):
        lines.append(f"  ── Record {idx} ──────────────────────────────────")
        lines.append(f"    Type:      {rec.get('type_name', 'UNKNOWN')}")
        lines.append(f"    Timestamp: {rec.get('timestamp', 0)}")
        lines.append(f"    Length:    {rec.get('length', 0)} bytes")
        lines.append("")

        payload = rec.get("payload", b"")
        type_name = rec.get("type_name", "")

        if type_name == "PANIC":
            # First bytes are the panic message string
            msg = payload.decode("utf-8", errors="replace").rstrip("\x00")
            lines.append(f"    🔴 PANIC MESSAGE:")
            for line in msg.split("\n"):
                lines.append(f"      {line}")

        elif type_name in ("REGISTERS",) or "REG" in type_name:
            regs = parse_registers(payload)
            lines.append(f"    CPU Register State:")
            for name in X86_REGS:
                if name in regs:
                    val = regs[name]
                    if name == "RIP":
                        lines.append(f"      {name:>8} = 0x{val:016X}  ← instruction pointer")
                    elif name == "RSP":
                        lines.append(f"      {name:>8} = 0x{val:016X}  ← stack pointer")
                    elif name == "CR2":
                        lines.append(f"      {name:>8} = 0x{val:016X}  ← page fault address")
                    else:
                        lines.append(f"      {name:>8} = 0x{val:016X}")

        elif type_name == "BACKTRACE":
            bt = parse_backtrace(payload)
            lines.append(f"    Backtrace ({len(bt)} frames):")
            for i, addr in enumerate(bt):
                marker = "  ← exception frame" if i == 0 else ""
                lines.append(f"      #{i:<4} 0x{addr:016X}{marker}")

        elif type_name in ("OOPS", "BUG"):
            msg = payload.decode("utf-8", errors="replace").rstrip("\x00")
            lines.append(f"    ⚠️  {type_name} MESSAGE:")
            for line in msg.split("\n"):
                lines.append(f"      {line}")

        elif type_name == "DMESG":
            msg = payload.decode("utf-8", errors="replace").rstrip("\x00")
            lines.append(f"    Kernel Log:")
            for line in msg.split("\n"):
                if line.strip():
                    lines.append(f"      {line}")

        else:
            # Unknown type — show hex
            if verbose:
                lines.append(f"    Raw data ({len(payload)} bytes):")
                hex_str = payload.hex() if isinstance(payload, bytes) else payload
                for i in range(0, len(hex_str), 64):
                    lines.append(f"      {hex_str[i:i+64]}")

        lines.append("")

    # Summary
    panic_count = sum(1 for r in records if r.get("type_name") == "PANIC")
    oops_count = sum(1 for r in records if r.get("type_name") == "OOPS")
    bug_count = sum(1 for r in records if r.get("type_name") == "BUG")

    if panic_count > 0 or oops_count > 0 or bug_count > 0:
        lines.append("  ── Summary ───────────────────────────────────────")
        lines.append(f"    Panics:  {panic_count}")
        lines.append(f"    Oops:    {oops_count}")
        lines.append(f"    BUGs:    {bug_count}")
        lines.append(f"    Total:   {len(records)} records")
        lines.append("")

    return "\n".join(lines)


def main():
    args = parse_args()
    input_data = read_input(args)
    result = None

    if args.raw:
        # Binary dump
        if isinstance(input_data, str):
            input_data = input_data.encode("latin-1")
        result = analyze_binary_pstore(input_data)
    else:
        # Text log — try to parse pstore records from it
        if isinstance(input_data, bytes):
            input_data = input_data.decode("utf-8", errors="replace")

        lines = input_data.splitlines()

        # First pass: look for binary pstore markers
        is_binary = False
        for line in lines[:10]:
            if "[PSTORE]" in line:
                is_binary = False
                break

        if not is_binary:
            # Try text format first
            records = parse_text_pstore(lines)
            if records:
                result = {"header": None, "records": records}

    if not result or not result.get("records"):
        # No structured records found; show raw log context
        if isinstance(input_data, str):
            lines = input_data.splitlines()
            print("=" * 60)
            print("  PStore Analysis — Raw Log Context")
            print("=" * 60)
            print("")
            print("  No structured pstore records found in input.")
            print("  Showing last 40 lines of log for manual inspection:")
            print("")
            for line in lines[-40:]:
                print(f"  {line}")
            return 1

    output = format_analysis(result, verbose=args.verbose)
    if output:
        print(output)
    else:
        print("[!] Could not parse pstore data from input.")
        return 1

    # Count failures
    if result and result.get("records"):
        failed = sum(1 for r in result["records"]
                     if r.get("type_name") in ("PANIC", "OOPS", "BUG"))
    else:
        failed = 1
    return 1 if failed > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
