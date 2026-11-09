#include "ulib.h"
static int streq(const char*a,const char*b){ while(*a&&*a==*b){a++;b++;} return *a==*b; }
void _start(void){
    char line[64];
    puts_("\nkng-sh: shell de usuario (ring 3). help | pid | exit | utest crash forktest\n");
    for(;;){
        puts_("kng$ ");
        int n=0;
        for(;;){
            char c; if(read(0,&c,1)!=1) continue;
            if(c=='\n'){ write(1,"\n",1); break; }
            if(c=='\b'){ if(n){ n--; write(1,"\b \b",3); } continue; }
            if(c>=32 && n<63){ line[n++]=c; write(1,&c,1); }
        }
        line[n]=0;
        if(!n) continue;
        if(streq(line,"exit")){ puts_("adios\n"); exit(0); }
        if(streq(line,"help")){ puts_("help pid exit | programas: utest crash forktest\n"); continue; }
        if(streq(line,"pid")){ putnum(getpid()); puts_(" (padre "); putnum(getppid()); puts_(")\n"); continue; }
        int pid=fork();
        if(pid<0){ puts_("fork fallo\n"); continue; }
        if(pid==0){ exec(line); puts_("kng-sh: no existe: "); puts_(line); puts_("\n"); exit(127); }
        int st=0; waitpid(pid,&st,0);
        puts_("[estado "); putnum(st>>8); puts_((st&0x7F)?" (matado por excepcion)]\n":"]\n");
    }
}
