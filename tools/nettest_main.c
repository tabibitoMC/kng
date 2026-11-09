/* nettest_main.c — compila la pila de red REAL (kernel/net.c) en el host con una tarjeta simulada.
 * Las tramas salen por fd FDOUT y entran por fd FDIN: nettest.py hace de red (DHCP, DNS, ARP, ICMP, servidor TCP/HTTP). */
#define HOSTNET 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <poll.h>
#include "../kernel/kng.h"
void *kmalloc(size_t n){ return malloc(n); }
void kfree(void *p){ free(p); }
static uint32_t T;
uint32_t kticks(void){ return T; }
void kprintf(const char *f,...){ va_list a; va_start(a,f); vfprintf(stderr,f,a); va_end(a); }
#include "../kernel/net.c"

static int fdin=3, fdout=4, fdsync=5, nsent; static uint8_t rb[1<<20]; static int rn;
int host_send(const uint8_t *f,int n){ nsent++; uint16_t l=(uint16_t)n; if(write(fdout,&l,2)!=2) exit(3); if(write(fdout,f,(size_t)n)!=n) exit(3); return 0; }
int host_recv(uint8_t *buf,int max){
    if(rn>=2){ int l=rb[0]|(rb[1]<<8); if(rn>=2+l){ if(l>max) l=max; memcpy(buf,rb+2,(size_t)l); int full=rb[0]|(rb[1]<<8); memmove(rb,rb+2+full,(size_t)(rn-2-full)); rn-=2+full; return l; } }
    return 0;
}
static void pump(int block){              /* lee tramas de la red simulada hacia rb */
    struct pollfd p={fdin,POLLIN,0};
    while(poll(&p,1,block)>0 && (p.revents&POLLIN)){ ssize_t r=read(fdin,rb+rn,sizeof rb-(size_t)rn); if(r<=0) break; rn+=(int)r; block=0; if(rn>=(int)sizeof rb) break; }
}
static void step(void){                   /* en sincronia: espera a que Python procese cada trama enviada (sin carreras de tiempo) */
    net_poll();
    while(nsent>0){
        struct pollfd p[2]={{fdsync,POLLIN,0},{fdin,POLLIN,0}};
        if(poll(p,2,5000)<=0) exit(5);
        if(p[1].revents&POLLIN){ ssize_t r=read(fdin,rb+rn,sizeof rb-(size_t)rn); if(r>0) rn+=(int)r; }
        if(p[0].revents&POLLIN){ char c; if(read(fdsync,&c,1)==1) nsent--; }
    }
    pump(0);
    T++;
}
static void run(int ticks){ for(int i=0;i<ticks;i++) step(); }
static void cmd(const char *s,int ticks){ char l[160]; snprintf(l,sizeof l,"%s",s); fprintf(stderr,"CMD %s\n",s); if(!net_cmd(l)) fprintf(stderr,"(no es de red)\n"); run(ticks); }
static void fetch(const char *url,int maxticks){
    fprintf(stderr,"FETCHSTART %s\n",url);
    if(http_get(url)<0){ fprintf(stderr,"FETCH %s ERR %s\n",url,http_error()); return; }
    for(int i=0;i<maxticks;i++){ step(); if(http_poll()) break; }
    int p=http_poll();
    if(p<0){ fprintf(stderr,"FETCH %s ERR %s\n",url,http_error()); return; }
    if(p==0){ fprintf(stderr,"FETCH %s TIMEOUT\n",url); http_cancel(); return; }
    int st=http_status(), html=http_is_html(); char fin[300]; snprintf(fin,sizeof fin,"%s",http_final_url());
    int n=0; char *b=http_take(&n); uint32_t h=2166136261u; for(int i=0;i<n;i++){ h^=(uint8_t)b[i]; h*=16777619u; }
    if(b[n]!=0) fprintf(stderr,"BUG: sin terminador\n");
    fprintf(stderr,"FETCH %s OK status=%d len=%d html=%d fnv=%08x final=%s\n",url,st,n,html,h,fin);
    free(b);
}
int main(void){
    if(getenv("FDIN")) fdin=atoi(getenv("FDIN"));
    if(getenv("FDOUT")) fdout=atoi(getenv("FDOUT"));
    if(getenv("FDSYNC")) fdsync=atoi(getenv("FDSYNC"));
    net_init();
    for(int i=0;i<3000 && net_state()!=2;i++) step();
    fprintf(stderr,"STATE %d\n",net_state());
    { char b[200]; net_info(b,sizeof b); fprintf(stderr,"INFO %s\n",b); }
    cmd("red",5);
    run(100);                                 /* tiempo para que el servidor pruebe ARP e ICMP contra nosotros */
    cmd("ping example.test",520);
    cmd("dns example.test",50);
    cmd("dns nx.test",100);
    fetch("http://example.test/",600);
    fetch("http://example.test/big",1500);
    fetch("http://example.test/chunk",600);
    fetch("http://example.test/redir",800);
    fetch("http://example.test/rel",1500);
    fetch("http://example.test/nolen",600);
    fetch("http://example.test/404",600);
    fetch("http://example.test/img",600);
    fetch("http://example.test/loss",3000);
    fetch("http://example.test:81/",600);
    fetch("https://example.test/",10);
    fetch("http://nx.test/",600);
    fetch("http://example.test/a b?x=1",600);
    fetch("http://93.184.216.34/",600);
    fetch("example.test/",600);
    cmd("wget http://example.test/",300);
    fetch("http://silent.test/",3500);
    fprintf(stderr,"FIN\n");
    return 0;
}
