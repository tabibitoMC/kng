/* kbd.c — ver kbd.h. Sin dependencias del kernel: se prueba tal cual en el PC (tools/kbdtest.c). */
#include "kbd.h"

#define DEAD_ACUTE 1
#define DEAD_DIAER 2
typedef struct { uint8_t sc; unsigned char n, s, a; } Key;      /* tecla: normal, con Shift, con AltGr */

/* base: distribucion US (set 1) hasta la tecla 0x35, mas '<' en 0x56 */
static const unsigned char US_N[0x57]={0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
 'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s','d','f','g','h','j','k','l',';','\'','`',
 0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '};
static const unsigned char US_S[0x57]={0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
 'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S','D','F','G','H','J','K','L',':','"','~',
 0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '};

static const Key LATAM[]={
 {0x29,'|',0xB0,0xAC},{0x02,'1','!','|'},{0x03,'2','"','@'},{0x04,'3','#',0},{0x05,'4','$',0},{0x06,'5','%',0},{0x07,'6','&',0},
 {0x08,'7','/',0},{0x09,'8','(',0},{0x0A,'9',')',0},{0x0B,'0','=',0},{0x0C,'\'','?','\\'},{0x0D,0xBF,0xA1,0},
 {0x10,'q','Q','@'},{0x1A,DEAD_ACUTE,DEAD_DIAER,0},{0x1B,'+','*','~'},{0x27,0xF1,0xD1,0},{0x28,'{','[','^'},{0x2B,'}',']','`'},
 {0x33,',',';',0},{0x34,'.',':',0},{0x35,'-','_',0},{0x56,'<','>',0},{0,0,0,0}};
static const Key ES[]={
 {0x29,0xBA,0xAA,'\\'},{0x02,'1','!','|'},{0x03,'2','"','@'},{0x04,'3',0xB7,'#'},{0x05,'4','$','~'},{0x06,'5','%',0},{0x07,'6','&',0xAC},
 {0x08,'7','/',0},{0x09,'8','(',0},{0x0A,'9',')',0},{0x0B,'0','=',0},{0x0C,'\'','?',0},{0x0D,0xA1,0xBF,0},
 {0x1A,'`','^','['},{0x1B,'+','*',']'},{0x27,0xF1,0xD1,0},{0x28,DEAD_ACUTE,DEAD_DIAER,'{'},{0x2B,0xE7,0xC7,'}'},
 {0x33,',',';',0},{0x34,'.',':',0},{0x35,'-','_',0},{0x56,'<','>',0},{0,0,0,0}};

static int layout, shift, caps, altgr, e0, dead, ctrl;
void kbd_set_layout(int l){ layout=(l>=0&&l<=2)?l:0; dead=0; }
int  kbd_layout(void){ return layout; }
const char *kbd_layout_name(int l){ return l==KBD_LATAM?"Latinoamerica":l==KBD_ES?"Espana":"US"; }

static int lookup(int sc){                    /* caracter de la tecla segun capa; 0 si no hay */
    if(sc>=0x57) return 0;
    int n=US_N[sc], s=US_S[sc], a=0;
    const Key *t=layout==KBD_LATAM?LATAM:layout==KBD_ES?ES:0;
    if(t) for(;t->sc;t++) if(t->sc==sc){ n=t->n; s=t->s; a=t->a; break; }
    if(altgr && a) return a;
    int up=shift;
    if(caps && ((n>='a'&&n<='z')||n==0xF1||n==0xE7)) up=!up;       /* Mayus solo afecta a letras */
    return up?s:n;
}
static int compose(int dk,int c){
    static const char V[]="aeiouAEIOU";
    static const unsigned char AC[]={0xE1,0xE9,0xED,0xF3,0xFA,0xC1,0xC9,0xCD,0xD3,0xDA};
    static const unsigned char DI[]={0xE4,0xEB,0xEF,0xF6,0xFC,0xC4,0xCB,0xCF,0xD6,0xDC};
    for(int i=0;V[i];i++) if(c==V[i]) return dk==DEAD_ACUTE?AC[i]:DI[i];
    if(c==' ') return dk==DEAD_ACUTE?0xB4:0xA8;
    return c;
}
int kbd_feed(uint8_t b){
    if(b==0xE0){ e0=1; return 0; }
    int rel=b&0x80, sc=b&0x7F;
    if(e0){
        e0=0;
        if(sc==0x38){ altgr=!rel; return 0; }                         /* AltGr */
        if(sc==0x1D){ ctrl=!rel; return 0; }                          /* Ctrl derecho */
        if(rel) return 0;
        switch(sc){ case 0x48: return 0x11; case 0x50: return 0x12; case 0x4B: return 0x13;
                    case 0x4D: return 0x14; case 0x49: return 0x15; case 0x51: return 0x16; case 0x53: return 0x7F; }
        return 0;
    }
    if(sc==0x2A||sc==0x36){ shift=!rel; return 0; }
    if(sc==0x1D){ ctrl=!rel; return 0; }                              /* Ctrl izquierdo */
    if(rel) return 0;
    if(sc==0x3A){ caps=!caps; return 0; }
    int c=lookup(sc); if(!c) return 0;
    if(ctrl&&((c>='a'&&c<='z')||(c>='A'&&c<='Z'))) return 0x400|(c&31);    /* Ctrl+letra: codigo aparte para no chocar con las flechas */
    if(c==DEAD_ACUTE||c==DEAD_DIAER){ if(dead){ int d=dead; dead=0; return d==DEAD_ACUTE?0xB4:0xA8; } dead=c; return 0; }
    if(dead){ int d=dead; dead=0; return compose(d,c); }
    return c;
}
