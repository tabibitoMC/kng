#include "ulib.h"
void _start(void){
    puts_("[utest] hola desde ring 3, pid="); putnum(getpid()); puts_("\n");
    puts_("[utest] write(puntero del kernel) -> "); putnum(write(1,(const void*)0x1000,4)); puts_("\n");
    for(int i=1;i<=3;i++){ puts_("[utest] paso "); putnum(i); puts_("/3\n"); sleep_ms(100); }
    exit(42);
}
