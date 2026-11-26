/* kng-xinput.c — puente entre el escritorio de kng y el servidor X (Xvfb). Habla el protocolo X11 directamente (sin libX11).
 *  1) Entrada: lee los eventos (formato evdev de Linux) que el escritorio pone en /dev/input/event0 y se los da a X con XTEST.
 *  2) Ventanas: cada ~150 ms lista las ventanas de nivel superior visibles (posicion, tamano, titulo) y las escribe en
 *     /tmp/.kng-xwin; el icono de cada programa (_NET_WM_ICON) va a /tmp/.kng-xicon-<id>. Asi el escritorio muestra cada
 *     programa de Linux en su propia ventana de kng (con su barra de titulo, en el dock, minimizable...).
 *  3) Ordenes del escritorio, en el mismo flujo de eventos: tipo 0x31 (parametro code = valor) y tipo 0x30
 *     (code 1 mover/cambiar tamano, 2 traer al frente y dar el teclado, 3 cerrar, 4 matar, 5 estado maximizada=par[0])
 *     con value = id de la ventana.
 *  4) Gestor de ventanas minimo (EWMH): asi los programas que dibujan su propio marco (Chromium, Brave, Chrome, las
 *     apps GTK con barra propia) piden mover, cambiar el tamano, maximizar o minimizar; esas peticiones van al escritorio
 *     en /tmp/.kng-xreq ("n tipo id dato": m mover/tamano(dato=direccion), x maximizar(0 quitar,1 poner,2 alternar),
 *     i minimizar, a activar). En la lista, emergente=2 quiere decir "sin marco de kng" (el programa trae el suyo).
 * Uso: kng-xinput [display]. Para pruebas: KNG_XDEV (dispositivo), KNG_XLIST (archivo de la lista). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

struct ev { long sec, usec; uint16_t type, code; int32_t value; };
static int xs; static uint16_t seq;
static int rd_all(int fd,void *b,size_t n){ size_t d=0; while(d<n){ ssize_t r=read(fd,(char*)b+d,n-d); if(r<=0){ if(r<0&&errno==EINTR) continue; return -1; } d+=(size_t)r; } return 0; }
static int wr_all(int fd,const void *b,size_t n){ size_t d=0; while(d<n){ ssize_t r=write(fd,(const char*)b+d,n-d); if(r<=0){ if(r<0&&errno==EINTR) continue; return -1; } d+=(size_t)r; } return 0; }
static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0]|p[1]<<8); }
static void put32(uint8_t *p,uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put16(uint8_t *p,uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint32_t root_win, rid_base;

static int xconnect(int disp){
    int s=socket(AF_UNIX,SOCK_STREAM,0); if(s<0) return -1;
    struct sockaddr_un a; memset(&a,0,sizeof a); a.sun_family=AF_UNIX; snprintf(a.sun_path,sizeof a.sun_path,"/tmp/.X11-unix/X%d",disp);
    if(connect(s,(struct sockaddr*)&a,sizeof a)<0){ close(s); return -1; }
    uint8_t req[12]={'l',0,11,0,0,0,0,0,0,0,0,0};                       /* orden little-endian, protocolo 11.0, sin autorizacion (Xvfb -ac) */
    if(wr_all(s,req,sizeof req)) { close(s); return -1; }
    uint8_t h[8]; if(rd_all(s,h,8)||h[0]!=1){ close(s); return -1; }
    size_t extra=(size_t)(h[6]|(h[7]<<8))*4; uint8_t *buf=malloc(extra?extra:1); if(!buf||rd_all(s,buf,extra)){ close(s); return -1; }
    /* raiz de la primera pantalla: tras la cabecera fija (32), el vendedor (relleno a 4) y los formatos (8 c/u) */
    rid_base=le32(buf+4);
    { size_t vlen=le16(buf+16), nfmt=buf[21]; size_t o=32+((vlen+3)&~3u)+nfmt*8; if(o+4<=extra) root_win=le32(buf+o); }
    free(buf); return s;
}
static int req(const void *b,size_t n){ seq++; return wr_all(xs,b,n); }
/* lee paquetes hasta la respuesta (o el error) de la peticion numero s; descarta eventos y errores de otras peticiones */
static uint8_t *rb; static size_t rbcap;
static uint8_t evq[64][32]; static int evn;                       /* eventos que atiende el gestor de ventanas */
static size_t read_packet(void){                                  /* un paquete completo en rb; devuelve su largo */
    uint8_t h[32]; if(rd_all(xs,h,32)){ fprintf(stderr,"kng-xinput: se cerro la conexion con X\n"); exit(1); }
    { int t=h[0]&0x7f; if((t==20||t==23||t==33)&&evn<64) memcpy(evq[evn++],h,32); }
    size_t extra=(h[0]==1||h[0]==35)?(size_t)le32(h+4)*4:0;
    if(32+extra>rbcap){ rbcap=32+extra+4096; rb=realloc(rb,rbcap); if(!rb) exit(1); }
    memcpy(rb,h,32); if(extra&&rd_all(xs,rb+32,extra)) exit(1);
    return 32+extra;
}
static uint8_t *reply(uint16_t s,size_t *len){
    for(;;){ size_t l=read_packet();
        if(le16(rb+2)==s){ if(rb[0]==1){ if(len) *len=l; return rb; } if(rb[0]==0) return 0; } }
}
static void skip_packet(void){ (void)read_packet(); }              /* error o evento suelto: se descarta */
static int xtest_opcode(void){
    uint8_t q[16]; memset(q,0,sizeof q); q[0]=98; q[2]=4; q[4]=5; memcpy(q+8,"XTEST",5);   /* QueryExtension */
    if(req(q,16)) return -1;
    uint8_t *r=reply(seq,0); if(!r||!r[8]) return -1;
    return r[9];
}
static int xop;
static void fake(int type,int detail,int x,int y){
    uint8_t q[36]; memset(q,0,sizeof q); q[0]=(uint8_t)xop; q[1]=2; q[2]=9; q[4]=(uint8_t)type; q[5]=(uint8_t)detail;
    put16(q+24,(uint16_t)x); put16(q+26,(uint16_t)y);
    req(q,sizeof q);
}
static uint32_t atom(const char *name){
    uint8_t q[64]; size_t n=strlen(name), pn=(n+3)&~3u; memset(q,0,sizeof q);
    q[0]=16; q[1]=0; put16(q+2,(uint16_t)(2+pn/4)); put16(q+4,(uint16_t)n); memcpy(q+8,name,n);
    req(q,8+pn); uint8_t *r=reply(seq,0); return r?le32(r+8):0;
}
static uint32_t A_NAME, A_UTF8, A_WMNAME, A_ICON, A_PROTO, A_DELETE, A_TYPE, A_TDOCK, A_TDESK;
static uint32_t A_MOTIF, A_STATE, A_MAXV, A_MAXH, A_FULL, A_HIDDEN, A_MOVERESIZE, A_ACTIVE, A_CLOSE, A_CHANGE, A_FRAME, A_REQFRAME, A_SUPPORTED, A_CHECK, A_WMSTATE;
/* GetProperty: devuelve los datos (o 0); *n = numero de elementos, *fmt = 8/16/32 */
static uint8_t *getprop(uint32_t w,uint32_t prop,uint32_t type,uint32_t maxlong,uint32_t *n,int *fmt){
    uint8_t q[24]; memset(q,0,sizeof q); q[0]=20; put16(q+2,6); put32(q+4,w); put32(q+8,prop); put32(q+12,type); put32(q+16,0); put32(q+20,maxlong);
    req(q,24); size_t l; uint8_t *r=reply(seq,&l); if(!r||!r[1]) return 0;
    *fmt=r[1]; *n=le32(r+16); return r+32;
}

