/* cal.c — display calendar with actual date computation */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static int is_leap(int y) {
    return (y%4==0 && y%100!=0) || (y%400==0);
}

static int month_days(int m, int y) {
    static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(m==2 && is_leap(y)) return 29;
    return days[m-1];
}

/* Zeller's congruence to find day of week (0=Sun) */
static int day_of_week(int y, int m, int d) {
    if(m<3){m+=12;y--;}
    int k=y%100,j=y/100;
    int dow=(d+(13*(m+1))/5+k+k/4+j/4+5*j)%7;
    return (dow+6)%7;
}

static void get_current_date(int *year, int *month, int *day) {
    struct timespec ts;
    if(clock_gettime(0,&ts)<0){
        *year=2026; *month=7; *day=1;
        return;
    }
    unsigned long long secs=ts.tv_sec;
    int y=1970,m=1,d=1;
    while(1){
        int dys=(y%4==0&&(y%100!=0||y%400==0))?366:365;
        if(secs<(unsigned long long)dys*86400) break;
        secs-=dys*86400; y++;
    }
    static const int md[]={31,28,31,30,31,30,31,31,30,31,30,31};
    while(1){
        int dim=md[m-1];
        if(m==2&&(y%4==0&&(y%100!=0||y%400==0))) dim=29;
        if(secs<(unsigned long long)dim*86400) break;
        secs-=dim*86400; m++;
    }
    d=(int)(secs/86400)+1;
    *year=y; *month=m; *day=d;
}

int main(int argc,char*argv[]){
    int year=0,month=0;
    int cur_year,cur_month,cur_day;
    get_current_date(&cur_year,&cur_month,&cur_day);
    (void)cur_day;

    if(argc==2){
        month=atoi(argv[1]);
        if(month<1||month>12){year=month;month=0;}
    }else if(argc>=3){
        month=atoi(argv[1]);
        year=atoi(argv[2]);
    }

    if(year==0) year=cur_year;
    if(month==0) month=cur_month;
    if(month<1||month>12){printf("cal: invalid month\n");return 1;}

    static const char *months[]={"January","February","March","April","May","June",
                                  "July","August","September","October","November","December"};

    int spaces=(20-(int)strlen(months[month-1])-4)/2;
    if(spaces<0) spaces=0;
    for(int i=0;i<spaces;i++) printf(" ");
    printf("%s %d\n",months[month-1],year);
    printf("Su Mo Tu We Th Fr Sa\n");

    int dim=month_days(month,year);
    int start_dow=day_of_week(year,month,1);

    for(int i=0;i<start_dow;i++) printf("   ");
    for(int d=1;d<=dim;d++){
        printf("%2d ",d);
        if((d+start_dow)%7==0&&d<dim) printf("\n");
    }
    printf("\n");
    return 0;
}
