# D257 — Host-Side Libc Tests: Task → Test Map

D257 (Host-Side Libc Tests, 15 changes) is implemented by the existing
`tests/host_libc/` suite, which `make unit-test` builds and runs with the
**host** gcc (no kernel/QEMU needed). All tests pass (0 failures).

The 15 plan tasks map to real, executing assertions as follows:

| # | Plan task | Function(s) | Test binary | Status |
|---|-----------|-------------|-------------|--------|
| 1 | string: memcmp/memcpy/memset (edge/alignment) | `memcmp`,`memcpy`,`memset`,`memmove` | `test_libc` | ✓ (incl. unaligned `n=4095`, overlap fwd/back) |
| 2 | string: strcmp/strcpy/strlen (all lengths) | `strcmp`,`strcpy`,`strlen` | `test_libc` | ✓ (incl. `strlen("")=0`) |
| 3 | string: strncpy/strncat/strncmp boundary | `strncpy`,`strncat`,`strncmp` | `test_libc` | ✓ (incl. `n=0`, truncation) |
| 4 | stdio: printf `%d %s %x %%` | `sprintf`/`printf` | `test_libc` | ✓ |
| 5 | stdio: sprintf overflow protection | `snprintf` | `test_libc` | ✓ (`snprintf(buf,8,...)` → ≤7 chars) |
| 6 | stdlib: atoi/strtol/strtoul | `atoi`,`strtol`,`strtoul` | `test_libc` | ✓ (base 10/16/auto, boundaries) |
| 7 | stdlib: rand_r seed determinism | `srand`/`rand` | `test_libc` | ✓ (reseed reproduces sequence) |
| 8 | time: mktime cross-month boundary | `mktime` | `test_time` | ✓ (Jan+Feb=59 days) |
| 9 | time: gmtime_r leap year | `gmtime_r` | `test_time` | ✓ (2024-12-31 leap) |
| 10 | bitmap: set/clear/find_next/find_first | `bitmap_set/clear`,`bitmap_find_next_zero_area` | `test_bitops_crc_sha` | ✓ |
| 11 | crc32 vs known-good vector | `crc32` | `test_bitops_crc_sha` | ✓ |
| 12 | sha256 vs NIST vectors | `sha256` | `test_bitops_crc_sha` | ✓ |
| 13 | aes vs NIST vectors | `aes_*` | `test_cipher` | ✓ |
| 14 | base64 encode/decode roundtrip | `base64_encode/decode` | `test_crypto_ext` | ✓ |
| 15 | uuid generate/parse/compare | `uuid_*` | `test_uuid` | ✓ |

## Run

```
make unit-test          # builds + runs tests/host_libc + tests/unit
cd tests/host_libc && make all && ./test_libc   # individual binaries
```

No new test files were required: the existing suite already covers every
D257 task with deterministic, host-runnable assertions. This document is the
only D257 addition (a traceability map).
