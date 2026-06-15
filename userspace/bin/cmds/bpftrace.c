/* bpftrace.c — dynamic tracing tool */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: bpftrace -e <program> or bpftrace <file>\n");return 1;}
    printf("bpftrace: kernel-level tracing via eBPF\n");
    if(strcmp(argv[1],"-e")==0&&argc>2)printf("  Program: %s\n",argv[2]);
    else printf("  Script: %s\n",argv[1]);
    printf("  Use kernel shell 'bpftrace' for actual execution\n");
    return 0;
}
