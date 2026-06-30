#ifndef SIGNAL_FRAME_H
#define SIGNAL_FRAME_H

#include "types.h"
#include "signal.h"

/*
 * signal_frame.h — User-mode signal delivery frame structures.
 *
 * When a signal with a user-installed handler is delivered, the kernel
 * pushes a signal frame onto the user stack, saves the full register
 * context, and redirects execution to the registered signal handler.
 *
 * The signal handler executes as a normal C function:
 *   void handler(int signum, siginfo_t *info, void *ucontext);
 *
 * When the handler returns via ret, execution jumps to the trampoline
 * (pointed to by pretcode), which calls sys_rt_sigreturn.  The kernel
 * reads the saved context from the signal frame and restores execution
 * at the point where the signal interrupted the user code.
 *
 * Layout on the user stack (high to low address):
 *   [siginfo]      ← &frame->info  (2nd arg to handler)
 *   [ucontext]     ← &frame->uc    (3rd arg to handler)
 *   [pretcode]     ← return addr   (RSP points here when handler starts)
 */

/* Saved register context for signal handling.
 * Mirrors the CPU-side register save layout so the kernel can
 * save/restore the full interrupted state. */
struct sigcontext {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t trapno, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

/* User context structure — POSIX ucontext_t compatible layout.
 * This structure is read by sys_rt_sigreturn to restore the
 * interrupted execution context. */
typedef struct {
    uint64_t uc_flags;
    void    *uc_link;              /* pointer to next context */
    void    *ss_sp;                /* alternate signal stack base */
    int      ss_flags;             /* SS_DISABLE, SS_ONSTACK */
    size_t   ss_size;              /* alternate signal stack size */
    uint64_t __pad;                /* alignment padding */
    struct sigcontext uc_mcontext; /* saved registers */
    uint64_t uc_sigmask;           /* signal mask to restore */
} ucontext_t;

/* Complete signal frame on the user stack.
 * The kernel allocates this on the user stack during signal delivery,
 * then redirects to the signal handler.  rt_sigreturn reads the
 * ucontext from the stack to restore the interrupted state. */
struct rt_sigframe {
    uint64_t    pretcode;   /* trampoline return address (consumed by ret) */
    ucontext_t  uc;         /* saved context + signal mask */
    struct siginfo info;    /* siginfo for the delivered signal */
};

#endif /* SIGNAL_FRAME_H */
