#ifndef RTC_ALARM_ENHANCED_H
#define RTC_ALARM_ENHANCED_H

#include "types.h"
#include "rtc.h"

/* Max alarms in the queue */
#define RTC_ALARM_MAX 8

/* Alarm modes */
#define RTC_ALARM_MODE_DISABLED  0
#define RTC_ALARM_MODE_ONESHOT   1  /* Fire once, then disable */
#define RTC_ALARM_MODE_REPEATING 2  /* Fire daily at the same time */
#define RTC_ALARM_MODE_HOURLY   3  /* Fire every hour */

/* Alarm callback */
typedef void (*rtc_alarm_callback_t)(int alarm_id, void *user_data);

/* Enhanced alarm descriptor */
struct rtc_enhanced_alarm {
    int      id;
    int      used;
    int      mode;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    int      fired;
    rtc_alarm_callback_t callback;
    void    *user_data;
};

/* API */
int  rtc_alarm_enhanced_init(void);
int  rtc_alarm_create(int mode, uint8_t hour, uint8_t minute, uint8_t second,
                      rtc_alarm_callback_t cb, void *user_data);
int  rtc_alarm_cancel(int alarm_id);
int  rtc_alarm_get_status(int alarm_id, struct rtc_enhanced_alarm *alarm);
void rtc_alarm_check_and_fire(void);
int  rtc_alarm_is_initialized(void);

#endif /* RTC_ALARM_ENHANCED_H */