/* ---------------- lista de ventanas ---------------- */
#define MAXV 64
typedef struct { uint32_t id; int x,y,w,h,ovr,seen,icon_try,csd; char title[96]; } XV;
static XV vs[MAXV]; static int nv;
static const char *list_path="/tmp/.kng-xwin";
static char last_out[MAXV*140];
static long now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000L+t.tv_nsec/1000000L; }
static void get_title(XV *v){
    uint32_t n; int f; uint8_t *d=getprop(v->id,A_NAME,A_UTF8,64,&n,&f);
    if(!d||f!=8) d=getprop(v->id,A_WMNAME,0,64,&n,&f);
    if(d&&f==8){ if(n>95) n=95; memcpy(v->title,d,n); v->title[n]=0; for(uint32_t i=0;i<n;i++) if((unsigned char)v->title[i]<32) v->title[i]=' '; }
    else if(!v->title[0]) strcpy(v->title,"Programa de Linux");
}
static void get_icon(XV *v){                                       /* el tamano mas cercano a 48 (sin pasar de 128) */
    uint32_t n; int f; uint8_t *d=getprop(v->id,A_ICON,6 /*CARDINAL*/,65536,&n,&f);
    if(!d||f!=32||n<3) return;
    uint32_t best=0, bw=0, i=0;
    while(i+2<=n){ uint32_t w=le32(d+i*4), h=le32(d+i*4+4); if(!w||!h||w>1024||h>1024||i+2+(uint64_t)w*h>n) break;
        int score=(int)w-48; if(score<0) score=-score*2; int bs=(int)bw-48; if(bs<0) bs=-bs*2;
        if(w<=128&&(!bw||score<bs)){ best=i; bw=w; }
        i+=2+w*h; }
    if(!bw) return;
    uint32_t w=le32(d+best*4), h=le32(d+best*4+4);
    char p[96]; snprintf(p,sizeof p,"/tmp/.kng-xicon-%x",v->id); char t[100]; snprintf(t,sizeof t,"%s.tmp",p);
    int fd=open(t,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd<0) return;
    uint8_t hd[8]; put32(hd,w); put32(hd+4,h); wr_all(fd,hd,8); wr_all(fd,d+best*4+8,(size_t)w*h*4); close(fd); rename(t,p);
    v->icon_try=99;
}
static unsigned nscan;
static void scan(void){
    nscan++;
    uint8_t q[8]; q[0]=15; q[1]=0; put16(q+2,2); put32(q+4,root_win); req(q,8);
    size_t l; uint8_t *r=reply(seq,&l); if(!r) return;
    int nc=le16(r+16); if(nc>1024) nc=1024;
    static uint32_t ch[1024]; for(int i=0;i<nc;i++) ch[i]=le32(r+32+i*4);
    /* atributos de todos (en tubo), luego geometria de los visibles */
    uint16_t s0=(uint16_t)(seq+1);
    for(int i=0;i<nc;i++){ uint8_t a[8]; a[0]=3; a[1]=0; put16(a+2,2); put32(a+4,ch[i]); req(a,8); }
    static uint8_t vis[1024], ovr[1024];
    for(int i=0;i<nc;i++){ uint8_t *ra=reply((uint16_t)(s0+i),0); vis[i]=ra&&ra[26]==2; ovr[i]=ra?ra[27]:0; }
    for(int i=0;i<nv;i++) vs[i].seen=0;
    int order[MAXV], no=0;
    for(int i=0;i<nc&&no<MAXV;i++){ if(!vis[i]) continue;
        uint8_t g[8]; g[0]=14; g[1]=0; put16(g+2,2); put32(g+4,ch[i]); req(g,8);
        uint8_t *rg=reply(seq,0); if(!rg) continue;
        int x=(int16_t)le16(rg+12), y=(int16_t)le16(rg+14), w=le16(rg+16), h=le16(rg+18);
        if(w<2||h<2) continue;
        int k=-1; for(int j=0;j<nv;j++) if(vs[j].id==ch[i]){ k=j; break; }
        if(k<0){ if(nv>=MAXV) continue; k=nv++; memset(&vs[k],0,sizeof vs[k]); vs[k].id=ch[i];
            if(!ovr[i]){ uint32_t n; int f; uint8_t *t=getprop(ch[i],A_TYPE,4 /*ATOM*/,8,&n,&f);         /* escritorio o panel: no son ventanas */
                if(t&&f==32) for(uint32_t z=0;z<n;z++){ uint32_t at=le32(t+z*4); if(at==A_TDOCK||at==A_TDESK) ovr[i]=9; } } }
        XV *v=&vs[k]; v->x=x; v->y=y; v->w=w; v->h=h; v->ovr=ovr[i]; v->seen=1;
        if(!v->ovr&&(!v->title[0]||nscan%8==0)){ get_title(v);
            uint32_t n; int f; uint8_t *m=getprop(v->id,A_MOTIF,0,5,&n,&f);                         /* _MOTIF_WM_HINTS: sin decoraciones = trae su marco */
            v->csd=m&&f==32&&n>=3&&(le32(m)&2)&&le32(m+8)==0; }
        if(!v->ovr&&v->icon_try<6){ v->icon_try++; get_icon(v); }
        order[no++]=k;
    }
    static char out[sizeof last_out]; int o=snprintf(out,sizeof out,"kngx 1\n");
    for(int k=0;k<no&&o<(int)sizeof out-200;k++){ XV *v=&vs[order[k]];
        if(v->ovr==9) continue;
        o+=snprintf(out+o,sizeof out-o,"%x %d %d %d %d %d %s\n",v->id,v->x,v->y,v->w,v->h,v->ovr?1:v->csd?2:0,v->ovr?"":v->title); }
    if(strcmp(out,last_out)){
        char t[128]; snprintf(t,sizeof t,"%s.tmp",list_path);
        int fd=open(t,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){ wr_all(fd,out,(size_t)o); close(fd); rename(t,list_path); strcpy(last_out,out); } }
    for(int i=0;i<nv;){ if(!vs[i].seen){ char p[64]; snprintf(p,sizeof p,"/tmp/.kng-xicon-%x",vs[i].id); unlink(p); vs[i]=vs[--nv]; } else i++; }   /* las que ya no estan */
}
void setprop32(uint32_t w,uint32_t prop,uint32_t type,const uint32_t *v,int n);
/* ---------------- ordenes del escritorio ---------------- */
static int32_t par[4];
static void cmd(int code,uint32_t w){
    if(code==1){                                                   /* mover / cambiar tamano */
        uint8_t q[28]; memset(q,0,sizeof q); int n=0; uint16_t mask=1|2; put32(q+12,(uint32_t)par[0]); put32(q+16,(uint32_t)par[1]); n=2;
        if(par[2]>0&&par[3]>0){ mask|=4|8; put32(q+20,(uint32_t)par[2]); put32(q+24,(uint32_t)par[3]); n=4; }
        q[0]=12; put16(q+2,(uint16_t)(3+n)); put32(q+4,w); put16(q+8,mask); req(q,12+n*4);
    } else if(code==2){                                            /* al frente + foco del teclado */
        uint8_t q[16]; memset(q,0,sizeof q); q[0]=12; put16(q+2,4); put32(q+4,w); put16(q+8,64); put32(q+12,0); req(q,16);
        uint8_t f[12]; memset(f,0,sizeof f); f[0]=42; f[1]=1; put16(f+2,3); put32(f+4,w); req(f,12);
    } else if(code==3){                                            /* cerrar: WM_DELETE_WINDOW si lo entiende; si no, matar */
        uint32_t n; int fm; uint8_t *p=getprop(w,A_PROTO,4,32,&n,&fm); int ok=0;
        if(p&&fm==32) for(uint32_t i=0;i<n;i++) if(le32(p+i*4)==A_DELETE) ok=1;
        if(ok){ uint8_t q[44]; memset(q,0,sizeof q); q[0]=25; put16(q+2,11); put32(q+4,w); put32(q+8,0);
            uint8_t *e=q+12; e[0]=33; e[1]=32; put32(e+4,w); put32(e+8,A_PROTO); put32(e+12,A_DELETE); req(q,44); }
        else code=4;
    }
    if(code==4){ uint8_t q[8]; q[0]=113; q[1]=0; put16(q+2,2); put32(q+4,w); req(q,8); }
    if(code==5){ uint32_t v[2]={A_MAXV,A_MAXH}; setprop32(w,A_STATE,4,v,par[0]?2:0); }
}
/* ---------------- gestor de ventanas minimo ---------------- */
void setprop32(uint32_t w,uint32_t prop,uint32_t type,const uint32_t *v,int n){       /* ChangeProperty (reemplazar), formato 32 */
    uint8_t q[24+4*16]; if(n>16) n=16; memset(q,0,sizeof q); q[0]=18; q[1]=0; put16(q+2,(uint16_t)(6+n)); put32(q+4,w); put32(q+8,prop); put32(q+12,type); q[16]=32; put32(q+20,(uint32_t)n);
    for(int i=0;i<n;i++){ put32(q+24+i*4,v[i]); } req(q,24+4*(size_t)n);
}
static void setprop8(uint32_t w,uint32_t prop,uint32_t type,const char *s8){
    uint8_t q[128]; size_t n=strlen(s8), pn=(n+3)&~3u; memset(q,0,sizeof q); q[0]=18; put16(q+2,(uint16_t)(6+pn/4)); put32(q+4,w); put32(q+8,prop); put32(q+12,type); q[16]=8; put32(q+20,(uint32_t)n); memcpy(q+24,s8,n); req(q,24+pn);
}
static const char *req_path="/tmp/.kng-xreq";
static char reqs[16][48]; static int nreq; static unsigned reqn;
static void report(char kind,uint32_t id,int arg){                 /* peticion para el escritorio */
    reqn++; if(nreq==16){ memmove(reqs,reqs+1,sizeof reqs[0]*15); nreq=15; }
    snprintf(reqs[nreq++],sizeof reqs[0],"%u %c %x %d\n",reqn,kind,id,arg);
    char t[128]; snprintf(t,sizeof t,"%s.tmp",req_path); int fd=open(t,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd<0) return;
    for(int i=0;i<nreq;i++){ wr_all(fd,reqs[i],strlen(reqs[i])); } close(fd); rename(t,req_path);
}
static int wm_on;
static void wm_start(void){
    uint8_t q[16]; memset(q,0,sizeof q); q[0]=2; put16(q+2,4); put32(q+4,root_win); put32(q+8,1u<<11); put32(q+12,(1u<<19)|(1u<<20));   /* SubstructureNotify|Redirect */
    req(q,16);
    uint8_t g[8]; g[0]=43; g[1]=0; put16(g+2,1); req(g,4);                                        /* GetInputFocus: para saber si hubo error */
    uint16_t s1=(uint16_t)(seq-1); for(;;){ (void)read_packet(); if(le16(rb+2)==s1&&rb[0]==0){ fprintf(stderr,"kng-xinput: ya hay otro gestor de ventanas\n"); return; } if(le16(rb+2)==seq&&rb[0]==1) break; }
    wm_on=1;
    /* ventana de comprobacion EWMH (solo entrada, fuera de la pantalla) */
    uint32_t chk=rid_base|0x10; uint8_t c[32];
    memset(c,0,sizeof c); c[0]=1; c[1]=0; put16(c+2,8); put32(c+4,chk); put32(c+8,root_win); put16(c+12,(uint16_t)-100); put16(c+14,(uint16_t)-100); put16(c+16,1); put16(c+18,1); put16(c+20,0); put16(c+22,2); put32(c+24,0); put32(c+28,0);
    req(c,32);
    setprop32(root_win,A_CHECK,33 /*WINDOW*/,&chk,1); setprop32(chk,A_CHECK,33,&chk,1); setprop8(chk,A_NAME,A_UTF8,"kng");
    uint32_t sup[16]={A_SUPPORTED,A_CHECK,A_NAME,A_STATE,A_MAXV,A_MAXH,A_FULL,A_HIDDEN,A_MOVERESIZE,A_ACTIVE,A_CLOSE,A_FRAME,A_REQFRAME,A_ICON};
    setprop32(root_win,A_SUPPORTED,4,sup,14);
    printf("kng-xinput: gestor de ventanas activo\n"); fflush(stdout);
}
static void wm_events(void){
    while(evn>0){ uint8_t e[32]; memcpy(e,evq[0],32); memmove(evq,evq+1,sizeof evq[0]*(size_t)(--evn)); int t=e[0]&0x7f;
        if(t==20){ uint32_t w=le32(e+8); uint32_t st[2]={1,0}; uint32_t fe[4]={0,0,0,0};          /* MapRequest */
            setprop32(w,A_WMSTATE,A_WMSTATE,st,2); setprop32(w,A_FRAME,6,fe,4);
            uint8_t m[8]; m[0]=8; m[1]=0; put16(m+2,2); put32(m+4,w); req(m,8);
            uint8_t f[12]; memset(f,0,sizeof f); f[0]=42; f[1]=1; put16(f+2,3); put32(f+4,w); req(f,12); }
        else if(t==23){ uint32_t w=le32(e+8); uint16_t mask=le16(e+26)&(1|2|4|8|16); uint8_t q[32]; int n=0;     /* ConfigureRequest: se cumple tal cual */
            memset(q,0,sizeof q); q[0]=12; put32(q+4,w); put16(q+8,mask);
            if(mask&1) put32(q+12+4*n++,(uint32_t)(int32_t)(int16_t)le16(e+16));
            if(mask&2) put32(q+12+4*n++,(uint32_t)(int32_t)(int16_t)le16(e+18));
            if(mask&4) put32(q+12+4*n++,le16(e+20));
            if(mask&8) put32(q+12+4*n++,le16(e+22));
            if(mask&16) put32(q+12+4*n++,le16(e+24));
            put16(q+2,(uint16_t)(3+n)); req(q,12+4*(size_t)n); }
        else if(t==33){ uint32_t w=le32(e+4), ty=le32(e+8), l0=le32(e+12), l1=le32(e+16), l2=le32(e+20);       /* ClientMessage al gestor */
            if(ty==A_MOVERESIZE) report('m',w,(int)l2);
            else if(ty==A_STATE){ if(l1==A_MAXV||l1==A_MAXH||l2==A_MAXV||l2==A_MAXH||l1==A_FULL) report('x',w,(int)l0); }
            else if(ty==A_CHANGE&&l0==3) report('i',w,0);
            else if(ty==A_ACTIVE) report('a',w,0);
            else if(ty==A_CLOSE){ cmd(3,w); }
            else if(ty==A_REQFRAME){ uint32_t fe[4]={0,0,0,0}; setprop32(w,A_FRAME,6,fe,4); } }
    }
}

