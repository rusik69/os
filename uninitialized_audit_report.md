# -Wmaybe-uninitialized Audit Report

## Summary

Total files audited: 22
Total files with potential issues: 3 (all borderline/false-positives)
Total real bugs found: 0

---

## Detailed Findings

### 1. `src/drivers/acpi_aml.c`
**Status: No bugs found**

- `aml_process_object()`: Variables declared at top. Error paths return 0 directly without using uninitialized locals. No goto that accesses potentially uninit data.
- `aml_exec_store()`: Has `done:` label. `ret` initialized to -1. `operand` and `val` carefully initialized before use. `done:` only checks/frees `operand` with NULL guard.
- `aml_exec_arithmetic()`: Has `error:` label. Only sets `ctx->error = 1; return 0;`. No uninitialized variable concern.

### 2. `src/drivers/drm/drm_atomic.c`
**Status: No bugs found**

- `drm_atomic_check()`: `out:` label. `int ret = atomic_parse_request(...)` — ret always set before `goto out;`.
- `drm_atomic_commit()`: `out:` label. Same pattern — ret always set before `goto out;`.

### 3. `src/drivers/drm/drm_gem.c`
**Status: No bugs found**

- `drm_gem_flink()`: `out:` label. `int ret = 0;` initialized.
- `drm_gem_open()`: `out:` label. `int ret = 0;` initialized.

### 4. `src/drivers/drm/drm_irq.c`
**Status: No bugs found**

- `drm_vblank_get()`: `out:` label. `int ret = 0;` initialized.

### 5. `src/drivers/igb.c`
**Status: No bugs found**

- `igb_init_hw()`: Has `err_teardown_tx:` and `err_teardown_rx:` labels. 
  - `int ret;` — ret is always set by `igb_reset_hw()` / `igb_wait_eeprom()` / `igb_setup_tx_ring()` / `igb_setup_rx_ring()` before any goto.
  - `pool_phys` and `pool_virt` are stored in `priv->buf_pool_phys/virt` before any TX/RX loop starts, so the cleanup code at the err labels correctly uses `priv->buf_pool_phys`.
- `igb_receive_ring()`: `skip_packet:` label. `pkt_len = 0;` at declaration.

### 6. `src/drivers/sound_oss.c`
**Status: No bugs found**

- `oss_ensure_streams()`: `out:` label. `int ret = 0;` initialized.

### 7. `src/drivers/tpm_tis.c`
**Status: No bugs found**

- `tpm_transmit()`: Has `cancel:`, `release:`, `out:` labels. `int ret = -1;` initialized at declaration.

### 8. `src/drivers/usb_cdc_acm.c`
**Status: No bugs found**

- `ehci_do_transfer()`: `fail:` label. `old_async` and `old_cmd` set before any `goto fail;`.

### 9. `src/drivers/usb_ehci.c`
**Status: No real bugs found (1 borderline false positive)**

- `ehci_pool_init()`: Has `fail_qtd:` and `fail_qh:` labels.
  - **Borderline case**: `last_qtd` initialized to 0 at line 603. If the first qTD allocation fails (`i=0`), `last_qtd` stays 0, but no qTD was ever successfully allocated. The `fail_qtd:` loop iterates `j=0..0`, calling `pmm_free_frame(g_qtd_pool[0].phys)`. Since `g_qtd_pool` is a static (BSS) array, it's zero-initialized, so `phys=0`. `pmm_free_frame(0)` is typically a no-op. **Verdict: False positive** — safe due to BSS zeroing; would be a real bug if auto storage.
  - All other paths correctly set `g_qh_pool_count` only on success, and `last_qtd` tracks the last successfully allocated index.
- `ehci_sync_submit()`: `cleanup:` label. `int ret;` declared but always set before `goto cleanup;` (lines 1577, 1584, 1587, 1599).
- `ehci_interrupt_transfer()`: `cleanup:` label. `int ret;` — always set before `goto cleanup;`.
- `ehci_iso_pool_create()`: `fail:` label. Pool is memset to 0 before allocation loop. Cleanup checks `if (b->virt)` and `if (b->itd_phys)` before freeing — safe.

### 10. `src/drivers/usb_hid.c`
**Status: No bugs found**

- `ehci_do_transfer()`: `fail:` label. `old_cmd` and `old_async` set before any `goto fail;`.

### 11. `src/drivers/usb_hub.c`
**Status: No bugs found**

- `ehci_do_transfer()`: `fail:` label. Same safe pattern as usb_hid.c/usb_msc.c.

### 12. `src/drivers/usb_msc.c`
**Status: No bugs found**

