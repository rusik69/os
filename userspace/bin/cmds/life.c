
#include <stdio.h>
#include <unistd.h>
#include <string.h>
int main(void){
    int w=40,h=20;char g[20][40]={0},gb[20][40]={0};
    // Glider
    g[5][5]=1;g[5][6]=1;g[5][7]=1;g[6][5]=1;g[7][6]=1;
    printf("\033[2JConway's Game of Life (#=live) any key=step q=quit\n");
    for(int gen=0;;gen++){
        printf("\033[HGen %d\n",gen);
        for(int r=0;r<h;r++){for(int c=0;c<w;c++)putchar(g[r][c]?'#':' ');putchar('\n');}
        char b[16];int n=read(0,b,15);if(n>0){b[n]=0;if(b[0]=='q')break;}
        memcpy(gb,g,sizeof(g));
        for(int r=0;r<h;r++)for(int c=0;c<w;c++){
            int n=0;
            for(int dr=-1;dr<=1;dr++)for(int dc=-1;dc<=1;dc++){
                if(dr==0&&dc==0)continue;
                int nr=r+dr,nc=c+dc;
                if(nr>=0&&nr<h&&nc>=0&&nc<w&&gb[nr][nc])n++;}
            if(gb[r][c])g[r][c]=(n==2||n==3)?1:0;
            else g[r][c]=(n==3)?1:0;}
    }
    return 0;
}
