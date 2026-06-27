
#include <stdio.h>
int main(void){
    printf("Primes up to 1000\n");
    char sieve[1001]={0};
    for(int i=2;i<=1000;i++){
        if(!sieve[i]){
            printf("%d ",i);
            for(int j=i*i;j<=1000;j+=i)sieve[j]=1;
        }
    }
    printf("\n");
    return 0;
}
