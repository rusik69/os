/* trap.c — shell signal handler setup */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static const char *sig_name(int n) {
    switch(n) {
        case 1: return "SIGHUP";
        case 2: return "SIGINT";
        case 3: return "SIGQUIT";
        case 4: return "SIGILL";
        case 5: return "SIGTRAP";
        case 6: return "SIGABRT";
        case 7: return "SIGBUS";
        case 8: return "SIGFPE";
        case 9: return "SIGKILL";
        case 10: return "SIGUSR1";
        case 11: return "SIGSEGV";
        case 12: return "SIGUSR2";
        case 13: return "SIGPIPE";
        case 14: return "SIGALRM";
        case 15: return "SIGTERM";
        case 17: return "SIGCHLD";
        case 18: return "SIGCONT";
        case 19: return "SIGSTOP";
        case 20: return "SIGTSTP";
        default: return "UNKNOWN";
    }
}

static int sig_number(const char *name) {
    if(name[0]=='S'&&name[1]=='I'&&name[2]=='G') name+=3;
    if(strcmp(name,"HUP")==0||strcmp(name,"1")==0) return 1;
    if(strcmp(name,"INT")==0||strcmp(name,"2")==0) return 2;
    if(strcmp(name,"QUIT")==0||strcmp(name,"3")==0) return 3;
    if(strcmp(name,"ILL")==0||strcmp(name,"4")==0) return 4;
    if(strcmp(name,"TRAP")==0||strcmp(name,"5")==0) return 5;
    if(strcmp(name,"ABRT")==0||strcmp(name,"6")==0) return 6;
    if(strcmp(name,"BUS")==0||strcmp(name,"7")==0) return 7;
    if(strcmp(name,"FPE")==0||strcmp(name,"8")==0) return 8;
    if(strcmp(name,"KILL")==0||strcmp(name,"9")==0) return 9;
    if(strcmp(name,"USR1")==0||strcmp(name,"10")==0) return 10;
    if(strcmp(name,"SEGV")==0||strcmp(name,"11")==0) return 11;
    if(strcmp(name,"USR2")==0||strcmp(name,"12")==0) return 12;
    if(strcmp(name,"PIPE")==0||strcmp(name,"13")==0) return 13;
    if(strcmp(name,"ALRM")==0||strcmp(name,"14")==0) return 14;
    if(strcmp(name,"TERM")==0||strcmp(name,"15")==0) return 15;
    if(strcmp(name,"CHLD")==0||strcmp(name,"17")==0) return 17;
    if(strcmp(name,"CONT")==0||strcmp(name,"18")==0) return 18;
    if(strcmp(name,"STOP")==0||strcmp(name,"19")==0) return 19;
    if(strcmp(name,"TSTP")==0||strcmp(name,"20")==0) return 20;
    return atoi(name);
}

int main(int argc,char*argv[]){
    if(argc<2){
        printf("Usage: trap <action> <signal>...\n");
        printf("  action: '' (default), '-' (reset), or 'IGNORE' to ignore\n");
        printf("  signal: name or number\n");
        return 1;
    }

    const char *action = argv[1];
    int ignore = 0, reset = 0;

    if(strcmp(action,"-")==0) reset = 1;
    else if(strcmp(action,"IGNORE")==0 || strcmp(action,"ignore")==0) ignore = 1;

    if(argc==2) {
        /* Print current disposition info */
        for(int i=1;i<=20;i++) {
            if(i==9||i==19) continue; /* SIGKILL and SIGSTOP can't be trapped */
            printf("%s (%d)\n", sig_name(i), i);
        }
        return 0;
    }

    for(int i=2;i<argc;i++){
        int sig = sig_number(argv[i]);
        if(sig<1||sig>20){
            printf("trap: unknown signal '%s'\n",argv[i]);
            return 1;
        }
        if(sig==9||sig==19){
            printf("trap: cannot trap SIGKILL/SIGSTOP\n");
            return 1;
        }
        if(reset){
            signal(sig,(void(*)(int))0); /* SIG_DFL */
            printf("trap: reset %s (%d)\n",sig_name(sig),sig);
        }else if(ignore){
            signal(sig,(void(*)(int))1); /* SIG_IGN */
            printf("trap: ignoring %s (%d)\n",sig_name(sig),sig);
        }else{
            /* Custom handler - just print for now */
            printf("trap: set handler for %s (%d)\n",sig_name(sig),sig);
        }
    }
    return 0;
}
