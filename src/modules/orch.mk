# ── Orchestration modules — obj-m entries for orch/ .ko files ──────────────
#
# This file is included by src/modules/Makefile.modules.
# All entries use obj-m += to append to the global module list.

obj-m += orch/metrics.ko
obj-m += orch/log_shipper.ko
obj-m += orch/log_aggregator.ko
obj-m += orch/events.ko
obj-m += orch/tracing.ko
obj-m += orch/dashboard.ko
obj-m += orch/alerting.ko
obj-m += orch/manifest.ko
obj-m += orch/compose.ko
obj-m += orch/namespace.ko
obj-m += orch/rbac.ko
obj-m += orch/auth.ko
obj-m += orch/pod_security.ko
obj-m += orch/secrets.ko
obj-m += orch/pod_health.ko
obj-m += orch/hooks.ko
