#!/usr/bin/env python3
"""
tests/junit_reporter.py — Wrap test binary runs in JUnit XML output.

Usage:
    python3 tests/junit_reporter.py <suite_name> <test_binary> [args...]

Runs the test binary, captures stdout/stderr, parses the output for
PASS/FAIL lines, and writes a JUnit XML file named:
    <suite_name>-results.xml

The XML file follows the JUnit schema accepted by GitHub Actions
(https://github.com/dorny/test-reporter).

Environment variables:
    JUNIT_DIR  - output directory for XML files (default: .)
    JUNIT_VERBOSE - set to 1 to print XML to stdout
"""

import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape


def parse_test_output(text: str):
    """Parse test output for PASS/FAIL/SKIP lines.

    Supports formats:
      "  PASS: some test name"          (host_libc style)
      "  FAIL: some test name"
      "  TEST: description ... PASS"    (tests/unit style)
      "  TEST: description ... FAIL"
      "  TEST: name ... PASS"           (kunit style)

    Returns list of (name, status, details) tuples.
    Status is one of: 'passed', 'failed', 'skipped'.
    """
    results = []
    for line in text.splitlines():
        stripped = line.strip()
        # Format 1: "PASS: name" or "FAIL: name"
        pass_match = re.match(r'^(?:PASS|OK):\s*(.+)$', stripped)
        fail_match = re.match(r'^FAIL:\s*(.+)$', stripped)
        skip_match = re.match(r'^SKIP:\s*(.+)$', stripped)

        if pass_match:
            results.append((pass_match.group(1).strip(), 'passed', ''))
        elif fail_match:
            results.append((fail_match.group(1).strip(), 'failed', line))
        elif skip_match:
            results.append((skip_match.group(1).strip(), 'skipped', ''))
        else:
            # Format 2: "TEST: description ... PASS" or "... FAIL"
            test_pass = re.match(r'TEST:\s*(.+?)\s*\.\.\.\s*PASS\s*$', stripped)
            test_fail = re.match(r'TEST:\s*(.+?)\s*\.\.\.\s*FAIL\s*$', stripped)
            if test_pass:
                results.append((test_pass.group(1).strip(), 'passed', ''))
            elif test_fail:
                results.append((test_fail.group(1).strip(), 'failed', line))
    return results


def build_junit_xml(suite_name: str, test_results: list,
                    stdout: str, stderr: str, elapsed: float):
    """Build a JUnit XML ElementTree from test results."""
    ts = ET.Element('testsuite')
    ts.set('name', suite_name)
    ts.set('tests', str(len(test_results)))
    ts.set('errors', '0')
    ts.set('failures', str(sum(1 for _, s, _ in test_results if s == 'failed')))
    ts.set('skipped', str(sum(1 for _, s, _ in test_results if s == 'skipped')))
    ts.set('time', f'{elapsed:.3f}')

    for name, status, details in test_results:
        tc = ET.SubElement(ts, 'testcase')
        tc.set('name', name)
        tc.set('classname', suite_name)
        tc.set('time', '0')

        if status == 'failed':
            fail_elem = ET.SubElement(tc, 'failure')
            fail_elem.set('message', 'Test failed')
            fail_elem.text = escape(details or 'FAIL')
        elif status == 'skipped':
            ET.SubElement(tc, 'skipped')

    # If no parsed results found, create a single test case from whole run
    if not test_results:
        tc = ET.SubElement(ts, 'testcase')
        tc.set('name', f'{suite_name} run')
        tc.set('classname', suite_name)
        tc.set('time', f'{elapsed:.3f}')
        if stdout or stderr:
            sys_out = ET.SubElement(tc, 'system-out')
            sys_out.text = escape(stdout[:10000])
            if stderr:
                sys_err = ET.SubElement(tc, 'system-err')
                sys_err.text = escape(stderr[:5000])
        else:
            # Empty run — treat as passed
            pass

    return ET.ElementTree(ts)


def main():
    if len(sys.argv) < 3:
        print(f'Usage: {sys.argv[0]} <suite_name> <test_binary> [args...]',
              file=sys.stderr)
        sys.exit(2)

    suite_name = sys.argv[1]
    binary = sys.argv[2]
    args = sys.argv[3:]

    junit_dir = os.environ.get('JUNIT_DIR', '.')
    verbose = os.environ.get('JUNIT_VERBOSE', '0') == '1'

    import time
    start = time.monotonic()

    try:
        result = subprocess.run(
            [binary] + args,
            capture_output=True,
            text=True,
            timeout=300,
        )
        stdout = result.stdout
        stderr = result.stderr
        exit_code = result.returncode
    except subprocess.TimeoutExpired:
        stdout = 'TIMEOUT'
        stderr = 'Test exceeded 300s timeout'
        exit_code = 124
    except FileNotFoundError:
        stdout = ''
        stderr = f'Binary not found: {binary}'
        exit_code = 127

    elapsed = time.monotonic() - start

    test_results = parse_test_output(stdout)

    # If exit code is non-zero but no FAIL lines parsed, add a synthetic failure
    if exit_code != 0 and not any(s == 'failed' for _, s, _ in test_results):
        test_results.append(('(exit code %d)' % exit_code, 'failed', stderr[:500]))

    # If no results at all and exit code 0, add a pass
    if not test_results and exit_code == 0:
        test_results.append((f'{suite_name} completed', 'passed', ''))

    tree = build_junit_xml(suite_name, test_results, stdout, stderr, elapsed)

    output_path = os.path.join(junit_dir, f'{suite_name}-results.xml')
    tree.write(output_path, xml_declaration=True, encoding='UTF-8')

    if verbose:
        print(f'--- JUnit XML ({output_path}) ---')
        print(ET.tostring(tree.getroot(), encoding='unicode'))

    # Print summary
    passed = sum(1 for _, s, _ in test_results if s == 'passed')
    failed = sum(1 for _, s, _ in test_results if s == 'failed')
    skipped = sum(1 for _, s, _ in test_results if s == 'skipped')
    total = len(test_results)
    print(f'[junit] {suite_name}: {total} tests, {passed} passed, '
          f'{failed} failed, {skipped} skipped '
          f'(exit={exit_code}, {elapsed:.1f}s)')
    print(f'[junit] XML: {output_path}')

    sys.exit(1 if failed > 0 else exit_code)


if __name__ == '__main__':
    main()
