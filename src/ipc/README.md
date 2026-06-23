# IPC — Inter-process communication: pipes, signals, shared memory, message queues, futexes

Provides kernel-level IPC primitives: pipes (anonymous and named FIFOs), shared memory segments, POSIX message queues, semaphores, mutexes, eventfd, signalfd, timerfd, inotify, and wait queues.

## Key Files

- **pipe.c** — Anonymous pipes with double-buffering, non-blocking I/O, and wait-queue-based blocking reads/writes.
- **fifo.c** — Named pipes (FIFOs) identified by filesystem path, backed by pipe buffer infrastructure.
- **shm.c** — Shared memory segments (16 max, 4 KiB each) with SysV-style permission control, page mapping via PML4 manipulation.
- **mqueue.c** — POSIX message queues with named lookup, blocking send/receive, and priority ordering.
- **semaphore.c** — Kernel counting semaphore with spin+yield implementation.
- **mutex.c** — Priority Inheritance Protocol (PIP) mutex with optimistic spinning, lockdep deadlock detection, and owner tracking.
- **waitqueue.c** — IRQ-safe wait queues for process blocking/wakeup with PID-based tracking.
- **eventfd.c** — Event notification file descriptor with counter semantics (EFD_SEMAPHORE, EFD_NONBLOCK, EFD_CLOEXEC).
- **signalfd.c** — File descriptor interface for receiving signals (alternative to signal handler callbacks).
- **timerfd.c** — Timer file descriptors: one-shot and periodic timers delivered via fd read events.
- **inotify.c** — File system event monitoring: inotify_init/add_watch/rm_watch atop the fsnotify infrastructure.

## Architecture

Each IPC primitive uses a fixed-size global table allocated at init time. Blocking operations rely on the shared waitqueue mechanism (`wait_queue_sleep` / `wait_queue_wake`). File-descriptor-based primitives (eventfd, signalfd, timerfd, inotify) expose themselves through a reserved fd range and integrate with poll/select via `POLLIN`/`POLLOUT` flags. The mutex implementation includes priority inheritance to prevent priority inversion in real-time scenarios.

## Cross-References

- **process/** — All IPC primitives interact with the process/scheduler layer for blocking and wakeup.
- **vfs/** — FIFO, signalfd, eventfd, timerfd, inotify register VFS operations for fd-based access.
- **signal/** — signalfd and signal delivery paths are shared with the core signal subsystem.
