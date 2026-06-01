#ifndef WDT_ENHANCED_H
#define WDT_ENHANCED_H

#include "types.h"

/* Enhanced watchdog states */
#define WDT_STATE_STOPPED   0
#define WDT_STATE_RUNNING   1
#define WDT_STATE_TIMEOUT   2

/* Watchdog pretimeout action */
#define WDT_ACT_NONE        0
#define WDT_ACT_INT         1
#define WDT_ACT_RESET       2
#define WDT_ACT_NMI         3

/* Hardware watchdog I/O ports (Intel TCO) */
#define TCO_BASE           0x60   /* TCOBASE typically at 0x60 on ICH/PCH */
#define TCO_RLD            (TCO_BASE + 0x00)  /* TCO Reload */
#define TCO_DAT_IN         (TCO_BASE + 0x02)  /* TCO Data In */
#define TCO_DAT_OUT        (TCO_BASE + 0x04)  /* TCO Data Out */
#define TCO1_STS           (TCO_BASE + 0x04)  /* TCO1 Status */
#define TCO2_STS           (TCO_BASE + 0x06)  /* TCO2 Status */
#define TCO1_CNT           (TCO_BASE + 0x08)  /* TCO1 Control */
#define TCO2_CNT           (TCO_BASE + 0x0A)  /* TCO2 Control */

/* TCO1_CNT bits */
#define TCO_CNT_TMR_HALT   (1 << 11)
#define TCO_CNT_TMR_PERIOD (1 << 13)

/* Software watchdog + hardware hybrid state */
struct wdt_enhanced_state {
    int      active;
    int      timeout_secs;
    int      pretimeout_secs;
    int      pretimeout_action;
    uint64_t last_pet;
    uint64_t timeout_ticks;
    int      timer_id;
    int      pretimer_id;
    int      hw_watchdog;     /* Whether HW TCO watchdog is available */
};

/* Callback type for pretimeout */
typedef void (*wdt_pretimeout_fn_t)(void);

/* Enhanced API */
int  wdt_enhanced_init(int timeout_secs);
void wdt_enhanced_pet(void);
void wdt_enhanced_stop(void);
int  wdt_enhanced_set_pretimeout(int secs, int action);
void wdt_enhanced_set_pretimeout_fn(wdt_pretimeout_fn_t fn);
int  wdt_enhanced_get_state(void);
int  wdt_enhanced_get_timeout(void);
int  wdt_enhanced_get_remaining(void);
int  wdt_enhanced_hw_probe(void);
void wdt_enhanced_hw_pet(void);

#endif /* WDT_ENHANCED_H */
