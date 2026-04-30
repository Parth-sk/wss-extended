#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>

int main()
{
    static struct timeval ts1, ts2;
    gettimeofday(&ts1, NULL);
    printf("Current time: %ld seconds and %ld microseconds\n", ts1.tv_sec, ts1.tv_usec);
    
    return 0;
}
