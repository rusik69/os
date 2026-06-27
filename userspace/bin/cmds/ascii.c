
#include <stdio.h>
int main(void){
    printf("ASCII Table (32-126)\n");
    for(int i=0;i<95;i++){
        printf("%3d: %c",i+32,i+32);
        if((i+1)%8==0)printf("\n");
        else printf(" | ");
    }
    printf("\n");
    return 0;
}
