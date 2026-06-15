/* bpftrace_probes.c — list available probes */
#include "unistd.h"
#include "stdio.h"

int main(void){
    printf("Available tracepoints:\n");
    printf("  syscalls:sys_enter_open, syscalls:sys_exit_open\n");
    printf("  sched:sched_process_fork, sched:sched_process_exec\n");
    printf("  irq:irq_handler_entry, irq:irq_handler_exit\n");
    printf("  Use kernel shell 'bpftrace_probes' for full list\n");
    return 0;
}
