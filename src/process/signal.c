/*
 * Signal handling — real-time signals + siginfo_t delivery
 *
 * Supports signals 1-64 (SIGRTMIN=32..SIGRTMAX=64).
 * Real-time signals (SIGRTMIN-SIGRTMAX) are queued with siginfo_t.
 * SIGCHLD delivers exit status via siginfo_t.
 */
#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "printf.h"
#include "syscall.h"
#include "errno.h"
#include "signal_validate.h"
#include "timer.h"
#include "coredump_core.h"
#include "signalfd.h"

#include "idt.h"            /* for struct interrupt_frame */
#include "smp.h"            /* for get_cpu_info() */
#include "vsyscall.h"       /* for VSYSCALL_PAGE_VADDR, VSYSCALL_SIGRETURN, VDSO_ENTRY_SIZE */
#include "signal_frame.h"   /* for struct rt_sigframe, ucontext_t */
#include "uaccess.h"        /* for copy_to_user */
#include "string.h"         /* for memset, memcpy */

/* Maximum signal number (signals 1-64) — bounds all signal arrays */
#define MAX_SIG  65

int signal_send(uint32_t pid, int signum) {
    if (signum == 0) {
        struct process *probe = process_get_by_pid(pid);
        return (probe && probe->state != PROCESS_UNUSED) ? 0 : -1;
    }
    if (signum <= 0 || signum >= SIG_MAX) return -1;
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    /* Acquire sig_lock to protect pending_signals/state/exit_code */
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);

    /* RLIMIT_SIGPENDING: enforce pending signal queue limit.
     * Exclude SIGKILL (always deliverable) and SIGSTOP (cannot be blocked). */
    if (signum != SIGKILL && signum != SIGSTOP) {
        uint64_t sig_limit = p->rlim_cur[RLIMIT_SIGPENDING];
        if (sig_limit != ~0ULL) {
            /* Count currently pending (non-masked) signals */
            uint64_t pending = p->pending_signals;
            /* Also count masked signals (they just hang out) */
            int count = 0;
            while (pending) { pending &= pending - 1; count++; }
            /* If at or over limit, reject the signal */
            if ((uint64_t)count >= sig_limit) {
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                return -EAGAIN;
            }
        }
    }

    /* Track resource usage */
    p->signals_received++;

    /* Permission check */
    struct process *caller = process_get_current();
    if (caller && caller->pid != p->pid) {
        if (!process_can_see(caller, p)) {
            spinlock_irqsave_release(&p->sig_lock, __sig_flags);
            return -1;
        }
    }

    if (p->state == PROCESS_ZOMBIE) {
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        return 0;
    }

    /* SIGKILL — immediate terminate */
    if (signum == SIGKILL) {
        p->state = PROCESS_ZOMBIE;
        p->exit_code = 128 + signum;
        p->is_suspended = 0;
        p->sleep_until = 0;
        scheduler_remove(p);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    /* SIGCONT — always resumes, regardless of mask */
    if (signum == SIGCONT) {
        p->pending_signals |= (1ULL << signum);
        if (p->is_suspended) {
            p->is_suspended = 0;
            p->sleep_until = 0;
            p->state = PROCESS_READY;
            scheduler_add(p);
        }
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        return 0;
    }

    /* If masked, just set pending */
    if (p->sig_mask & (1ULL << signum)) {
        p->pending_signals |= (1ULL << signum);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        return 0;
    }

    if (signum == SIGTERM) {
        p->state = PROCESS_ZOMBIE;
        p->exit_code = 128 + signum;
        p->is_suspended = 0;
        p->sleep_until = 0;
        scheduler_remove(p);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    if (signum == SIGSTOP || signum == SIGTSTP || signum == SIGTTIN || signum == SIGTTOU) {
        p->is_suspended = 1;
        p->sleep_until = 0;
        p->state = PROCESS_BLOCKED;
        scheduler_remove(p);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    p->pending_signals |= (1ULL << signum);

    /* Wake process if it's blocked in sigtimedwait for this signal */
    if (p->state == PROCESS_BLOCKED && (p->sigwait_mask & (1ULL << signum))) {
        p->sigwait_mask = 0;
        p->sleep_until = 0;
        p->state = PROCESS_READY;
        p->last_run_tick = timer_get_ticks();
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        scheduler_add(p);
        return 0;
    }

    /* Notify signalfd listeners */
    signalfd_notify(signum);

    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
    return 0;
}

/* Extended signal send with siginfo_t support.
 * Stores siginfo for delivery by signal_check (or signal fd read).
 * For real-time signals (SIGRTMIN-SIGRTMAX) the info is queued.
 * For standard signals, keeps the most recent info only.
 * Validates siginfo fields before storing (security).
 *
 * Note: This function acquires the process's sig_lock internally
 * so it is SMP-safe.  It does NOT call signal_send() separately
 * to avoid a TOCTOU race between setting the pending bit and
 * storing the siginfo.
 */
int signal_send_info(uint32_t pid, int signum, struct siginfo *info) {
    if (signum <= 0 || signum >= SIG_MAX) return -1;

    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_ZOMBIE)
        return -1;

    /* Hold the lock for the entire operation */
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);

    /* Permission check */
    struct process *caller = process_get_current();
    if (caller && caller->pid != p->pid) {
        if (!process_can_see(caller, p)) {
            spinlock_irqsave_release(&p->sig_lock, __sig_flags);
            return -1;
        }
    }

    if (signum == SIGKILL) {
        p->state = PROCESS_ZOMBIE;
        p->exit_code = 128 + signum;
        p->is_suspended = 0;
        p->sleep_until = 0;
        scheduler_remove(p);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    /* Set the pending signal bit */
    p->pending_signals |= (1ULL << signum);

    /* Store siginfo if provided */
    if (info) {
        /* Extra bounds check: signal number must be within array range */
        if (signum <= 0 || signum >= MAX_SIG) {
            spinlock_irqsave_release(&p->sig_lock, __sig_flags);
            return -1;
        }
        struct siginfo validated = *info;
        int is_from_userspace = (caller && caller->is_user) ? 1 : 0;
        signal_validate_siginfo(&validated, is_from_userspace);
        p->sig_info[signum] = validated;
    }

    /* Wake process if it's blocked in sigtimedwait for this signal */
    if (p->state == PROCESS_BLOCKED && (p->sigwait_mask & (1ULL << signum))) {
        p->sigwait_mask = 0;
        p->sleep_until = 0;
        p->state = PROCESS_READY;
        p->last_run_tick = timer_get_ticks();
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        scheduler_add(p);
        return 0;
    }

    spinlock_irqsave_release(&p->sig_lock, __sig_flags);

    /* Notify signalfd listeners */
    signalfd_notify(signum);
    if (info) {
        signalfd_notify_ext(signum, info->si_code, info->si_pid, info->si_uid,
                           (uint64_t)(uintptr_t)info->si_addr, info->si_status);
    }

    return 0;
}

int signal_send_group(uint32_t pgid, int signum) {
    if (pgid == 0) return -1;
    struct process *table = process_get_table();
    int sent = 0;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED || table[i].pgid != pgid) continue;
        if (signal_send(table[i].pid, signum) == 0) sent++;
    }
    return sent > 0 ? 0 : -1;
}

