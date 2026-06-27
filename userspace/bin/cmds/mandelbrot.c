
#include <stdio.h>
int main(void){
    printf("Mandelbrot Set\n");
    for(int y=0;y<24;y++){
        for(int x=0;x<80;x++){
            double zx=0,zy=0,cx=(double)(x-40)/30,cy=(double)(y-12)/15;
            int i;for(i=0;i<100;i++){double nx=zx*zx-zy*zy+cx;zy=2*zx*zy+cy;zx=nx;if(zx*zx+zy*zy>4)break;}
            putchar(i>=100?'@':(i>20?'#':(i>5?'.':' ')));
        }
        printf("\n");
    }
    return 0;
}