int main(int argc,char **argv){
    int disp=argc>1?atoi(argv[1]):0;
    const char *dev=getenv("KNG_XDEV")?getenv("KNG_XDEV"):"/dev/input/event0";
    if(getenv("KNG_XLIST")) list_path=getenv("KNG_XLIST");
    for(int i=0;i<200;i++){ xs=xconnect(disp); if(xs>=0) break; usleep(100000); }       /* espera a que el servidor X este listo (hasta 20 s) */
    if(xs<0){ fprintf(stderr,"kng-xinput: no se pudo conectar al servidor X :%d\n",disp); return 1; }
    xop=xtest_opcode(); if(xop<0){ fprintf(stderr,"kng-xinput: el servidor X no tiene XTEST\n"); return 1; }
    A_NAME=atom("_NET_WM_NAME"); A_UTF8=atom("UTF8_STRING"); A_WMNAME=39; A_ICON=atom("_NET_WM_ICON"); A_PROTO=atom("WM_PROTOCOLS"); A_DELETE=atom("WM_DELETE_WINDOW");
    A_TYPE=atom("_NET_WM_WINDOW_TYPE"); A_TDOCK=atom("_NET_WM_WINDOW_TYPE_DOCK"); A_TDESK=atom("_NET_WM_WINDOW_TYPE_DESKTOP");
    A_MOTIF=atom("_MOTIF_WM_HINTS"); A_STATE=atom("_NET_WM_STATE"); A_MAXV=atom("_NET_WM_STATE_MAXIMIZED_VERT"); A_MAXH=atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    A_FULL=atom("_NET_WM_STATE_FULLSCREEN"); A_HIDDEN=atom("_NET_WM_STATE_HIDDEN"); A_MOVERESIZE=atom("_NET_WM_MOVERESIZE"); A_ACTIVE=atom("_NET_ACTIVE_WINDOW");
    A_CLOSE=atom("_NET_CLOSE_WINDOW"); A_CHANGE=atom("WM_CHANGE_STATE"); A_FRAME=atom("_NET_FRAME_EXTENTS"); A_REQFRAME=atom("_NET_REQUEST_FRAME_EXTENTS");
    A_SUPPORTED=atom("_NET_SUPPORTED"); A_CHECK=atom("_NET_SUPPORTING_WM_CHECK"); A_WMSTATE=atom("WM_STATE");
    if(getenv("KNG_XREQ")) req_path=getenv("KNG_XREQ");
    if(!getenv("KNG_SIN_GESTOR")) wm_start();
    int fd=open(dev,O_RDONLY); if(fd<0){ perror("kng-xinput: dispositivo de entrada"); return 1; }
    printf("kng-xinput: listo (X :%d, XTEST %d, raiz %x)\n",disp,xop,root_win); fflush(stdout);
    int ax=0, ay=0; long next=now_ms();
    for(;;){
        wm_events();
        long t=now_ms(); if(t>=next){ scan(); next=t+150; }
        wm_events();
        struct pollfd p[2]={{fd,POLLIN,0},{xs,POLLIN,0}};
        int tmo=(int)(next-now_ms()); if(tmo<0) tmo=0;
        if(poll(p,2,tmo)<0&&errno!=EINTR) break;
        if(p[1].revents&POLLIN){ skip_packet(); wm_events(); }       /* eventos del gestor de ventanas, errores sueltos */
        if(p[1].revents&(POLLHUP|POLLERR)) break;
        if(!(p[0].revents&POLLIN)) continue;
        struct ev eb[64]; ssize_t r=read(fd,eb,sizeof eb); if(r<=0){ if(r<0&&errno==EINTR) continue; if(r==0){ usleep(20000); continue; } break; }
        for(int k=0;k<(int)(r/(ssize_t)sizeof(struct ev));k++){ struct ev e=eb[k];
            if(e.type==3){ if(e.code==0) ax=e.value; else if(e.code==1) ay=e.value; }                    /* EV_ABS: posicion */
            else if(e.type==0&&e.code==0){ fake(6,0,ax,ay); }                                              /* EV_SYN: mover el puntero */
            else if(e.type==1){
                int down=e.value!=0;
                if(e.code>=0x110&&e.code<=0x112){ int b=e.code==0x110?1:e.code==0x112?2:3; fake(down?4:5,b,ax,ay); }     /* botones: izq, medio, der */
                else if(e.code<0x100){ fake(down?2:3,e.code+8,0,0); }                                                       /* teclas: codigo X = codigo evdev + 8 */
            }
            else if(e.type==2&&e.code==8){ int b=e.value>0?4:5; fake(4,b,ax,ay); fake(5,b,ax,ay); }                       /* rueda */
            else if(e.type==0x31&&e.code<4) par[e.code]=e.value;
            else if(e.type==0x30) cmd(e.code,(uint32_t)e.value);
        }
    }
    return 0;
}
