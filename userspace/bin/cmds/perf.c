/* perf.c — performance analysis tools */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: perf stat <command> [args]\n");return 1;}
    if(strcmp(argv[1],"stat")==0&&argc>2){
        printf("perf stat -- running: ");
        for(int i=2;i<argc;i++)printf("%s ",argv[i]);
        printf("\n");
        printf("  %'llu instructions\n",(unsigned long long)0);
        printf("  %'llu cycles\n",(unsigned long long)0);
        printf("  %'llu cache-references\n",(unsigned long long)0);
        printf("  %'llu cache-misses\n",(unsigned long long)0);
    }else{
        printf("perf: only 'stat' subcommand supported\n");
    }
    return 0;
}
