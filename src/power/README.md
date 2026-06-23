# Power — Power management: ACPI sleep states, CPU idle, suspend/resume

Implements CPU frequency scaling (cpufreq), CPU idle state management (cpuidle), device frequency scaling (devfreq), system suspend/resume (ACPI S3 and S0ix), energy modelling, and PM QoS.

## Key Files

- **cpufreq.c** — Core CPU frequency scaling: MSR-based P-state control (Intel SpeedStep / AMD Cool'n'Quiet), sysfs interface for governors and frequency queries.
- **cpufreq_ondemand.c** — OnDemand governor: periodic APERF/MPERF sampling, up/down threshold decision, rate-limited transitions.
- **cpufreq_conservative.c** — Conservative governor: gradual 5% frequency steps up, quick reduction on load drop.
- **cpufreq_schedutil.c** — SchedUtil governor: reads PELT utilisation directly from the scheduler for instant frequency adjustment.
- **cpufreq_userspace.c** — Userspace governor: manual frequency control via scaling_setspeed sysfs file.
- **cpuidle_teo.c** — TEO (Timer Events Oriented) idle governor: selects C-states based on observed timer event patterns with sliding-window statistics.
- **cpuidle_ladder.c** — Ladder idle governor: step through C-states incrementally with promotion/demotion thresholds.
- **suspend.c** — ACPI S3 Suspend-to-RAM: save/restore CPU state, enter via PM1a_CNT. Also S0ix via MWAIT(C1e) loop.
- **suspend_s2idle.c** — Suspend-to-Idle (s2idle): shallowest sleep state, CPUs enter deep idle, wakeup via IRQ-based detection.
- **wakeup.c** — Wakeup event source tracking: registers devices with wakeup capability, coordinates suspend deferral during active events.
- **devfreq.c** — Device frequency scaling framework: governors adjust device frequencies based on utilisation; sysfs interface at `/sys/class/devfreq/`.
- **energy_model.c** — Energy Model for Energy-Aware Scheduling (EAS): per-CPU power cost tables, dynamic/static power computation.
- **pm_qos.c** — PM Quality of Service: latency constraints from kernel components influence cpuidle C-state selection (global and per-device).
- **rapl.c** — Running Average Power Limit: reads RAPL MSRs for PKG/DRAM/PP0 energy/power information.

## Architecture

Plugin-based governor architecture for both cpufreq and cpuidle: a core framework exposes registration APIs, and individual governors (ondemand, conservative, schedutil, userspace; TEO, ladder) are selectable at runtime. The suspend subsystem implements a save/restore cycle with ACPI PM register programming. PM QoS acts as a cross-cutting constraint system that cpuidle consults before entering deep C-states. Energy modelling provides scheduler integration for power-aware task placement.

## Cross-References

- **boot/** — ACPI table walking at boot time provides the foundation for ACPI sleep state support.
- **scheduler/** — SchedUtil governor and energy model integrate with the CPU scheduler for power-aware decisions.
- **device/** — Device frequency scaling interacts with individual device drivers.
