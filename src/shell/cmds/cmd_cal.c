/* cmd_cal.c -- Print a monthly calendar */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_cal(int argc, char **argv) {
    struct libc_rtc_time tm;
    if (libc_rtc_get_time(&tm) != 0) {
        kprintf("cal: cannot get current time\n");
        return 1;
    }

    int year = tm.year;
    int month = tm.month;

    /* Parse optional [month] [year] arguments */
    if (argc >= 2) {
        char *end;
        long m = strtol(argv[1], &end, 10);
        if (*end == '\0' && m >= 1 && m <= 12) {
            month = (int)m;
        } else {
            kprintf("cal: invalid month '%s' (use 1-12)\n", argv[1]);
            return 1;
        }
    }
    if (argc >= 3) {
        char *end;
        long y = strtol(argv[2], &end, 10);
        if (*end == '\0' && y >= 1 && y <= 9999) {
            year = (int)y;
        } else {
            kprintf("cal: invalid year '%s'\n", argv[2]);
            return 1;
        }
    }

    static const char *months[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    static const int dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    int days = dim[month - 1];
    if (month == 2) {
        int leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        if (leap) days = 29;
    }

    /* Zeller's congruence: day of week for the 1st of the month */
    int m = month;
    int y = year;
    if (m < 3) { m += 12; y--; }
    int K = y % 100;
    int J = y / 100;
    /* h: 0=Sat,1=Sun,2=Mon,3=Tue,4=Wed,5=Thu,6=Fri */
    int h = (1 + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    /* Convert so 0=Sunday, 1=Monday, ..., 6=Saturday */
    int dow = (h + 6) % 7;

    /* Print header */
    kprintf("      %s %d\n", months[month - 1], year);
    kprintf("Su Mo Tu We Th Fr Sa\n");

    /* Print leading blank days */
    int col;
    for (col = 0; col < dow; col++)
        kprintf("   ");

    /* Print days */
    for (int d = 1; d <= days; d++) {
        kprintf("%2d", d);
        if (++col >= 7) {
            if (d < days) kprintf("\n");
            col = 0;
        } else {
            kprintf(" ");
        }
    }
    kprintf("\n");
    return 0;
}
