/* kbd.h — traduccion de teclado PS/2 (set 1) a caracteres Latin-1: distribuciones US, Latinoamerica y Espana.
 * Soporta Mayus (Caps Lock), AltGr, teclas muertas (acento agudo y dieresis) y las flechas / Re Pag / Av Pag. */
#ifndef KBD_H
#define KBD_H
#include "kng.h"
enum { KBD_US=0, KBD_LATAM=1, KBD_ES=2 };
void        kbd_set_layout(int l);
int         kbd_layout(void);
const char *kbd_layout_name(int l);
int         kbd_feed(uint8_t byte);   /* le das cada byte que llega del teclado; devuelve un codigo de caracter (0 = ninguno aun) */
#endif
