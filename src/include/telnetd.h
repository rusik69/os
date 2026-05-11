#ifndef TELNETD_H
#define TELNETD_H

/* Legacy boot-time initializer */
void telnetd_init(void);
void telnetd_task(void);

/* Service-style start / stop */
int  telnetd_start(void);
void telnetd_stop(void);

#endif
