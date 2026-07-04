# ── Memory subsystem modules — obj-m entries for memory/ .ko files ──────────
#
# This file is included by src/modules/Makefile.modules.
# All entries use obj-m += to append to the global module list.

obj-m += memory/zram.ko
obj-m += memory/zswap.ko
obj-m += memory/zsmalloc.ko
obj-m += memory/zbud.ko
obj-m += memory/ksm.ko
obj-m += memory/thp.ko
obj-m += memory/damon.ko
obj-m += memory/page_pool.ko
obj-m += memory/memhotplug.ko
obj-m += memory/compaction.ko
obj-m += memory/mglru.ko
obj-m += memory/hugetlb.ko
obj-m += memory/zram_writeback.ko
obj-m += memory/zcomp.ko
