#!/bin/bash
#
# scripts/run_kunit.sh — KUnit test runner for Hermes OS
#
# Parses KUnit output from kernel boot logs and reports pass/fail per suite.
# Can read from a serial log file or from stdin.
#
# Usage:
#   ./scripts/run_kunit.sh [--log serial.log] [--quiet]
#   cat serial.log | ./scripts/run_kunit.sh
#
# The script looks for KUnit output lines of the form:
#   [KUNIT] SUITE: <name> (<N> cases)
#   [KUNIT] RUN:   <suite>.<test>
#   [KUNIT] PASS:  <suite>.<test>
#   [KUNIT] FAIL:  <suite>.<test> (<N> failures)
#   [KUNIT] Results: <N> passed, <N> failed, <N> assertions (<N> failures)
#
# Exit code: number of failed tests (0 = all passed)
#
# Requires: bash, grep, awk, sort, uniq

set -euo pipefail

QUIET=0
LOG_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --log) LOG_FILE="$2"; shift 2 ;;
        --quiet) QUIET=1; shift ;;
        --help|-h)
            echo "Usage: $0 [--log serial.log] [--quiet]"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

TMPDIR=$(mktemp -d /tmp/kunit_XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

RAW_LOG="$TMPDIR/raw.log"
RESULTS_LOG="$TMPDIR/results.log"

# Read input
if [[ -n "$LOG_FILE" ]]; then
    if [[ ! -f "$LOG_FILE" ]]; then
        echo "ERROR: Log file not found: $LOG_FILE" >&2
        exit 1
    fi
    cp "$LOG_FILE" "$RAW_LOG"
else
    cat > "$RAW_LOG"
fi

# Extract KUnit lines
grep -E '^\[KUNIT\]' "$RAW_LOG" > "$RESULTS_LOG" || true

if [[ ! -s "$RESULTS_LOG" ]]; then
    echo "No KUnit output found in log." >&2
    exit 0
fi

# ── Parse results ──────────────────────────────────────────────────────

declare -A SUITE_CASES    # suite -> total cases
declare -A SUITE_PASSED   # suite -> passed tests
declare -A SUITE_FAILED   # suite -> failed tests
declare -A SUITE_FAILURES # suite -> failure messages

TOTAL_PASSED=0
TOTAL_FAILED=0
TOTAL_ASSERTIONS=0
TOTAL_ASSERT_FAILS=0
CURRENT_SUITE=""

while IFS= read -r line; do
    # Suite header
    if [[ "$line" =~ ^\[KUNIT\]\ SUITE:\ (.+)\ \(([0-9]+)\ cases\)$ ]]; then
        CURRENT_SUITE="${BASH_REMATCH[1]}"
        SUITE_CASES["$CURRENT_SUITE"]="${BASH_REMATCH[2]}"
        SUITE_PASSED["$CURRENT_SUITE"]=0
        SUITE_FAILED["$CURRENT_SUITE"]=0
        SUITE_FAILURES["$CURRENT_SUITE"]=""

    # Test run
    elif [[ "$line" =~ ^\[KUNIT\]\ RUN:\ +(.+)\.(.+)$ ]]; then
        : # No-op, just tracking

    # Test pass
    elif [[ "$line" =~ ^\[KUNIT\]\ PASS:\ +(.+)\.(.+)$ ]]; then
        suite="${BASH_REMATCH[1]}"
        if [[ -n "$suite" ]]; then
            SUITE_PASSED["$suite"]=$((SUITE_PASSED["$suite"] + 1))
            TOTAL_PASSED=$((TOTAL_PASSED + 1))
        fi

    # Test fail
    elif [[ "$line" =~ ^\[KUNIT\]\ FAIL:\ +(.+)\.(.+)$ ]]; then
        suite="${BASH_REMATCH[1]}"
        testname="${BASH_REMATCH[2]}"
        if [[ -n "$suite" ]]; then
            SUITE_FAILED["$suite"]=$((SUITE_FAILED["$suite"] + 1))
            TOTAL_FAILED=$((TOTAL_FAILED + 1))
            SUITE_FAILURES["$suite"]+="    FAIL: ${testname}${line#*FAIL: }"$'\n'
        fi

    # Summary line
    elif [[ "$line" =~ ^\[KUNIT\]\ Results:\ ([0-9]+)\ passed,\ ([0-9]+)\ failed, ]]; then
        TOTAL_PASSED="${BASH_REMATCH[1]}"
        TOTAL_FAILED="${BASH_REMATCH[2]}"
        # Also parse assertions if present
        if [[ "$line" =~ ([0-9]+)\ assertions\ \(([0-9]+)\ failures\)$ ]]; then
            TOTAL_ASSERTIONS="${BASH_REMATCH[1]}"
            TOTAL_ASSERT_FAILS="${BASH_REMATCH[2]}"
        fi
    fi
done < "$RESULTS_LOG"

# ── Print report ────────────────────────────────────────────────────────

if [[ "$QUIET" -eq 0 ]]; then
    echo "============================================"
    echo "  KUnit Test Results"
    echo "============================================"
    echo ""

    # Print per-suite results
    for suite in "${!SUITE_CASES[@]}"; do
        passed="${SUITE_PASSED[$suite]:-0}"
        failed="${SUITE_FAILED[$suite]:-0}"
        total=$((passed + failed))
        expected="${SUITE_CASES[$suite]:-0}"

        if [[ "$failed" -gt 0 ]]; then
            echo "  ❌ $suite: ${passed}/${total} passed (${failed} failed)"
            echo -n "${SUITE_FAILURES[$suite]}"
        else
            echo "  ✅ $suite: ${passed}/${total} passed"
        fi
    done

    echo ""
    echo "--------------------------------------------"
    echo "  Total:  ${TOTAL_PASSED} passed, ${TOTAL_FAILED} failed"
    if [[ "$TOTAL_ASSERTIONS" -gt 0 ]]; then
        echo "  Assertions: ${TOTAL_ASSERTIONS} total, ${TOTAL_ASSERT_FAILS} failures"
    fi
    echo "--------------------------------------------"
fi

exit "$TOTAL_FAILED"
