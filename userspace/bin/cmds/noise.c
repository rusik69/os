
#include <stdio.h>
#include <stdlib.h>
int main(void){
    printf("Value Noise\n");
    for(int y=0;y<20;y++){
        for(int x=0;x<60;x++){
            int v=(rand()%256+rand()%128+rand()%64)/3;
            char c=v<50?'.':(v<100?'-':(v<150?'=':(v<200?'#':'@')));
            putchar(c);
        }
        printf("\n");
    }
    return 0;
}
