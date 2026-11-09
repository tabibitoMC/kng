#include "ulib.h"
static int g=1;
void _start(void){
    int bad=0, st=0, w;
    int pid=fork();
    if(pid==0){ g=2; puts_("[forktest] hijo 1: exec utest\n"); exec("utest"); exit(99); }
    w=waitpid(pid,&st,0);
    puts_("[forktest] hijo 1 status="); putnum(st>>8); puts_("\n");
    if(w!=pid||(st>>8)!=42) bad|=1;
    if(g!=1) bad|=2;                                   /* el hijo no debe cambiar mi memoria */
    pid=fork();
    if(pid==0){ g=5; exit(g==5?7:1); }
    w=waitpid(pid,&st,0);
    if(w!=pid||(st>>8)!=7) bad|=4;
    if(g!=1) bad|=8;
    u32 b0=(u32)brk(0), b1=(u32)brk(b0+8192);
    if(b1!=b0+8192) bad|=16;
    else {
        *(volatile int*)b0=1234; *(volatile int*)(b0+4096)=5678;
        if(*(volatile int*)b0!=1234||*(volatile int*)(b0+4096)!=5678){ bad|=32; }
        brk(b0);
    }
    if(waitpid(-1,&st,0)!=-10) bad|=64;                /* sin hijos: ECHILD */
    puts_("[forktest] resultado="); putnum(bad); puts_(bad?" FALLO\n":" OK\n");
    exit(bad);
}