int signal_send_pgid(uint32_t pgid, int signum) {
    return signal_send_group(pgid, signum);
}

/* Retrieve the siginfo for a signal, if available.
 * Returns the siginfo pointer, or NULL if no info stored.
 * Clears the stored info after returning it (one-shot). */
struct siginfo *signal_get_info(struct process *p, int signum) {
    if (!p || signum <= 0 || signum >= SIG_MAX) return NULL;
    if (!(p->pending_signals & (1ULL << signum))) return NULL;
    /* Only return info if it was explicitly set (non-zero si_signo) */
    if (p->sig_info[signum].si_signo == signum) {
        return &p->sig_info[signum];
    }
    return NULL;
}

/* ── signal_setup_frame_userspace ───────────────────────────────────────
 * Build a signal frame on the user stack and redirect the interrupt/syscall
 * saved context to the user-space signal handler.
 *
 * Called from signal_check() (interrupt context) when a user-mode process
 * has a custom handler for a pending unblocked signal.
 *
 * @frame:   The saved interrupt frame (will be modified: rip → handler, rsp → sigframe)
 * @signum:  The signal number being delivered
 * @handler_addr:  User-space address of the signal handler
 * @info:    The siginfo_t to deliver (may be zeroed with si_signo/si_code set)
 * @saved_sigmask: The signal mask before delivery (stored in ucontext for sigreturn)
 *
 * Returns 0 on success, -errno on failure (pending bit should be restored by caller).
 */
