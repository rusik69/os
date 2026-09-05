# ── tests.mk — Test targets (included from top-level Makefile) ───────
# Self-contained test/verification targets. Included by the main Makefile.

# Host-side unit tests (compiled with host gcc, no kernel deps)
unit-test:
	@echo "=== Host-side unit tests ==="
	$(MAKE) -C tests/host_libc all
	$(MAKE) -C tests/unit all

# JUnit XML test reporting (for CI)
# Usage: make junit-test JUNIT_DIR=build/test-reports
junit-test:
	@echo "=== Host-side unit tests (JUnit XML output) ==="
	mkdir -p $(JUNIT_DIR)
	$(MAKE) -C tests/host_libc all JUNIT_DIR=$(JUNIT_DIR)
	$(MAKE) -C tests/unit all JUNIT_DIR=$(JUNIT_DIR)
	@echo "JUnit XML reports in $(JUNIT_DIR)/"

# Full clean rebuild + test
test-clean: clean
	$(MAKE) test

# Test with code coverage
test-coverage: CFLAGS += -fprofile-arcs -ftest-coverage --coverage
test-coverage: LDFLAGS += --coverage
test-coverage: clean
	@echo "=== Building with code coverage (-fprofile-arcs -ftest-coverage) ==="
	$(MAKE) -j$(NPROCS) test-kernel
	@echo ""
	@echo "=== Running tests with coverage instrumentation ==="
	$(MAKE) unit-test
	@echo ""
	@echo "=== Coverage data written to build_test/ directory ==="
	@echo "Run: gcov -o build_test/ src/kernel/*.c  (per-file coverage)"
	@echo "Or:  lcov -c -d build_test/ -o coverage.info && genhtml coverage.info -o coverage/"

# E2E tests: boot normal kernel in QEMU with user-mode networking + telnet hostfwd
e2e: $(BUILDDIR)/disk.img
	$(MAKE) -j$(NPROCS) $(BUILDDIR)/kernel.bin
	@chmod +x tests/e2e.sh tests/e2e.py
	@./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# E2E smoke test: fast CI subset of e2e tests
e2e-smoke: $(BUILDDIR)/disk.img
	$(MAKE) -j$(NPROCS) $(BUILDDIR)/kernel.bin
	@chmod +x tests/e2e.sh tests/e2e.py
	@E2E_SMOKE=1 ./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# Fast run: build test-kernel and run tests in one invocation
run-test:
	$(MAKE) test

# Fast pre-merge verification: format check + static analysis + app boundary check
verify:
	$(MAKE) format-check
	$(MAKE) lint
	$(MAKE) check-app-boundary

# Verify doom framebuffer (PCI BAR0) renders non-black pixels in QEMU -vga std
doom-test: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@chmod +x tests/doom_fb.sh
	@./tests/doom_fb.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# E2E with explicit telnet port (override default 2323)
e2e-port-%: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@chmod +x tests/e2e.sh tests/e2e.py
	@E2E_PORT=$* ./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img