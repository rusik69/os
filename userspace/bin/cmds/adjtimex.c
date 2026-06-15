/* adjtimex.c — Show/adjust system clock parameters */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int set_mode = 0;
    int freq = 0;

    if (argc > 1) {
        if (argc == 3 && strcmp(argv[1], "--set") == 0) {
            set_mode = 1;
            freq = atoi(argv[2]);
        } else {
            printf("Usage: adjtimex          (display clock parameters)\n");
            printf("       adjtimex --set FREQ  (set tick frequency)\n");
            return 1;
        }
    }

    if (set_mode) {
        /* In a real system this would call adjtimex(2) to set frequency.
         * We just report what we would do. */
        printf("adjtimex: would set frequency to %d\n", freq);
        return 0;
    }

    /* Display mode: show system time and clock parameters */
    struct timespec ts;
    struct sysinfo si;
    int have_time = (clock_gettime(0, &ts) == 0);
    int have_sys = (sysinfo(&si) == 0);

    printf("System clock parameters:\n");
    if (have_time) {
        printf("  Time:              %llu.%09llu seconds since epoch\n",
               (unsigned long long)ts.tv_sec,
               (unsigned long long)ts.tv_nsec);
    }
    if (have_sys) {
        unsigned long long days = si.uptime / 86400;
        unsigned long long rem = si.uptime % 86400;
        unsigned long long hrs = rem / 3600;
        unsigned long long mins = (rem % 3600) / 60;
        unsigned long long secs = rem % 60;
        printf("  Uptime:            %llu day%s %02llu:%02llu:%02llu\n",
               days, days == 1 ? "" : "s", hrs, mins, secs);
        printf("  Load average:      %llu.%02llu %llu.%02llu %llu.%02llu\n",
               si.loads[0] / 65536, (si.loads[0] % 65536) * 100 / 65536,
               si.loads[1] / 65536, (si.loads[1] % 65536) * 100 / 65536,
               si.loads[2] / 65536, (si.loads[2] % 65536) * 100 / 65536);
    }
    printf("  Timer frequency:   1000 Hz (assumed)\n");
    printf("  (adjtimex --set <freq> to adjust)\n");

    return 0;
}
