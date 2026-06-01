#ifndef MQUEUE_H
#define MQUEUE_H

#include "types.h"
#include "signal.h"
#include "waitqueue.h"

/* POSIX message queue — simplified implementation for kernel.
 *
 * Each queue holds up to 10 messages of up to 64 bytes each.
 */

#define MQUEUE_MAX     16
#define MQUEUE_MAX_MSG 10
#define MQUEUE_MAX_SIZE 64

/* Notification type for mq_notify */
struct sigevent {
    int      sigev_notify;            /* SIGEV_SIGNAL, SIGEV_NONE, SIGEV_THREAD */
    int      sigev_signo;             /* signal number */
    void    *sigev_value;             /* value to pass with signal */
    void    (*sigev_notify_function)(void *);
    void    *sigev_notify_attributes;
};

#define SIGEV_SIGNAL 0
#define SIGEV_NONE   1
#define SIGEV_THREAD 2

struct mqueue_msg {
    uint8_t data[MQUEUE_MAX_SIZE];
    uint32_t len;
    uint32_t priority;
    int in_use;
};

typedef int mqd_t;  /* message queue descriptor */

struct mqueue {
    char     name[32];
    struct mqueue_msg msgs[MQUEUE_MAX_MSG];
    int      msg_count;
    int      msg_max;       /* max msgs (MQUEUE_MAX_MSG) */
    int      msg_size_max;  /* max size (MQUEUE_MAX_SIZE) */
    int      read_pos;
    int      in_use;
    struct wait_queue r_wq;  /* readers wait for msgs */
    struct wait_queue w_wq;  /* writers wait for space */

    /* Notification state (single notifier) */
    int      notify_pid;
    int      notify_signo;
    int      notify_active; /* 1 = notification armed */
};

/* ── Attributes structure ────────────────────────────────────────── */
struct mq_attr;

/* ── API ─────────────────────────────────────────────────────────── */

/* Create or open a named message queue */
mqd_t mq_open(const char *name, int oflag, ...);

/* Close a message queue */
int mq_close(mqd_t mqdes);

/* Send a message to the queue */
int mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned int msg_prio);

/* Receive a message from the queue */
ssize_t mq_receive(mqd_t mqdes, char *msg_ptr, size_t msg_len, unsigned int *msg_prio);

/* Register for notification when a message arrives on an empty queue */
int mq_notify(mqd_t mqdes, const struct sigevent *notification);

/* Get queue attributes */
int mq_getattr(mqd_t mqdes, struct mq_attr *attr);

/* Initialise message queue subsystem */
void mqueue_init(void);

/* ── Attributes structure ────────────────────────────────────────── */
struct mq_attr {
    long mq_flags;
    long mq_maxmsg;
    long mq_msgsize;
    long mq_curmsgs;
};

#endif /* MQUEUE_H */