int signal_setup_frame_userspace(struct interrupt_frame *frame, int signum,
                                 uint64_t handler_addr,
                                 struct siginfo *info,
                                 uint64_t saved_sigmask)
{
    /* Compute sigreturn trampoline address in the VDSO code page.
     * Each VDSO entry is 256 bytes. */
    uint64_t tramp_addr = VSYSCALL_PAGE_VADDR
                          + (uint64_t)VSYSCALL_SIGRETURN * 256ULL;

    /* Capture the interrupted user register state from the interrupt frame.
     * These values will be saved into the sigcontext so sigreturn can restore them. */
    uint64_t user_rip   = frame->rip;
    uint64_t user_rsp   = frame->rsp;
    uint64_t user_rflags = frame->rflags;

    /* Compute the new user stack pointer.
     * Allocate space for the full rt_sigframe below the current user RSP,
     * aligned to 16 bytes (x86-64 stack alignment convention). */
    size_t frame_size = sizeof(struct rt_sigframe);
    uint64_t new_rsp = (user_rsp - frame_size) & ~15ULL;

    /* Validate the new stack pointer is in user space */
    if (new_rsp >= USER_VADDR_MAX || new_rsp == 0)
        return -EFAULT;

    /* Build the rt_sigframe in kernel memory, then copy it to user stack. */
    struct rt_sigframe sigframe;
    memset(&sigframe, 0, sizeof(sigframe));

    /* The pretcode is the return address the signal handler will ret to.
     * It points to the VDSO sigreturn trampoline (mov eax, SYS_RT_SIGRETURN; syscall). */
    sigframe.pretcode = tramp_addr;

    /* ── Fill ucontext (read by sys_rt_sigreturn to restore state) ── */
    sigframe.uc.uc_flags  = 0;
    sigframe.uc.uc_link   = NULL;
    sigframe.uc.ss_sp     = NULL;
    sigframe.uc.ss_flags  = SS_DISABLE;
    sigframe.uc.ss_size   = 0;
    sigframe.uc.__pad     = 0;

    /* Save the OLD signal mask — sigreturn will restore it */
    sigframe.uc.uc_sigmask = saved_sigmask;

    /* ── Save all general-purpose registers from the interrupt frame ── */
    sigframe.uc.uc_mcontext.r15   = frame->r15;
    sigframe.uc.uc_mcontext.r14   = frame->r14;
    sigframe.uc.uc_mcontext.r13   = frame->r13;
    sigframe.uc.uc_mcontext.r12   = frame->r12;
    sigframe.uc.uc_mcontext.r11   = frame->r11;
    sigframe.uc.uc_mcontext.r10   = frame->r10;
    sigframe.uc.uc_mcontext.r9    = frame->r9;
    sigframe.uc.uc_mcontext.r8    = frame->r8;
    sigframe.uc.uc_mcontext.rbp   = frame->rbp;
    sigframe.uc.uc_mcontext.rdi   = signum;  /* RDI = signal number (1st arg to handler) */
    sigframe.uc.uc_mcontext.rsi   = frame->rsi;
    sigframe.uc.uc_mcontext.rdx   = frame->rdx;
    sigframe.uc.uc_mcontext.rcx   = frame->rcx;
    sigframe.uc.uc_mcontext.rbx   = frame->rbx;
    sigframe.uc.uc_mcontext.rax   = frame->rax;

    /* Trap info (not strictly needed but useful for debugging) */
    sigframe.uc.uc_mcontext.trapno     = (uint64_t)signum;
    sigframe.uc.uc_mcontext.error_code = 0;

    /* The interrupted execution point — these are what sigreturn restores as RIP/RSP */
    sigframe.uc.uc_mcontext.rip     = user_rip;
    sigframe.uc.uc_mcontext.cs      = frame->cs;
    sigframe.uc.uc_mcontext.rflags  = user_rflags;
    sigframe.uc.uc_mcontext.rsp     = user_rsp;
    sigframe.uc.uc_mcontext.ss      = frame->ss;

    /* ── Fill siginfo ──────────────────────────────────────────────── */
    if (info) {
        memcpy(&sigframe.info, info, sizeof(sigframe.info));
    } else {
        sigframe.info.si_signo = signum;
        sigframe.info.si_code  = SI_USER;
    }

    /* Copy the entire frame to the user stack */
    if (copy_to_user((uint64_t)new_rsp, &sigframe, frame_size) < 0)
        return -EFAULT;

    /* ── Redirect the saved execution context to the signal handler ── */
    frame->rip = handler_addr;       /* RIP = signal handler */
    frame->rsp = new_rsp;            /* RSP → pretcode (ret will pop it) */
    frame->rdi = (uint64_t)signum;   /* RDI = 1st argument to handler */

    return 0;
}

