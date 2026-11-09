#include "ulib.h"
void _start(void){
    puts_("[crash] voy a escribir en memoria del kernel...\n");
    *(volatile int*)0x1000 = 1;      /* debe provocar falta de pagina y matar SOLO a este proceso */
    puts_("[crash] ERROR: esto no debe verse\n");
    exit(0);
}
