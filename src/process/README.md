# Process Subsystem

**Path:** `src/process/`
**Headers:** `src/include/process.h`, `src/include/scheduler.h`, `src/include/signal.h`

The process subsystem manages process and thread lifecycle, scheduling, signal
delivery, inter-process synchronization (futex), and user/group management.
It implements a multi-class scheduler with CFS/EEVDF for normal tasks and EDF
for real-time tasks, plus SMP load balancing and NUMA-aware placement.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    System Calls                          │
│  fork/exec/exit/wait  →  process.c                      │
│  sched_yield/set_sched →  scheduler.c + sched_deadline.c│
│  kill/sigaction       →  signal.c                       │
│  futex                →  futex.c                        │
│  setpgid/getsid       →  pgrp.c                        │
│  clone                →  thread.c                       │
└─────────────────────────────────────────────────────────┘
                           │
┌──────────────────────────────────────────────────────────┐
│               Core Data Structures                        │
│  process_table[PROCESS_MAX]  (256 entries)                │
│  ├─ struct process: pid, state (UNUSED/READY/RUNNING/    │
│  │                    BLOCKED/ZOMBIE), context, stack,    │
│  │                    creds, signals, fd table, limits    │
│  ├─ per-CPU runqueues: cfs_rq, rt_rq, deadline_rq        │
│  ├─ signal pending queues (siginfo_t for RT signals)     │
│  └─ pgrp/session tables for job control                  │
└──────────────────────────────────────────────────────────┘
```

## File Descriptions

| File | Description |
|------|-------------|
| `process.c` | Core process lifecycle: fork, exec, exit, wait. PID bitmap allocator, process table management, credentials, rlimits, namespace integration |
| `scheduler.c` | SMP multi-class scheduler: CFS/EEVDF for SCHED_OTHER, priority queues for RT, idle balancing, PELT load tracking, context switch, NUMA-aware placement, preemption, PSI integration |
| `signal.c` | Signal delivery (signals 1-64), real-time queued signals with siginfo_t, SIGCHLD exit status, core dump handler, signal mask and pending set management |
| `thread.c` | Thread management: clone() implementation, TLS area setup via FS segment base, per-thread errno and signal masks, thread descriptor allocator |
| `futex.c` | Fast Userspace Mutex: futex_wait/futex_wake/requeue operations with hash-based bucketing (256 buckets), timeout support, PI futex |
| `pgrp.c` | Process group and session management: setpgid, getsid, session leaders, foreground process group tracking for job control |
| `pelt.c` | Per-Entity Load Tracking: EWMA-based CPU utilisation averages updated per tick, fixed-point representation, sleep-time decay compensation |
| `sched_deadline.c` | SCHED_DEADLINE scheduling class: EDF with CBS budget enforcement, per-CPU deadline runqueue, admission control, replenishment, throttling |
| `switch.asm` | Assembly context switch routine: saves/restores callee-saved registers (r15-r12, rbx, rbp, rflags, rip), called with interrupts disabled |

## Key Conventions

- **Process table:** Fixed-size array of 256 entries (`PROCESS_MAX`), indexed by
  PID bitmap. The idle process occupies PID 0.
- **Kernel stacks:** 128 KB per process with a guard page below for overflow
  detection. Stack pages are allocated on fork.
- **Scheduling classes evaluated in priority order:** DEADLINE → FIFO → RR →
  OTHER → IDLE. Each class has a `pick_next_task` callback.
- **Context switch:** Caller must `cli` before `context_switch()`, `sti` after
  return. The switch.asm function saves the old context and restores the new
  one, including CR3 for address space switch.
- **PELT:** Utilisation is updated per timer tick via `scheduler_tick()`. A
  halflife of 32 ticks gives ~320ms half-life at 100 Hz.
- **Signals:** Signals 1-31 are standard, 32-64 are real-time (queued with
  siginfo_t). Signal disposition supports default, ignore, handler, and
  sigaction with SA_SIGINFO.
- **User credentials:** Managed in `users.c` with a flat user/group table.
  Default users: root (uid=0), guest (uid=1000).