/* ── signal_check — Check and deliver pending signals ────────────────── */
void signal_check(void) {
    struct process *p = process_get_current();
    if (!p || !p->pending_signals) return;

    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);

    for (int sig = 1; sig < SIG_MAX; sig++) {
        if (!(p->pending_signals & (1ULL << sig))) continue;

        /* Skip masked signals */
        if (p->sig_mask & (1ULL << sig)) continue;

        p->pending_signals &= ~(1ULL << sig);

        signal_handler_t handler = p->sig_handlers[sig];

        if (handler == SIG_IGN) {
            continue;
        }

        if (handler != SIG_DFL) {
            /* User handler installed — release lock while handler runs */
            if (!p->is_user) {
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                handler(sig);
                spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
            } else {
                /* User-space handler — build sigframe on user stack
                 * if we're in interrupt context with a valid frame. */
                struct cpu_info *cpu = get_cpu_info();
                struct interrupt_frame *frame = cpu ? cpu->current_frame : NULL;
                if (frame && (frame->cs & 3)) {
                    /* Save the current sigmask before we modify it */
                    uint64_t saved_mask = p->sig_mask;
                    uint64_t sa_mask = p->sig_sa_mask[sig];
                    uint32_t flags = p->sig_flags[sig];
                    uint64_t handler_addr = (uint64_t)(uintptr_t)handler;

                    /* Capture siginfo before releasing lock */
                    struct siginfo sig_info_data;
                    memset(&sig_info_data, 0, sizeof(sig_info_data));
                    if (p->sig_info[sig].si_signo == sig) {
                        memcpy(&sig_info_data, &p->sig_info[sig],
                               sizeof(sig_info_data));
                        memset(&p->sig_info[sig], 0, sizeof(struct siginfo));
                    } else {
                        sig_info_data.si_signo = sig;
                        sig_info_data.si_code  = SI_USER;
                    }

                    /* Update signal mask: block this signal during handler
                     * (unless SA_NODEFER), plus any additional sa_mask
                     * signals specified by sigaction. */
                    p->sig_mask |= sa_mask;
                    if (!(flags & SA_NODEFER))
                        p->sig_mask |= (1ULL << sig);

                    /* SA_RESETHAND: reset to SIG_DFL after first delivery */
                    if (flags & SA_RESETHAND)
                        p->sig_handlers[sig] = SIG_DFL;

                    /* Release the sig_lock before building the frame
                     * (copy_to_user may fault; lock must not be held) */
                    spinlock_irqsave_release(&p->sig_lock, __sig_flags);

                    /* Build the sigframe and redirect execution */
                    int ret = signal_setup_frame_userspace(frame, sig,
                            handler_addr, &sig_info_data, saved_mask);

                    if (ret != 0) {
                        /* Frame setup failed — restore the pending bit */
                        spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
                        p->pending_signals |= (1ULL << sig);
                        p->sig_mask = saved_mask;
                        if (flags & SA_RESETHAND)
                            p->sig_handlers[sig] = handler;
                        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                    }

                    /* Signal delivered (or failed) — interrupt context returns
                     * to the handler or original code. Don't process more
                     * signals in this check invocation. */
                    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
                }
                /* If no interrupt frame available, the signal remains pending
                 * (we cleared the bit, but signal_setup_frame_userspace wasn't
                 * called or will be called on next interrupt).  We re-set it
                 * so the next signal_check can retry. */
                if (!p->is_user || !(get_cpu_info() &&
                    get_cpu_info()->current_frame &&
                    (get_cpu_info()->current_frame->cs & 3))) {
                    /* No frame — put the pending bit back for next time */
                    p->pending_signals |= (1ULL << sig);
                }
            }
            continue;
        }

        /* Default actions */
        switch (sig) {
            case SIGSEGV:
            case SIGQUIT:
            case SIGABRT:
                do_coredump(p, sig);
                __attribute__((fallthrough));
            case SIGKILL:
            case SIGTERM:
            case SIGPIPE:
                p->state = PROCESS_ZOMBIE;
                p->exit_code = 128 + sig;
                scheduler_remove(p);
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                scheduler_yield();
                return; /* zombie — never resumes */
            case SIGCHLD:
                /* Default for SIGCHLD is to ignore */
                break;
            case SIGSTOP:
            case SIGTSTP:
            case SIGTTIN:
            case SIGTTOU:
                p->is_suspended = 1;
                p->state = PROCESS_BLOCKED;
                scheduler_remove(p);
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                scheduler_yield();
                /* Resumed by SIGCONT — re-acquire and check more signals */
                spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
                continue;
            case SIGCONT:
                break;
            default:
                /* For real-time signals (SIGRTMIN+) with no handler: ignore */
                if (sig >= SIGRTMIN && sig <= SIGRTMAX)
                    break;
                /* Unknown signal with default: terminate */
                p->state = PROCESS_ZOMBIE;
                p->exit_code = 128 + sig;
                scheduler_remove(p);
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                scheduler_yield();
                return; /* zombie — never resumes */
        }
    }

    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
}

