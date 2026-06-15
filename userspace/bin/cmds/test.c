/* test.c — evaluate expression */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    if(argc<2)return 1;
    if(strcmp(argv[1],"!")==0)return !main(argc-1,argv+1);
    if(argc==4&&strcmp(argv[2],"=")==0)return strcmp(argv[1],argv[3])!=0;
    if(argc==4&&strcmp(argv[2],"!=")==0)return strcmp(argv[1],argv[3])==0;
    if(argc==4&&strcmp(argv[2],"-eq")==0)return atoi(argv[1])!=atoi(argv[3]);
    if(argc==4&&strcmp(argv[2],"-ne")==0)return atoi(argv[1])==atoi(argv[3]);
    if(argc==4&&strcmp(argv[2],"-lt")==0)return atoi(argv[1])>=atoi(argv[3]);
    if(argc==4&&strcmp(argv[2],"-le")==0)return atoi(argv[1])>atoi(argv[3]);
    if(argc==4&&strcmp(argv[2],"-gt")==0)return atoi(argv[1])<=atoi(argv[3]);
    if(argc==4&&strcmp(argv[2],"-ge")==0)return atoi(argv[1])<atoi(argv[3]);
    if(argc==3&&strcmp(argv[1],"-f")==0){
        struct stat st;return stat(argv[2],&st)<0;
    }
    if(argc==3&&strcmp(argv[1],"-d")==0){
        struct stat st;return stat(argv[2],&st)<0||!(st.st_mode&040000);
    }
    return 0;
}
