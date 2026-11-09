#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(int argc,char **argv){
    printf("hola desde glibc estatica (static-pie) sobre kng, argc=%d\n",argc);
    for(int i=0;i<argc;i++) printf("  argv[%d]=%s\n",i,argv[i]);
    char *p=malloc(1<<20); if(!p){ puts("malloc FALLO"); return 1; }
    memset(p,7,1<<20); long s=0; for(int i=0;i<(1<<20);i+=4096) s+=p[i]; printf("malloc 1 MB ok (%ld)\n",s); free(p);
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); printf("reloj ok (%ld s)\n",(long)t.tv_sec);
    return 0;
}