- `ehci_do_transfer()`: `fail:` label. Same safe pattern.

### 13. `src/drivers/usb_uas.c`
**Status: No bugs found**

- `ehci_do_transfer()`: `fail:` label. Same safe pattern.

### 14. `src/fs/ext2.c`
**Status: No bugs found**

- `ext2_symlink()`: Has `err_free_block:` and `err_free_inode:` labels.
  - **False positive candidate**: At `err_free_block:`, `sym_inode.i_block[0]` is read. `sym_inode` is fully memset to 0 at line 2061 before any code path reaches the goto. The guard `target_len > sizeof(sym_inode.i_block)` also ensures the body only runs for the slow symlink path. **Verdict: False positive**.

### 15. `src/fs/ext4.c`
**Status: No bugs found**

- `ext4_mount()`: Has `fail:` label. Uses `ep->bgd_cache` (checked for NULL) and `ep` (kmalloc'd + memset at top). All correct.

### 16. `src/fs/jbd2.c`
**Status: No bugs found**

- `jbd2_commit_transaction()`: Has `out_err:` label.
  - Many variables declared at function start (lines 607-621). The `out_err:` handler only uses `handle`, `i`, and `ret`.
  - `handle` is validated at line 623 before inner code runs.
  - `i` is re-initialized to 0 in the for-loop at `out_err:`.
  - `ret` is always set before `goto out_err;`.
  - Variables like `journal_pos`, `data_pos`, `start_pos`, `desc_blocks`, `commit_buf`, `commit_hdr` are never read at `out_err:`, so their initialization state is irrelevant.
  - **Verdict: Safe** — the compiler might warn about some of these, but none are actually used uninitialized.

- `jbd2_replay_blocks()`: `skip_block:` label — just a loop increment. No variable issue.

### 17. `src/fs/luks.c`
**Status: No bugs found**

- `luks_open_keyslot()`: `out:` label. `derived_key` and `key_material` are checked for NULL before free. Both are NULL-initialized via kmalloc return (if kmalloc fails, they remain NULL; goto out is taken). Safe.

### 18. `src/kernel/cpu.c`
**Status: No bugs found**

- `cpuhp_bring_cpu()`: `out:` label. `int ret = CPUHP_OK;` initialized.
- `cpuhp_take_cpu_offline()`: `out:` label. `int ret = CPUHP_OK;` initialized.

### 19. `src/kernel/elf.c`
**Status: No bugs found**

- `elf_load()`: No goto labels.
- `elf_exec()`: No goto labels (uses early return).
- `process_execve()`: Has `fail_cleanup:` label. Uses `new_pml4` and `buf`, both allocated before the `goto fail_cleanup;`. Safe.

### 20. `src/net/smtp.c`
**Status: No bugs found**

- `smtp_connect_and_send()`: `out:` label. `int ret = -1;` initialized. Uses `conn_id` which is always set before `goto out;`. Safe.

### 21. `src/net/tls_handshake.c`
**Status: No bugs found**

- `tls_handshake_step()`: Has `error:` label. Only sets `*new_state = TLS_HS_ERROR; return -EPROTO;`. No variable access issues.

### 22. `src/compiler/cc_link.c`
**Status: No bugs found**

- `cc_link()`: Has `fail:` and `fail_alloc:` labels.
  - At `fail_alloc:` (from line 143): `merged_text`, `merged_data`, `syms`, `objs` are checked for NULL before freeing. This code runs when one of the 4 initial allocations fails — only the ones that succeeded are non-NULL. Safe.
  - At `fail:` (from within the loop): All 4 allocations were successful (since we passed line 141). The cleanup checks for NULL anyway. `file_buf` is either NULL (if alloc failed and we goto fail) or was freed before goto fail. Safe.

---

## Total Real Bugs: 0

### Borderline Cases (all false positives):
1. **`usb_ehci.c:ehci_pool_init()`** — `last_qtd` used in `fail_qtd:` cleanup loop at index 0 when no qTD slots were ever allocated. Safe because the pool is in BSS (zero-initialized), making `pmm_free_frame(0)` a no-op. Would be a real bug if pool were auto-storage.
2. **`ext2.c:ext2_symlink()`** — `sym_inode.i_block[0]` read at `err_free_block:`, but `sym_inode` was memset to 0 before any goto. Not actually uninitialized.
3. **`jbd2.c:jbd2_commit_transaction()`** — Several variables (`journal_pos`, `data_pos`, `start_pos`, `desc_blocks`, `commit_buf`, `commit_hdr`) could reach `out_err:` without being initialized, but none are actually read there.
