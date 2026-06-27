
#include <stdio.h>
#include <unistd.h>
#include <string.h>
int main(void){
    char b[9]={0};int turn=0,player=1;char buf[16];
    printf("\033[2JTic-Tac-Toe\n1-9=move q=quit\n");
    while(1){
        printf("\033[H  %c|%c|%c\n ---+---+---\n  %c|%c|%c\n ---+---+---\n  %c|%c|%c\n",
            b[0]?b[0]:'1',b[1]?b[1]:'2',b[2]?b[2]:'3',
            b[3]?b[3]:'4',b[4]?b[4]:'5',b[5]?b[5]:'6',
            b[6]?b[6]:'7',b[7]?b[7]:'8',b[8]?b[8]:'9');
        if(turn==9){printf("DRAW!\n");break;}
        printf("Player %d (%c): ",player,player==1?'X':'O');
        int n=read(0,buf,15);if(n<=0)break;buf[n]=0;if(buf[0]=='q')break;
        int m=buf[0]-'1';if(m<0||m>8||b[m])continue;
        b[m]=player==1?'X':'O';turn++;
        int win=0;
        for(int i=0;i<3;i++)if(b[i*3]&&b[i*3]==b[i*3+1]&&b[i*3]==b[i*3+2])win=1;
        for(int i=0;i<3;i++)if(b[i]&&b[i]==b[i+3]&&b[i]==b[i+6])win=1;
        if(b[0]&&b[0]==b[4]&&b[0]==b[8])win=1;
        if(b[2]&&b[2]==b[4]&&b[2]==b[6])win=1;
        if(win){printf("%c wins!\n",player==1?'X':'O');break;}
        player=player==1?2:1;
    }
    return 0;
}