void signal_register(int signum, signal_handler_t handler) {
    if (signum <= 0 || signum >= SIG_MAX) return;
    struct process *p = process_get_current();
    if (!p) return;
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
    p->sig_handlers[signum] = handler;
    p->sig_flags[signum] = 0;  /* No SA flags when using signal() */
    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
}

void signal_register_flags(int signum, signal_handler_t handler, uint32_t flags) {
    if (signum <= 0 || signum >= SIG_MAX) return;
    struct process *p = process_get_current();
    if (!p) return;
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
    p->sig_handlers[signum] = handler;
    p->sig_flags[signum] = flags;
    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
}

void signal_mask(uint64_t sigmask) {
    struct process *p = process_get_current();
    if (!p) return;
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
    p->sig_mask |= sigmask;
    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
}

void signal_unmask(uint64_t sigmask) {
    struct process *p = process_get_current();
    if (!p) return;
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
    p->sig_mask &= ~sigmask;
    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
}

/* ── signal_handle ─────────────────────────────── */
static int signal_handle(void *task, int sig)
{
    (void)task;
    if (sig <= 0 || sig >= SIG_MAX) return -EINVAL;

    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    /* Clear pending and deliver the signal */
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);

    if (p->pending_signals & (1ULL << sig)) {
        p->pending_signals &= ~(1ULL << sig);

        signal_handler_t handler = p->sig_handlers[sig];

        if (handler == SIG_IGN || handler == SIG_DFL) {
            /* Default action handled by signal_check */
            spinlock_irqsave_release(&p->sig_lock, __sig_flags);
            return 0;
        }

        spinlock_irqsave_release(&p->sig_lock, __sig_flags);

        /* Call userspace handler */
        if (p->is_user) {
            /* For user processes, signal delivery happens on return to userspace */
            return 0;
        }

        handler(sig);
        return 0;
    }

    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
    return -EAGAIN;
}

/* ── signal_register_handler ─────────────────────────────── */
static int signal_register_handler(int sig, void *handler)
{
    if (sig <= 0 || sig >= SIG_MAX) return -EINVAL;
    /* ISO C does not allow direct cast from void* to function pointer;
     * use union-based pun for pedantic compliance. */
    union { void *obj; signal_handler_t fn; } u;
    u.obj = handler;
    signal_register(sig, u.fn);
    return 0;
}

/* ── signal_block ─────────────────────────────── */
static int signal_block(int sig)
{
    if (sig <= 0 || sig >= SIG_MAX) return -EINVAL;
    /* Block a single signal by masking it */
    uint64_t mask = (1ULL << sig);
    /* SIGKILL and SIGSTOP cannot be blocked */
    if (sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;
    signal_mask(mask);
    return 0;
}

/* ── signal_unblock ─────────────────────────────── */
static int signal_unblock(int sig)
{
    if (sig <= 0 || sig >= SIG_MAX) return -EINVAL;
    uint64_t mask = (1ULL << sig);
    signal_unmask(mask);
    return 0;
}
