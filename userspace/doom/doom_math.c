#include "doom.h"

static int32_t sin_table[DOOM_ANGLE_UNITS];

/* Quarter-wave sine table (0..90 deg), values * 65536 */
static const int32_t sin_q[257] = {
    0, 402, 804, 1206, 1608, 2009, 2410, 2811,
    3212, 3612, 4011, 4410, 4808, 5205, 5602, 5998,
    6393, 6786, 7179, 7571, 7962, 8351, 8740, 9127,
    9512, 9896, 10278, 10659, 11039, 11417, 11793, 12167,
    12539, 12910, 13279, 13645, 14010, 14372, 14732, 15090,
    15446, 15800, 16151, 16499, 16846, 17189, 17530, 17869,
    18204, 18537, 18868, 19195, 19519, 19841, 20159, 20475,
    20787, 21096, 21403, 21706, 22005, 22301, 22594, 22884,
    23170, 23452, 23731, 24007, 24279, 24547, 24811, 25072,
    25329, 25582, 25832, 26077, 26319, 26556, 26790, 27019,
    27245, 27466, 27683, 27896, 28105, 28310, 28510, 28706,
    28898, 29085, 29268, 29447, 29621, 29791, 29956, 30117,
    30273, 30424, 30571, 30714, 30852, 30985, 31113, 31237,
    31356, 31470, 31580, 31685, 31785, 31880, 31971, 32057,
    32137, 32213, 32285, 32351, 32412, 32469, 32521, 32567,
    32609, 32646, 32678, 32705, 32728, 32745, 32757, 32765,
    32767, 32765, 32757, 32745, 32728, 32705, 32678, 32646,
    32609, 32567, 32521, 32469, 32412, 32351, 32285, 32213,
    32137, 32057, 31971, 31880, 31785, 31685, 31580, 31470,
    31356, 31237, 31113, 30985, 30852, 30714, 30571, 30424,
    30273, 30117, 29956, 29791, 29621, 29447, 29268, 29085,
    28898, 28706, 28510, 28310, 28105, 27896, 27683, 27466,
    27245, 27019, 26790, 26556, 26319, 26077, 25832, 25582,
    25329, 25072, 24811, 24547, 24279, 24007, 23731, 23452,
    23170, 22884, 22594, 22301, 22005, 21706, 21403, 21096,
    20787, 20475, 20159, 19841, 19519, 19195, 18868, 18537,
    18204, 17869, 17530, 17189, 16846, 16499, 16151, 15800,
    15446, 15090, 14732, 14372, 14010, 13645, 13279, 12910,
    12539, 12167, 11793, 11417, 11039, 10659, 10278, 9896,
    9512, 9127, 8740, 8351, 7962, 7571, 7179, 6786,
    6393, 5998, 5602, 5205, 4808, 4410, 4011, 3612,
    3212, 2811, 2410, 2009, 1608, 1206, 804, 402,
    0,
};

void doom_math_init(void) {
    const int qmax = 128;
    const int qsize = DOOM_ANGLE_UNITS / 4;
    for (int i = 0; i < DOOM_ANGLE_UNITS; i++) {
        int quadrant = i / qsize;
        int idx = i % qsize;
        int t = (idx * qmax) / qsize;
        if (t > qmax) t = qmax;
        int32_t s;
        switch (quadrant) {
        case 0: s = sin_q[t]; break;
        case 1: s = sin_q[qmax - t]; break;
        case 2: s = -sin_q[t]; break;
        default: s = -sin_q[qmax - t]; break;
        }
        sin_table[i] = s * 2;
    }
}

int32_t doom_sin(int32_t angle) {
    angle &= (DOOM_ANGLE_UNITS - 1);
    return sin_table[angle];
}

int32_t doom_cos(int32_t angle) {
    return doom_sin(angle + DOOM_ANGLE_UNITS / 4);
}

int32_t doom_atan2(int32_t dy, int32_t dx) {
    if (dx == 0 && dy == 0) return 0;
    int32_t ax = dx < 0 ? -dx : dx;
    int32_t ay = dy < 0 ? -dy : dy;
    int32_t angle;
    if (ax >= ay)
        angle = (ay * (DOOM_ANGLE_UNITS / 4)) / (ax + 1);
    else
        angle = (DOOM_ANGLE_UNITS / 4) - (ax * (DOOM_ANGLE_UNITS / 4)) / (ay + 1);
    if (dx >= 0 && dy >= 0) return angle;
    if (dx < 0 && dy >= 0) return DOOM_ANGLE_UNITS / 2 - angle;
    if (dx < 0 && dy < 0) return DOOM_ANGLE_UNITS / 2 + angle;
    return DOOM_ANGLE_UNITS - angle;
}

int doom_test_trig(void) {
    doom_math_init();
    int32_t s0 = doom_sin(0);
    int32_t c0 = doom_cos(0);
    int32_t s90 = doom_sin(DOOM_ANGLE_UNITS / 4);
    int32_t c90 = doom_cos(DOOM_ANGLE_UNITS / 4);
    if (s0 < -1000 || s0 > 1000) return 0;
    if (c0 < 60000 || c0 > 70000) return 0;
    if (s90 < 60000 || s90 > 70000) return 0;
    if (c90 < -1000 || c90 > 1000) return 0;
    return 1;
}
