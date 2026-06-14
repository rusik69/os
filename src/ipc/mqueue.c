#include "mqueue.h"
#include "string.h"
#include "printf.h"
#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "heap.h"
#include "errno.h"
#include "types.h"

#ifndef UINT32_MAX
#define UINT32_MAX 4294967295U
#endif

static struct mqueue mqueue_table[MQUEUE_MAX];
static int mqueue_inited = 0;

void mqueue_init(void) {
    memset(mqueue_table, 0, sizeof(mqueue_table));
    for (int i = 0; i < MQUEUE_MAX; i++) {
        wait_queue_init(&mqueue_table[i].r_wq);
        wait_queue_init(&mqueue_table[i].w_wq);
    }
    mqueue_inited = 1;
}

static int find_queue(const char *name) {
    for (int i = 0; i < MQUEUE_MAX; i++) {
        if (mqueue_table[i].in_use && strcmp(mqueue_table[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int alloc_queue(void) {
    for (int i = 0; i < MQUEUE_MAX; i++) {
        if (!mqueue_table[i].in_use) {
            memset(&mqueue_table[i], 0, sizeof(struct mqueue));
            mqueue_table[i].in_use = 1;
            mqueue_table[i].msg_max = MQUEUE_MAX_MSG;
            mqueue_table[i].msg_size_max = MQUEUE_MAX_SIZE;
            wait_queue_init(&mqueue_table[i].r_wq);
            wait_queue_init(&mqueue_table[i].w_wq);
            return i;
        }
    }
    return -1;
}

mqd_t mq_open(const char *name, int oflag, ...) {
    if (!mqueue_inited) return -1;

    int idx = find_queue(name);
    if (idx >= 0) {
        /* Queue already exists — return existing descriptor */
        /* If O_EXCL is set and queue exists, fail */
        if (oflag & 0x80) /* O_EXCL = 0x80 in POSIX */
            return -1;
        return idx;
    }

    idx = alloc_queue();
    if (idx < 0) return -1;

    strncpy(mqueue_table[idx].name, name, 31);
    mqueue_table[idx].name[31] = '\0';
    mqueue_table[idx].oflags = oflag;
    return idx;
}

int mq_close(mqd_t mqdes) {
    if (mqdes < 0 || mqdes >= MQUEUE_MAX || !mqueue_table[mqdes].in_use)
        return -1;
    mqueue_table[mqdes].in_use = 0;
    return 0;
}

int mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned int msg_prio) {
    (void)msg_prio;
    if (mqdes < 0 || mqdes >= MQUEUE_MAX || !mqueue_table[mqdes].in_use)
        return -1;
    /* Reject messages larger than max size or longer than uint32_t (for len field) */
    if (msg_len > (size_t)mqueue_table[mqdes].msg_size_max)
        return -1;
    if (msg_len > UINT32_MAX)
        return -1;

    struct mqueue *q = &mqueue_table[mqdes];

    /* Block if full — unless O_NONBLOCK is set */
    if (q->msg_count >= q->msg_max) {
        if (q->oflags & O_NONBLOCK) {
            return -EAGAIN;
        }
        while (q->msg_count >= q->msg_max) {
            struct process *cur = process_get_current();
            if (!cur) return -1;
            cur->state = PROCESS_BLOCKED;
            scheduler_remove(cur);
            wait_queue_sleep(&q->w_wq);
            if (!q->in_use) return -1;
        }
    }

    /* Find a free message slot */
    int slot = -1;
    for (int i = 0; i < q->msg_max; i++) {
        if (!q->msgs[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    memcpy(q->msgs[slot].data, msg_ptr, msg_len);
    q->msgs[slot].len = (uint32_t)msg_len;
    q->msgs[slot].priority = (uint32_t)msg_prio;
    q->msgs[slot].in_use = 1;
    q->msg_count++;

    /* Wake any readers */
    wait_queue_wake_all(&q->r_wq);

    /* mq_notify: if notification is armed and queue was empty before, send signal */
    if (q->notify_active && q->msg_count == 1) {
        if (q->notify_pid > 0) {
            signal_send(q->notify_pid, q->notify_signo);
        }
        q->notify_active = 0; /* one-shot */
    }

    return 0;
}

ssize_t mq_receive(mqd_t mqdes, char *msg_ptr, size_t msg_len, unsigned int *msg_prio) {
    if (mqdes < 0 || mqdes >= MQUEUE_MAX || !mqueue_table[mqdes].in_use)
        return -1;
    if (msg_len > UINT32_MAX)
        return -1;

    struct mqueue *q = &mqueue_table[mqdes];

    /* Block if empty — unless O_NONBLOCK is set */
    if (q->msg_count == 0) {
        if (q->oflags & O_NONBLOCK) {
            return -EAGAIN;
        }
        while (q->msg_count == 0) {
            struct process *cur = process_get_current();
            if (!cur) return -1;
            cur->state = PROCESS_BLOCKED;
            scheduler_remove(cur);
            wait_queue_sleep(&q->r_wq);
            if (!q->in_use) return -1;
        }
    }

    /* Find highest-priority message */
    int best = -1;
    unsigned int best_prio = 0;
    for (int i = 0; i < q->msg_max; i++) {
        if (q->msgs[i].in_use && q->msgs[i].priority >= best_prio) {
            best = i;
            best_prio = q->msgs[i].priority;
        }
    }
    if (best < 0) return -1;

    size_t copy = q->msgs[best].len < msg_len ? q->msgs[best].len : msg_len;
    memcpy(msg_ptr, q->msgs[best].data, copy);
    if (msg_prio) *msg_prio = q->msgs[best].priority;
    q->msgs[best].in_use = 0;
    q->msg_count--;

    /* Wake any writers waiting for space */
    wait_queue_wake_all(&q->w_wq);

    return (ssize_t)copy;
}

int mq_notify(mqd_t mqdes, const struct sigevent *notification) {
    if (mqdes < 0 || mqdes >= MQUEUE_MAX || !mqueue_table[mqdes].in_use)
        return -1;

    struct mqueue *q = &mqueue_table[mqdes];

    if (!notification) {
        /* Unregister notification */
        q->notify_active = 0;
        q->notify_pid = 0;
        return 0;
    }

    struct process *cur = process_get_current();
    if (!cur) return -1;

    q->notify_pid = cur->pid;
    q->notify_signo = notification->sigev_signo;
    q->notify_active = 1;

    return 0;
}

int mq_getattr(mqd_t mqdes, struct mq_attr *attr) {
    if (mqdes < 0 || mqdes >= MQUEUE_MAX || !mqueue_table[mqdes].in_use)
        return -1;
    if (!attr) return -1;

    struct mqueue *q = &mqueue_table[mqdes];
    attr->mq_flags = 0;
    attr->mq_maxmsg = q->msg_max;
    attr->mq_msgsize = q->msg_size_max;
    attr->mq_curmsgs = q->msg_count;
    return 0;
}
