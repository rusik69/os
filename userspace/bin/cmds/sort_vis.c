/* sort_vis.c — bubble sort visualization */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void delay(void) {
    for (volatile int d = 0; d < 1000000; d++);
}

int main(void) {
    int n = 20, arr[20];
    for (int i = 0; i < n; i++) arr[i] = rand() % 20 + 1;
    printf("\033[2JBubble Sort Visualization\n");
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) { int t = arr[j]; arr[j] = arr[j + 1]; arr[j + 1] = t; }
            printf("\033[H");
            for (int k = 0; k < n; k++) {
                for (int h = 0; h < arr[k]; h++) putchar('#');
                printf("\n");
            }
            delay();
        }
    }
    printf("Sorted!\n");
    return 0;
}
