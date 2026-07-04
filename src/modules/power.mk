# ── Power management modules — obj-m entries for power/ .ko files ──────────
#
# This file is included by src/modules/Makefile.modules.
# All entries use obj-m += to append to the global module list.

obj-m += power/cpufreq_ondemand.ko
obj-m += power/cpufreq_conservative.ko
obj-m += power/cpufreq_userspace.ko
obj-m += power/cpufreq_schedutil.ko
obj-m += power/cpuidle_ladder.ko
obj-m += power/cpuidle_teo.ko
obj-m += power/devfreq.ko
obj-m += power/energy_model.ko
obj-m += power/rapl.ko
