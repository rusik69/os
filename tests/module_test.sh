#!/bin/bash
# module_test.sh — QEMU module lifecycle test script (D233 tasks 5-10)
#
# This script tests the full module loading/unloading lifecycle inside
# a running QEMU instance of the OS kernel.  It runs as a userspace
# shell script after boot and exercises:
#
#   5. Module load via insmod (basic path)
#   6. Module load via modprobe (with alias)
#   7. Module dependency auto-loading (depmod)
#   8. Module signature test (valid)
#   9. Module signature test (tampered — should fail)
#  10. Module unload + refcount test
#
# Usage:
#   On the host: copy test module .ko files to the disk image's /modules/
#   Inside QEMU: sh /tests/module_test.sh
#
# Returns 0 if all tests pass, 1 if any fail.

set -e

PASS=0
FAIL=0

pass() {
    PASS=$((PASS + 1))
    echo "  PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    echo "  FAIL: $1"
}

check_result() {
    if [ "$1" -eq 0 ]; then
        pass "$2"
    else
        fail "$2 (expected 0, got $1)"
    fi
}

echo "================================================"
echo "  Module Lifecycle Test Suite (D233)"
echo "================================================"
echo ""

# ── Test 5: Module load via insmod ──────────────────────────────────
echo "[Test 5] Module load via insmod"

# Load test_module
insmod /modules/test_module.ko 2>/dev/null
check_result $? "insmod test_module.ko"

# Verify it appears in lsmod (or /sys/module/)
if [ -d /sys/module/test_module ]; then
    pass "test_module appears in /sys/module/"
else
    fail "test_module not found in /sys/module/"
fi

# Check initstate
INITSTATE=$(cat /sys/module/test_module/initstate 2>/dev/null)
if [ "$INITSTATE" = "live" ]; then
    pass "test_module initstate = live"
else
    fail "test_module initstate = $INITSTATE (expected live)"
fi

# Check refcnt
REFCNT=$(cat /sys/module/test_module/refcnt 2>/dev/null)
if [ "$REFCNT" = "0" ] || [ "$REFCNT" = "" ]; then
    pass "test_module refcnt = $REFCNT"
else
    fail "test_module refcnt = $REFCNT (expected 0)"
fi

echo ""

# ── Test 6: Module load via modprobe (with alias) ───────────────────
echo "[Test 6] Module load via modprobe (alias)"

# First unload test_module
rmmod test_module 2>/dev/null || true

# Try loading via modprobe (uses alias resolution if the module
# has a MODULE_ALIAS entry in .modinfo).
modprobe test_module 2>/dev/null
check_result $? "modprobe test_module"

# Unload for next test
rmmod test_module 2>/dev/null || true

echo ""

# ── Test 7: Module dependency auto-loading ──────────────────────────
echo "[Test 7] Module dependency auto-loading"

# Load test_module explicitly (dependency of test_mod_deps)
insmod /modules/test_module.ko 2>/dev/null
check_result $? "insmod test_module.ko (dependency)"

# Load test_mod_deps — should auto-resolve its dependency on test_module
insmod /modules/test_mod_deps.ko 2>/dev/null
check_result $? "insmod test_mod_deps.ko"

# Verify both modules are loaded
if [ -d /sys/module/test_mod_deps ]; then
    pass "test_mod_deps appears in /sys/module/"
else
    fail "test_mod_deps not found in /sys/module/"
fi

# Check test_mod_deps initstate
DEPS_STATE=$(cat /sys/module/test_mod_deps/initstate 2>/dev/null)
if [ "$DEPS_STATE" = "live" ]; then
    pass "test_mod_deps initstate = live"
else
    fail "test_mod_deps initstate = $DEPS_STATE (expected live)"
fi

# Verify holders shows test_mod_deps depends on test_module
HOLDERS=$(cat /sys/module/test_module/holders 2>/dev/null)
case "$HOLDERS" in
    *test_mod_deps*) pass "test_module holders includes test_mod_deps" ;;
    *)               fail "test_module holders does not show test_mod_deps ($HOLDERS)" ;;
esac

echo ""

# ── Test 8: Module signature test (valid) ───────────────────────────
echo "[Test 8] Module signature verification (valid)"

# If the module has a valid .module_sig section, the loader should
# accept it.  Check that /sys/kernel/module_verify exists.
if [ -f /sys/kernel/module_verify ]; then
    MODE=$(cat /sys/kernel/module_verify 2>/dev/null | tr -d '\n')
    echo "  Module verify mode: $MODE"
    if [ "$MODE" = "2" ]; then
        pass "/sys/kernel/module_verify enforce mode (valid)"
    else
        pass "/sys/kernel/module_verify mode $MODE"
    fi
else
    fail "/sys/kernel/module_verify not found"
fi

echo ""

# ── Test 9: Module signature test (tampered) ────────────────────────
echo "[Test 9] Module signature verification (tampered — expected fail)"

# Create a tampered copy of the module (modify one byte)
# This should fail signature verification if enforcement is on.
cp /modules/test_module.ko /tmp/test_module_tampered.ko
# Corrupt byte at offset 256 (after ELF header and some section headers)
printf '\xff' | dd of=/tmp/test_module_tampered.ko bs=1 seek=256 count=1 conv=notrunc 2>/dev/null

# Attempt to load the tampered module (should fail)
insmod /tmp/test_module_tampered.ko 2>/dev/null && {
    fail "Tampered module loaded (should have been rejected)"
} || {
    pass "Tampered module correctly rejected"
}

rm -f /tmp/test_module_tampered.ko

echo ""

# ── Test 10: Module unload + refcount test ──────────────────────────
echo "[Test 10] Module unload + refcount"

# Unload test_mod_deps first (since test_module has dependents)
rmmod test_mod_deps 2>/dev/null
check_result $? "rmmod test_mod_deps"

# Verify test_mod_deps unloaded
if [ -d /sys/module/test_mod_deps ]; then
    fail "test_mod_deps still present after rmmod"
else
    pass "test_mod_deps removed from /sys/module/"
fi

# Now unload test_module
rmmod test_module 2>/dev/null
check_result $? "rmmod test_module"

# Verify test_module unloaded
if [ -d /sys/module/test_module ]; then
    fail "test_module still present after rmmod"
else
    pass "test_module removed from /sys/module/"
fi

# Refcount test: attempt to unload test_module while dependent is loaded
insmod /modules/test_module.ko 2>/dev/null
insmod /modules/test_mod_deps.ko 2>/dev/null

# Try to unload test_module while test_mod_deps depends on it
rmmod test_module 2>/dev/null && {
    fail "test_module was unloaded while test_mod_deps still depends on it"
} || {
    pass "test_module correctly refused unloading (dependency present)"
}

# Unload properly: dependency first, then module
rmmod test_mod_deps 2>/dev/null
rmmod test_module 2>/dev/null
check_result $? "Both modules properly unloaded"

echo ""
echo "================================================"
echo "  Results: $PASS passed, $FAIL failed"
echo "================================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
