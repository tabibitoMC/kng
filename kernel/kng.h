/* kng.h — API minima que el kernel ofrece a la interfaz grafica y a las apps (tambien la usa el simulador del host). */
#ifndef KNG_H
#define KNG_H
#include <stdint.h>
#include <stddef.h>
#define KNG_VERSION "0.7"
#ifndef KNG_ARCH
#define KNG_ARCH "i386, 32 bits"
#endif
void *kmalloc(size_t n);
void  kfree(void *p);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void  kprintf(const char *f, ...);
uint32_t kticks(void);                 /* ticks del timer (100 Hz) */
int   kgetc(void);                     /* -1 si no hay tecla */
void  kshell_exec(char *line);         /* ejecuta un comando del shell del kernel (salida por putc) */
void  kreboot(void);
void  khalt(void);
int   krtc(int *h, int *m);            /* hora del RTC (CMOS); 0 si ok */
uint32_t kram_kb(void);
extern void (*kcon_hook)(char);        /* si no es NULL, la salida de consola va a esta funcion */
/* programas "pegados" a la ventana de terminal (kernel de 64 bits): corren en segundo plano de verdad y reciben las teclas de la ventana */
extern int  kattach_pid;               /* pid del programa que acaba de lanzar el ultimo comando (0 = ninguno); lo pone el shell y lo lee la ventana */
extern char kshell_cwd[96];            /* carpeta actual de la ventana que lanza el comando (la heredan los programas) */
extern void (*kproc_done)(int pid,int code);   /* se llama al terminar un programa pegado a la terminal */
void  kinput_event(int type,int code,int value);   /* evento de entrada (formato evdev) hacia /dev/input/event0 */
void  kproc_key(int c);                /* tecla de la ventana de terminal hacia el programa (codigos Latin-1, flechas 0x11-0x16, Ctrl+x = 0x400|x) */

static inline int k_strlen(const char *s){ int n=0; while(s[n]) n++; return n; }
static inline int k_strcmp(const char *a,const char *b){ while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
static inline void k_strcpy(char *d,const char *s,int max){ int i=0; for(;i<max-1&&s[i];i++) d[i]=s[i]; d[i]=0; }
static inline int k_startswith(const char *s,const char *p){ while(*p){ if(*s++!=*p++) return 0; } return 1; }
static inline int k_min(int a,int b){ return a<b?a:b; }
static inline int k_max(int a,int b){ return a>b?a:b; }
#endif
