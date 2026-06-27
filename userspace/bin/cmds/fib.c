
#include <stdio.h>
int main(void){
    unsigned long long a=0,b=1,c;
    printf("Fibonacci Sequence\n");
    for(int i=0;i<90;i++){
        printf("fib(%d) = %llu\n",i,a);
        c=a+b;a=b;b=c;
        if(i>0&&i%30==0){printf("--press enter--");char tmp[4];read(0,tmp,1);}
    }
    return 0;
}
