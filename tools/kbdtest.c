/* kbdtest.c — prueba del traductor de teclado en el PC: make/break de verdad, Mayus, AltGr, teclas muertas. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../kernel/kbd.c"
static int fails;
static void chk(int got,int want,const char *what){ if(got!=want){ printf("FALLO %s: obtuve 0x%x, esperaba 0x%x\n",what,got,want); fails++; } }
static int press(int sc){ int r=kbd_feed((uint8_t)sc); kbd_feed((uint8_t)(sc|0x80)); return r; }
static int with_shift(int sc){ kbd_feed(0x2A); int r=press(sc); kbd_feed(0xAA); return r; }
static int with_altgr(int sc){ kbd_feed(0xE0); kbd_feed(0x38); int r=press(sc); kbd_feed(0xE0); kbd_feed(0xB8); return r; }
int main(void){
    kbd_set_layout(KBD_US);
    chk(press(0x1E),'a',"US a"); chk(with_shift(0x1E),'A',"US A"); chk(with_shift(0x08),'&',"US shift+7"); chk(press(0x35),'/',"US /");
    kbd_feed(0x3A); kbd_feed(0xBA); chk(press(0x1E),'A',"US caps a"); chk(with_shift(0x1E),'a',"US caps+shift a"); chk(press(0x02),'1',"caps no afecta numeros"); kbd_feed(0x3A); kbd_feed(0xBA);
    kbd_set_layout(KBD_LATAM);
    chk(with_shift(0x08),'/',"LATAM shift+7 = /"); chk(with_shift(0x0B),'=',"LATAM shift+0 = ="); chk(press(0x0C),'\'',"LATAM '");
    chk(with_shift(0x0C),'?',"LATAM ?"); chk(with_altgr(0x10),'@',"LATAM altgr+q = @"); chk(with_altgr(0x0C),'\\',"LATAM altgr+' = \\");
    chk(press(0x27),0xF1,"LATAM n con tilde"); chk(with_shift(0x27),0xD1,"LATAM N con tilde"); chk(with_shift(0x03),'"',"LATAM shift+2 = \"");
    chk(with_shift(0x34),':',"LATAM :"); chk(press(0x35),'-',"LATAM -"); chk(with_shift(0x35),'_',"LATAM _"); chk(press(0x34),'.',"LATAM .");
    chk(press(0x1A),0,"LATAM tecla muerta no emite"); chk(press(0x12),0xE9,"LATAM acento + e = e con tilde");
    press(0x1A); chk(press(0x1E),0xE1,"LATAM acento + a"); kbd_feed(0x2A); press(0x1A); kbd_feed(0xAA); chk(press(0x16),0xFC,"LATAM dieresis + u");
    press(0x1A); chk(press(0x39),0xB4,"LATAM acento + espacio"); press(0x1A); chk(press(0x30),'b',"LATAM acento + b = b");
    chk(with_altgr(0x28),'^',"LATAM altgr+{ = ^"); chk(press(0x28),'{',"LATAM {"); chk(with_shift(0x1B),'*',"LATAM *"); chk(with_altgr(0x1B),'~',"LATAM ~");
    kbd_set_layout(KBD_ES);
    chk(press(0x27),0xF1,"ES n"); chk(with_altgr(0x03),'@',"ES altgr+2 = @"); chk(press(0x2B),0xE7,"ES c cedilla"); chk(with_shift(0x08),'/',"ES /");
    chk(with_altgr(0x04),'#',"ES altgr+3 = #"); chk(press(0x56),'<',"ES <"); chk(press(0x34),'.',"ES .");
    kbd_set_layout(KBD_US);
    kbd_feed(0xE0); chk(kbd_feed(0x48),0x11,"flecha arriba"); kbd_feed(0xE0); kbd_feed(0xC8);
    kbd_feed(0xE0); chk(kbd_feed(0x51),0x16,"Av Pag"); kbd_feed(0xE0); kbd_feed(0xD1);
    if(fails){ printf("%d fallos\n",fails); return 1; }
    puts("KBD OK"); return 0;
}
