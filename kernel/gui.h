#ifndef GUI_H
#define GUI_H
#include "kng.h"
#define KEY_UP 0x11
#define KEY_DOWN 0x12
#define KEY_LEFT 0x13
#define KEY_RIGHT 0x14
#define KEY_PGUP 0x15
#define KEY_PGDN 0x16
int  gui_init(uint32_t *fb, int w, int h, int pitch_bytes);   /* 0 ok */
void gui_mouse_event(int dx, int dy, int buttons);            /* movimiento relativo (PS/2) */
void gui_mouse_abs(int x, int y, int buttons);                /* posicion absoluta (pruebas) */
void gui_key(int c);
void gui_frame(void);                                          /* dibuja si hace falta */
void gui_rawkey(int byte);                                     /* byte crudo del teclado (set 1) para la pantalla X */
void gui_open_xscreen(int w,int h);                            /* abre la ventana de la pantalla X con ese tamano */
int  gui_screen_size(int *w,int *h);                           /* tamano de la pantalla del escritorio; -1 si no hay */
void gui_force_redraw(void);
int  gui_ready(void);
void gui_panic(const char *name,const char *ipname,uint64_t ip,uint64_t err,uint64_t cr2,int has_cr2);   /* pantalla de error fatal */
#endif
