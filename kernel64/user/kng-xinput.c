/* kng-xinput.c — puente de entrada para el servidor X (Xvfb) en kng.
 * Lee los eventos del raton y del teclado que la ventana "Pantalla X" de kng pone en /dev/input/event0 (formato evdev de Linux)
 * y se los entrega al servidor X con la extension XTEST, hablando el protocolo X11 directamente (no necesita libX11).
 * Uso: kng-xinput [display]   (por omision 0, el socket /tmp/.X11-unix/X0). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

struct ev { long sec, usec; uint16_t type, code; int32_t value; };
static int xs;
static int rd_all(int fd,void *b,size_t n){ size_t d=0; while(d<n){ ssize_t r=read(fd,(char*)b+d,n-d); if(r<=0){ if(r<0&&errno==EINTR) continue; return -1; } d+=(size_t)r; } return 0; }
static int wr_all(int fd,const void *b,size_t n){ size_t d=0; while(d<n){ ssize_t r=write(fd,(const char*)b+d,n-d); if(r<=0){ if(r<0&&errno==EINTR) continue; return -1; } d+=(size_t)r; } return 0; }

static int xconnect(int disp){
    int s=socket(AF_UNIX,SOCK_STREAM,0); if(s<0) return -1;
    struct sockaddr_un a; memset(&a,0,sizeof a); a.sun_family=AF_UNIX; snprintf(a.sun_path,sizeof a.sun_path,"/tmp/.X11-unix/X%d",disp);
    if(connect(s,(struct sockaddr*)&a,sizeof a)<0){ close(s); return -1; }
    uint8_t req[12]={'l',0,11,0,0,0,0,0,0,0,0,0};                       /* orden little-endian, protocolo 11.0, sin autorizacion (Xvfb -ac) */
    if(wr_all(s,req,sizeof req)) { close(s); return -1; }
    uint8_t h[8]; if(rd_all(s,h,8)||h[0]!=1){ close(s); return -1; }
    size_t extra=(size_t)(h[6]|(h[7]<<8))*4; uint8_t *buf=malloc(extra?extra:1); if(!buf||rd_all(s,buf,extra)){ close(s); return -1; }
    free(buf); return s;
}
static int xtest_opcode(void){
    uint8_t req[16]; memset(req,0,sizeof req); req[0]=98; req[2]=4; req[4]=5; memcpy(req+8,"XTEST",5);   /* QueryExtension: 8 bytes de cabecera + nombre rellenado a 8 = 16 bytes (4 palabras) */
    if(wr_all(xs,req,16)) return -1;
    uint8_t r[32]; if(rd_all(xs,r,32)||r[0]!=1||!r[8]) return -1;
    return r[9];
}
static int xop;
static void fake(int type,int detail,int x,int y){
    uint8_t q[36]; memset(q,0,sizeof q); q[0]=(uint8_t)xop; q[1]=2; q[2]=9; q[4]=(uint8_t)type; q[5]=(uint8_t)detail;
    q[24]=(uint8_t)(x&255); q[25]=(uint8_t)((x>>8)&255); q[26]=(uint8_t)(y&255); q[27]=(uint8_t)((y>>8)&255);
    wr_all(xs,q,sizeof q);
}
/* los errores que devuelve X (32 bytes) se descartan sin bloquear */
static void drain(void){ uint8_t b[512]; int fl=fcntl(xs,F_GETFL); fcntl(xs,F_SETFL,fl|O_NONBLOCK); while(read(xs,b,sizeof b)>0); fcntl(xs,F_SETFL,fl); }

int main(int argc,char **argv){
    int disp=argc>1?atoi(argv[1]):0;
    for(int i=0;i<200;i++){ xs=xconnect(disp); if(xs>=0) break; usleep(100000); }       /* espera a que el servidor X este listo (hasta 20 s) */
    if(xs<0){ fprintf(stderr,"kng-xinput: no se pudo conectar al servidor X :%d\n",disp); return 1; }
    xop=xtest_opcode(); if(xop<0){ fprintf(stderr,"kng-xinput: el servidor X no tiene XTEST\n"); return 1; }
    int fd=open("/dev/input/event0",O_RDONLY); if(fd<0){ perror("kng-xinput: /dev/input/event0"); return 1; }
    printf("kng-xinput: listo (X :%d, XTEST opcode %d)\n",disp,xop); fflush(stdout);
    int ax=0, ay=0, n=0; struct ev e;
    for(;;){
        if(rd_all(fd,&e,sizeof e)) break;
        if(e.type==3){ if(e.code==0) ax=e.value; else if(e.code==1) ay=e.value; }                    /* EV_ABS: posicion */
        else if(e.type==0&&e.code==0){ fake(6,0,ax,ay); }                                              /* EV_SYN: mover el puntero */
        else if(e.type==1){
            int down=e.value!=0;
            if(e.code>=0x110&&e.code<=0x112){ int b=e.code==0x110?1:e.code==0x112?2:3; fake(down?4:5,b,ax,ay); }     /* botones: izq, medio, der */
            else if(e.code<0x100){ fake(down?2:3,e.code+8,0,0); }                                                       /* teclas: codigo X = codigo evdev + 8 */
        }
        else if(e.type==2&&e.code==8){ int b=e.value>0?4:5; fake(4,b,ax,ay); fake(5,b,ax,ay); }                       /* rueda */
        if(++n%16==0) drain();
    }
    return 0;
}
