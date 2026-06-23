# Memory Subsystem

**Path:** `src/memory/`
**Headers:** `src/include/` (vmm.h, pmm.h, slab.h, heap.h, mglru.h, etc.)

The memory subsystem provides the full memory management stack: physical page
allocation, virtual address space management, kernel heap and slab allocators,
transparent huge pages, memory compaction, KSM, zram/zswap compressed swap,
and access monitoring (DAMON).

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                    Virtual Memory (VMM)                   │
│  src/memory/vmm.c                                        │
│  4-level page tables (PML4→PDPT→PD→PT)                  │
│  Demand paging, COW fork, NX/SMEP, THP, MMIO mappings   │
│  TLB shootdown via IPI on SMP                            │
├──────────────────────────────────────────────────────────┤
│              Physical Memory Manager (PMM)               │
│  src/memory/pmm.c                                        │
│  Bitmap allocator, per-CPU hot caches, refcounting       │
│  Contiguous allocation (order>0), memhotplug, OOM-safe   │
├──────────────────────────────────────────────────────────┤
│        Kernel Heap / Slab / Page Pool / CMA              │
│  heap.c  — first-fit block allocator (64MB max)          │
│  slab.c  — object cache (kmem_cache), per-CPU freelist   │
│  cma.c   — contiguous memory allocator for drivers       │
│  page_pool.c  — page caches for NIC buffer allocation    │
├──────────────────────────────────────────────────────────┤
│        Huge Pages & Compaction                           │
│  thp.c   — Transparent Huge Pages + khugepaged           │
│  hugetlb.c  — HugeTLB pool (pre-allocated 2MB pages)     │
│  hugepage_migration.c  — migrate THP without splitting   │
│  compaction.c  — defrag physical memory for order>0      │
├──────────────────────────────────────────────────────────┤
│        Page Reclaim & Merging                            │
│  mglru.c   — Multi-Gen LRU page reclaim                  │
│  ksm.c     — Kernel Same-page Merging (de-duplication)   │
│  page_poison.c  — page poisoning for debug               │
├──────────────────────────────────────────────────────────┤
│        Compressed Swap & ZRAM                            │
│  zram.c          — compressed RAM block device           │
│  zram_writeback.c  — writeback from zram to backing disk │
│  zswap.c         — compressed in-memory swap cache       │
│  zsmalloc.c      — allocator for compressed pages        │
│  zbud.c          — buddy allocator for zram              │
│  zcomp.c         — compression backend framework         │
│  zcomp_fast.c    — fast (LZSS) compression backend       │
├──────────────────────────────────────────────────────────┤
│        Monitoring & Debug                                │
│  damon.c    — Data Access Monitor (access pattern)       │
│  page_owner.c — page allocation tracking for debug       │
│  memhotplug.c — physical memory hotplug support          │
│  numa_balancing.c — NUMA locality fault detection+migrate│
└──────────────────────────────────────────────────────────┘
```

## File Descriptions

| File | Description |
|------|-------------|
| `pmm.c` | Physical Memory Manager — bitmap allocator, per-CPU hot page caches, page reference counting for COW, contiguous allocation with compaction fallback, memory hotplug, OOM-safe return |
| `vmm.c` | Virtual Memory Manager — 4-level page table management, map/unmap single or huge pages, demand paging, COW fork (vmm_clone_pml4), NX/SMEP enforcement, TLB shootdown, MMIO mapping for drivers |
| `heap.c` | Kernel heap — first-fit block allocator in high-half VMA, 64 MB maximum size, grows by requesting frames from PMM, kmalloc/kfree wrappers |
| `slab.c` | Slab allocator — O(1) fixed-size object cache with per-CPU freelist, slab states (full/partial/free), object poisoning, redzone overflow detection, kmem_cache_create/destroy |
| `page_poison.c` | Page poisoning — fills freed pages with a poison pattern to detect use-after-free, configurable at boot |
| `compaction.c` | Physical memory compaction — relocates pages from MIGRATE_MOVABLE pageblocks to coalesce free regions into larger contiguous blocks for order>0 allocations |
| `thp.c` | Transparent Huge Pages — khugepaged daemon that scans and promotes eligible memory regions to 2MB pages, PTE/PMD management for THP |
| `hugetlb.c` | HugeTLB pool — pre-allocates 2MB physically-contiguous huge pages from CMA for MAP_HUGETLB mmap() requests |
| `hugepage_migration.c` | THP migration — moves transparent huge pages between physical locations using PMD-level migration entries without splitting the huge page |
| `ksm.c` | Kernel Same-page Merging — incremental page scanning with hash-based dedup, scan pacing, NUMA-aware merging, memory pressure throttling |
| `mglru.c` | Multi-Gen LRU page reclaim — alternative reclaim algorithm using multiple generation lists instead of a single LRU, reduces reclaim overhead, improved OOM behavior |
| `cma.c` | Contiguous Memory Allocator — reserves physically contiguous memory regions for DMA and device driver use |
| `page_pool.c` | Page pool — pre-allocated page caches for fast NIC driver buffer allocation, NAPI-compatible, DMA address caching |
| `zram.c` | Compressed RAM block device — multi-stream compression (fast/LZSS/none), per-CPU streams for lock-free concurrent compression |
| `zram_writeback.c` | ZRAM writeback — evicts compressed pages from zram to a backing disk when memory pressure is high |
| `zswap.c` | Compressed in-memory swap cache — compresses swapped-out pages, stores in memory pool, avoids disk I/O on swap-in |
| `zsmalloc.c` | Zsmalloc allocator — memory allocator designed for compressed pages, high density, low fragmentation |
| `zbud.c` | Zbud allocator — buddy allocator for zram/zswap compressed page storage |
| `zcomp.c` | Compression backend framework — pluggable compression algorithm interface |
| `zcomp_fast.c` | Fast compression backend — LZSS-based compression for zram |
| `damon.c` | Data Access Monitor (DAMON) — monitors memory access patterns for proactive reclaim and migration decisions |
| `page_owner.c` | Page owner tracking — records allocation stack traces for each page frame, aids memory leak debugging |
| `memhotplug.c` | Memory hotplug — runtime addition/removal of physical memory regions, ACPI-based memory device enumeration |
| `numa_balancing.c` | NUMA balancing — periodic page table scanning to detect NUMA locality faults, automatic page migration to accessing node |

## Key Conventions

- **PMM bitmap:** One bit per 4 KB page. Bit 0 = page 0, etc. CPU-local hot
  caches (N pages per CPU) provide lockless fast allocation.
- **VMM layout:** Kernel mapped at `PHYS_TO_VIRT(phys) = phys + 0xFFFF800000000000`.
  PML4 entry 256 maps the kernel; each process gets its own PML4 with shared
  kernel entries.
- **Heap:** Grows from `_kernel_end` upward. Maximum size 64 MB (configurable
  via `HEAP_MAX_SIZE`). First-fit strategy with coalescing on free.
- **Slab:** Per-CPU cache array (`SLAB_CPU_CACHE_SIZE = 8` objects) avoids
  lock contention. Random freelist order for heap exploit mitigation.
- **THP:** khugepaged runs periodically scanning process address spaces for
  eligible regions to promote to 2MB pages. Controlled via sysfs.
- **zram/zswap:** Per-CPU compression streams prevent lock contention.
  Multiple compression algorithms selectable per-device.
