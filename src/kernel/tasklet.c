#include "tasklet.h"
#include "printf.h"
#include "string.h"
#include "softirq.h"
#define TASKLET_MAX 32
static struct tasklet_struct *tasklet_list[TASKLET_MAX];
static int tasklet_count = 0;
static void tasklet_handler(void) {
    for (int i = 0; i < tasklet_count; i++) {
        if (tasklet_list[i] && tasklet_list[i]->state) {
            tasklet_list[i]->state = 0;
            if (tasklet_list[i]->func)
                tasklet_list[i]->func(tasklet_list[i]->data);
        }
    }
}
void tasklet_init(void) {
    memset(tasklet_list, 0, sizeof(tasklet_list));
    tasklet_count = 0;
    softirq_register(SOFTIRQ_TASKLET, tasklet_handler);
    kprintf("[OK] Tasklets initialized\n");
}
int tasklet_schedule(struct tasklet_struct *t) {
    if (!t || tasklet_count >= TASKLET_MAX) return -1;
    if (t->state) return 0; /* already scheduled */
    t->state = 1;
    tasklet_list[tasklet_count++] = t;
    softirq_raise(SOFTIRQ_TASKLET);
    return 0;
}

/* ── Stub: tasklet_hi_schedule ─────────────────────────────── */
int tasklet_hi_schedule(void *t)
{
    (void)t;
    kprintf("[tasklet] tasklet_hi_schedule: not yet implemented\n");
    return 0;
}
/* ── Stub: tasklet_kill ─────────────────────────────── */
int tasklet_kill(void *t)
{
    (void)t;
    kprintf("[tasklet] tasklet_kill: not yet implemented\n");
    return 0;
}
/* ── Stub: tasklet_enable ─────────────────────────────── */
int tasklet_enable(void *t)
{
    (void)t;
    kprintf("[tasklet] tasklet_enable: not yet implemented\n");
    return 0;
}
/* ── Stub: tasklet_disable ─────────────────────────────── */
int tasklet_disable(void *t)
{
    (void)t;
    kprintf("[tasklet] tasklet_disable: not yet implemented\n");
    return 0;
}
