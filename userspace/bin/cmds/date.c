#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_months[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

int main(void) {
    struct timespec ts;
    if (clock_gettime(0, &ts) < 0) {
        printf("date: clock_gettime failed\n");
        return 1;
    }
    long long secs = (long long)ts.tv_sec;
    /* Epoch 1970-01-01 */
    int y = 1970;
    while (1) {
        int d = is_leap(y) ? 366 : 365;
        if (secs >= d * 86400LL) {
            secs -= d * 86400LL;
            y++;
        } else break;
    }
    days_in_months[1] = is_leap(y) ? 29 : 28;
    int m;
    for (m = 0; m < 12; m++) {
        int dim = days_in_months[m] * 86400;
        if (secs >= dim) {
            secs -= dim;
        } else break;
    }
    int day = (int)(secs / 86400) + 1;
    secs %= 86400;
    int h = (int)(secs / 3600);
    secs %= 3600;
    int min = (int)(secs / 60);
    int s = (int)(secs % 60);
    printf("%04d-%02d-%02d %02d:%02d:%02d\n", y, m + 1, day, h, min, s);
    return 0;
}
