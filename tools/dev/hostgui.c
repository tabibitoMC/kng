/* hostgui.c — simulador del escritorio en el host: compila el MISMO codigo de la interfaz (gfx, fs, html, gui),
 * simula teclado/raton y guarda capturas PPM. Uso: tools/dev/hostgui.py (convierte a PNG). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../kernel/kng.h"
void *kmalloc(size_t n){ return malloc(n); }
void kfree(void *p){ free(p); }
static uint32_t T=1000;
uint32_t kticks(void){ return T; }
int kgetc(void){ return -1; }
void (*kcon_hook)(char);
static void out(const char *s){ while(*s){ if(kcon_hook) kcon_hook(*s); s++; } }
void kprintf(const char *f,...){ (void)f; }
void kshell_exec(char *l){
    if(!strcmp(l,"info")) out("kng-kernel 0.5 (i386, Multiboot)\nParte de Königreich OS.\n");
    else if(!strcmp(l,"uptime")) out("12.3 s\n");
    else { out("comando desconocido: "); out(l); out(" (prueba 'help')\n"); }
}
void kreboot(void){ puts("[reboot]"); }
void khalt(void){ puts("[halt]"); }
int krtc(int *h,int *m){ *h=15; *m=45; return 0; }   /* 15:45 UTC -> 10:45 en UTC-5 */
uint32_t kram_kb(void){ return 262144; }
/* ---- red simulada para la interfaz: http_get "descarga" tras unos cuadros ---- */
#include "../../kernel/net.h"
static int fk_state, fk_wait; static char fk_url[300], fk_err[100];
static const char FK_PAGE[]="<html><head><title>Pagina de prueba</title><style>body{color:red}</style><script>alert('no se debe ver')</script></head>"
 "<body><!-- comentario --><h1>example.test</h1><p>Esta pagina llego por <b>HTTP</b> &mdash; con entidades: &copy; &amp; &lt;etiqueta&gt; &#233; &eacute;.</p>"
 "<h2>Enlaces</h2><ul><li><a href=\"/otra\">Ruta absoluta</a></li><li><a href=\"sub/pagina.html\">Ruta relativa</a></li><li><a href=\"../arriba\">Padre</a></li>"
 "<li><a href=\"http://otro.test:8080/x?y=1\">Otro sitio</a></li><li><a href=\"#ancla\">Ancla</a></li></ul>"
 "<table><tr><td>uno</td><td>dos</td></tr></table><h3>Titulo tres</h3><p><i>enfasis</i> y <strong>fuerte</strong>.</p><img src=\"x.png\" alt=\"logo\"></body></html>";
int http_get(const char *url){ k_strcpy(fk_url,url,sizeof fk_url); fk_err[0]=0;
    if(k_startswith(url,"https://")){ k_strcpy(fk_err,"HTTPS no esta soportado todavia (kng no tiene TLS). Usa una direccion http://",sizeof fk_err); return -1; }
    fk_state=0; fk_wait=3; return 0; }
int http_poll(void){ if(fk_state) return fk_state; if(--fk_wait>0) return 0; if(strstr(fk_url,"fallo")){ k_strcpy(fk_err,"No se pudo resolver el nombre: fallo.test",sizeof fk_err); fk_state=-1; } else fk_state=1; return fk_state; }
char *http_take(int *len){ char *b=malloc(sizeof FK_PAGE); memcpy(b,FK_PAGE,sizeof FK_PAGE); *len=sizeof FK_PAGE-1; return b; }
int http_is_html(void){ return 1; }
int http_status(void){ return 200; }
const char *http_error(void){ return fk_err; }
const char *http_final_url(void){ return fk_url; }
void http_cancel(void){ fk_state=0; fk_wait=0; }
void net_info(char *o,int max){ k_strcpy(o,"Red: Intel e1000  IP 192.168.7.50",max); }
int net_state(void){ return 2; }
#include "../../kernel/gfx.c"
#include "../../kernel/fs.c"
#define HOSTDISK 1
#include "../../kernel/disk.c"
#include "../../kernel/html.c"
#include "../../kernel/kbd.c"
#include "../../kernel/zip.c"
/* lo que en el kernel real ponen el shell y los procesos */
int kattach_pid; char kshell_cwd[96]="/"; void (*kproc_done)(int,int); void kproc_key(int c){ (void)c; }
static int kev[4096][3], nkev; void kinput_event(int t,int c,int v){ if(nkev<4096){ kev[nkev][0]=t; kev[nkev][1]=c; kev[nkev][2]=v; nkev++; } } void kserial_puts(const char *s){ (void)s; }
#include "../../kernel/gui.c"

#define W 1024
#define H 768
static uint32_t *fb;
static void shot(const char *name){
    gui_frame();
    char p[128]; snprintf(p,sizeof p,"%s/%s.ppm",getenv("OUTDIR")?getenv("OUTDIR"):"/tmp",name);
    FILE *f=fopen(p,"wb"); fprintf(f,"P6\n%d %d\n255\n",W,H);
    for(int i=0;i<W*H;i++){ uint32_t c=fb[i]; fputc((c>>16)&255,f); fputc((c>>8)&255,f); fputc(c&255,f); }
    fclose(f);
}
static void click(int x,int y){ gui_mouse_abs(x,y,0); gui_frame(); gui_mouse_abs(x,y,1); gui_frame(); gui_mouse_abs(x,y,0); gui_frame(); T+=5; }
static void dclick(int x,int y){ click(x,y); T+=3; click(x,y); }
static void typestr(const char *s){ for(;*s;s++){ gui_key(*s=='\\'?'\n':*s); } }
static void dock(int i){ int x,y; dock_base_center(i,&x,&y); click(x,y); T+=20; gui_frame(); }   /* misma geometria que el dock real */
static void closeall(void){ for(int k=0;k<14;k++){ int top=win_top(); if(top<0) break; Win *w=&wins[top]; int x,y,cw,ch; ctl_geom(w,0,&x,&y,&cw,&ch); click(x+cw/2,y+ch/2); } T+=30; gui_frame(); }
int main(void){
    fb=calloc(W*H,4);
    if(gui_init(fb,W,H,W*4)){ puts("gui_init fallo"); return 1; }
    shot("01_escritorio");
    dock(2); T+=100;                       /* Archivos */
    dclick(170,160); shot("02_archivos");
    closeall();
    dock(2); shot("02_archivos_raiz");
    dclick(170,150);                        /* Documentos (primera fila) */
    shot("02c_archivos_documentos");
    closeall();
    dock(3); typestr("help\\ls /\\cd /Documentos\\ls\\cat notas.txt\\write hola.txt Hola desde kng\\cat hola.txt\\info\\"); shot("03_terminal");
    closeall();
    dock(1); shot("04_navegador");
    click(190,254); shot("05_navegador_enlace");
    /* navegador con http simulado: cargando -> pagina -> enlaces relativos */
    closeall(); dock(1);
    { Win *b=&wins[win_top()]; click(b->x+200,b->y+TB+18); typestr("example.test/dir/index.html\\"); }
    shot("05b_http_cargando");
    gui_frame(); gui_frame(); gui_frame(); gui_frame(); shot("05c_http_pagina");
    { Win *b=&wins[win_top()];
      char u[192];
      resolve("http://example.test/dir/index.html","/otra",u,sizeof u);               if(strcmp(u,"http://example.test/otra")) { puts("FALLO resolve 1"); return 1; }
      resolve("http://example.test/dir/index.html","sub/pagina.html",u,sizeof u);     if(strcmp(u,"http://example.test/dir/sub/pagina.html")) { puts(u); puts("FALLO resolve 2"); return 1; }
      resolve("http://example.test/dir/index.html","../arriba",u,sizeof u);            if(strcmp(u,"http://example.test/arriba")) { puts(u); puts("FALLO resolve 3"); return 1; }
      resolve("http://example.test:8080/a/b?q=1","?z=2",u,sizeof u);                  if(strcmp(u,"http://example.test:8080/a/b?z=2")) { puts(u); puts("FALLO resolve 4"); return 1; }
      resolve("http://example.test/a/","//cdn.test/x",u,sizeof u);                    if(strcmp(u,"http://cdn.test/x")) { puts(u); puts("FALLO resolve 5"); return 1; }
      resolve("http://example.test/a/b","http://z.test/",u,sizeof u);                 if(strcmp(u,"http://z.test/")) { puts("FALLO resolve 6"); return 1; }
      resolve("file:///Documentos/pagina.html","notas.txt",u,sizeof u);               if(strcmp(u,"file:///Documentos/notas.txt")) { puts(u); puts("FALLO resolve 7"); return 1; }
      resolve("http://example.test","x",u,sizeof u);                                   if(strcmp(u,"http://example.test/x")) { puts(u); puts("FALLO resolve 8"); return 1; }
      puts("RESOLVE OK"); (void)b; }
    { Win *b=&wins[win_top()]; click(165,226); }              /* primer enlace de la lista (ruta absoluta) */
    gui_frame(); gui_frame(); gui_frame(); gui_frame(); gui_frame(); shot("05d_http_enlace");
    { Win *b=&wins[win_top()]; click(b->x+200,b->y+TB+18); typestr("https://segura.test\\"); }
    shot("05e_https_error");
    { Win *b=&wins[win_top()]; click(b->x+200,b->y+TB+18); typestr("fallo.test\\"); gui_frame(); gui_frame(); gui_frame(); gui_frame(); }
    shot("05f_http_fallo");
    closeall();
    dock(0); shot("07_acerca");
    closeall();
    dock(7); { Win *st=&wins[win_top()]; click(st->x+1+SBW+20+44+14,st->y+TB+SOY+54); click(st->x+1+SBW+20+96+40,st->y+TB+SOY+114+25); } shot("08_ajustes_cambio");
    closeall();
    /* buscador */
    dock(1);
    { Win *b=&wins[win_top()]; click(b->x+200,b->y+TB+18); typestr("bienvenida\\"); } shot("09_buscar");
    { Win *b=&wins[win_top()]; click(b->x+200,b->y+TB+18); typestr("kng\\"); } shot("09b_buscar_kng");
    { Win *b=&wins[win_top()]; if(strcmp(b->url,"http://10.0.2.2:8080/search?q=kng")){ puts(b->url); puts("FALLO buscador web url"); return 1; } }
    { Win *b=&wins[win_top()]; click(b->x+200,b->y+TB+18); typestr("https://es.wikipedia.org/wiki/Kernel\\"); if(strncmp(b->url,"http://10.0.2.2:8080/go?u=https%3A%2F%2Fes.wikipedia.org",55)){ puts(b->url); puts("FALLO https por proxy"); return 1; } }
    { FsNode *cf=fs_resolve(fs_root(),"/Sistema/kng.cfg"); fs_write(cf,"idioma=es\n",10); }                 /* sin buscador web: busqueda local */
    { Win *b=&wins[win_top()]; click(b->x+200,b->y+TB+18); typestr("kng\\"); if(!strstr(b->url,"kng://buscar?q=kng")){ puts(b->url); puts("FALLO buscador local"); return 1; } }
    { Win *b=&wins[win_top()]; click(b->x+200,b->y+TB+18); typestr("zzzzqq nada\\"); } shot("09c_buscar_vacio");
    closeall();
    /* notas */
    dock(4); typestr("Hola desde Notas\nSegunda linea escrita con el teclado de kng."); { Win *n=&wins[win_top()]; click(n->x+1+86+20,n->y+TB+16); } shot("10_notas");
    { FsNode *f=fs_resolve(fs_root(),"/Documentos/nota.txt"); if(!f||f->size<20){ puts("FALLO guardar nota"); return 1; } }
    closeall();
    /* calculadora */
    dock(5); typestr("12*34="); shot("11_calculadora");
    { Win *c=&wins[win_top()]; if(c->ccur!=408*CSC){ puts("FALLO calc 12*34"); return 1; } typestr("C7/0="); if(!c->cerr){ puts("FALLO calc /0"); return 1; } typestr("C100-1="); if(c->ccur!=99*CSC){ puts("FALLO calc 100-1"); return 1; } }
    closeall();
    /* snake */
    dock(6); gui_key('d'); for(int i=0;i<40;i++){ T+=3; gui_frame(); } shot("12_snake");
    { Win *s2=&wins[win_top()]; if(!s2->sstart){ puts("FALLO snake"); return 1; } }
    for(int i=0;i<400;i++){ T+=3; gui_frame(); } shot("12b_snake_fin");
    closeall();
    /* disco y sistema de archivos: formatear, guardar, copiar, mover, renombrar, recargar */
    dock(3); typestr("disco\\disco formatear si\\cd /Documentos\\write a.txt hola disco\\cp a.txt b.txt\\mv b.txt /Escritorio\\ren a.txt z.txt\\mkdir sub\\cp /Sistema sub\\df\\"); shot("17_disco_terminal");
    T+=1000; kngfs_tick();
    if(!fs_resolve(fs_root(),"/Escritorio/b.txt")||!fs_resolve(fs_root(),"/Documentos/z.txt")||fs_resolve(fs_root(),"/Documentos/a.txt")||!fs_resolve(fs_root(),"/Documentos/sub/Sistema/version.txt")){ puts("FALLO cp/mv/ren"); return 1; }
    closeall();
    { FsNode *z=fs_resolve(fs_root(),"/Documentos/z.txt"); fs_write(z,"CAMBIADO",8); fs_remove(fs_resolve(fs_root(),"/Escritorio/b.txt")); if(kngfs_save()){ puts("FALLO guardar"); return 1; } }
    { fs_write(fs_resolve(fs_root(),"/Documentos/z.txt"),"basura",6); fs_create(fs_root(),"temporal.txt","x",1); }       /* cambio sin guardar: al montar se recupera lo guardado (el montaje superpone el disco sobre lo que hay en RAM) */
    mounted=0; if(kngfs_mount()){ puts("FALLO montar"); return 1; }
    { FsNode *z=fs_resolve(fs_root(),"/Documentos/z.txt"); if(!z||z->size!=8||memcmp(z->data,"CAMBIADO",8)){ printf("z=%p size=%d data=%.10s\n",(void*)z,z?z->size:-1,z&&z->data?z->data:"-"); puts("FALLO persistencia"); return 1; } puts("DISCO OK"); }
    dock(8); shot("18_monitor"); closeall();
    /* escritorio modular: arrastrar icono y widget, abrir con doble clic */
    { int ox=22+38, oy=BARH+18+30; gui_mouse_abs(ox,oy,0); gui_frame(); gui_mouse_abs(ox,oy,1); gui_frame(); gui_mouse_abs(ox+300,oy+200,1); gui_frame(); gui_mouse_abs(ox+300,oy+200,0); gui_frame(); T+=100;
      if(dobj[0].x<200){ puts("FALLO arrastrar icono"); return 1; }
      dclick(ox+300,oy+200); if(nord<1||wins[win_top()].type!=A_BROWSER){ puts("FALLO abrir icono"); return 1; } }
    shot("14_escritorio_modular");
    { Win *b=&wins[win_top()]; int tx=b->x+300, ty=b->y+TB/2; T+=100; click(tx,ty); T+=3; click(tx,ty); if(!b->maxed||b->w!=W){ puts("FALLO maximizar"); return 1; } shot("15_maximizada");
      T+=100; ty=b->y+TB/2; click(tx,ty); T+=3; click(tx,ty); if(b->maxed){ puts("FALLO restaurar"); return 1; }
      T+=100; gui_mouse_abs(b->x+300,b->y+10,0); gui_frame(); gui_mouse_abs(b->x+300,b->y+10,1); gui_frame(); gui_mouse_abs(2,300,1); gui_frame(); gui_mouse_abs(2,300,0); gui_frame();
      if(!b->maxed||b->w!=W/2||b->x!=0){ puts("FALLO ajustar izquierda"); return 1; } shot("16_ajustada"); }
    closeall();
    /* teclado: distribuciones, tildes en terminal/notas/navegador, persistencia y selector en Ajustes */
    { dock(3); typestr("teclado latam\\");
      if(kbd_layout()!=KBD_LATAM){ puts("FALLO teclado latam"); return 1; }
      { FsNode *cf=fs_resolve(fs_root(),"/Sistema/kng.cfg"); if(!cf||!strstr(cf->data,"teclado=latam")||!strstr(cf->data,"idioma=")){ puts("FALLO cfg teclado"); return 1; } }
      #define SC(b) gui_key(kbd_feed(b))
      SC(0x2A); SC(0x08); SC(0xA8); SC(0xAA);                 /* Shift+7 en Latam = '/' */
      SC(0x27); SC(0xA7);                                     /* n con tilde */
      SC(0x1A); SC(0x9A); SC(0x12); SC(0x92);                 /* tilde muerta + e = e con tilde */
      { Win *t=&wins[win_top()]; unsigned char *g=(unsigned char*)t->grid[t->tr];
        if(t->inlen!=5||memcmp(t->in,"/\xC3\xB1\xC3\xA9",5)||g[t->tc-1]!=0xE9||g[t->tc-2]!=0xF1){ puts("FALLO tildes terminal"); return 1; }
        gui_key('\b'); if(t->inlen!=3){ puts("FALLO borrar tilde"); return 1; } }
      shot("19_terminal_teclado");
      closeall();
      dock(4); SC(0x27); SC(0xA7); SC(0x1E); SC(0x9E); { Win *n=&wins[win_top()]; if(n->nlen!=3||memcmp(n->note,"\xC3\xB1""a",3)){ puts("FALLO nota ñ"); return 1; } gui_key('\b'); gui_key('\b'); if(n->nlen!=0){ puts("FALLO borrar ñ"); return 1; } } closeall();
      kbd_set_layout(KBD_US);
      dock(7); { Win *st=&wins[win_top()]; click(st->x+1+SBW+170+60,st->y+TB+SOY+322+13); if(kbd_layout()!=KBD_LATAM){ puts("FALLO boton teclado"); return 1; } } shot("20_ajustes_teclado"); closeall();
      kbd_set_layout(KBD_US); { char kv[16]; cfg_get("teclado",kv,sizeof kv); kbd_set_layout(layout_from_name(kv)); if(kbd_layout()!=KBD_LATAM){ puts("FALLO leer cfg"); return 1; } }
      gui_panic("Falta de pagina","eip",0x1234abcdULL,0x2,0xdeadbeefULL,1); shot("21_panic"); gui_force_redraw(); puts("TECLADO GUI OK"); }
    /* escritorio lleno */
    dock(0); dock(1); dock(2); dock(3); dock(4); dock(5); dock(6); shot("13_escritorio_lleno");
    closeall();
    /* estilos visuales: Frutiger Aero, Windows 11, Mac */
    for(int t=0;t<NTEMAS;t++){ char n[64];
        tema_aplicar(t,1);
        { char v[8]; cfg_get("tema",v,sizeof v); if(v[0]!='0'+t){ puts("FALLO guardar tema"); return 1; } }
        snprintf(n,sizeof n,"29_tema%d_escritorio",t); shot(n);
        dock(2); dock(3); typestr("ls /\\"); dock(7);
        { Win *st=&wins[win_top()]; click(st->x+1+80,st->y+TB+86+1*40+18); }
        snprintf(n,sizeof n,"30_tema%d_ventanas",t); shot(n);
        pw_open=1; pw_hov=1; dirty=1; snprintf(n,sizeof n,"34_tema%d_apagar",t); shot(n); pw_open=0; pw_hov=-1; dirty=1;
        { int x,y; dock_base_center(2,&x,&y); gui_mouse_abs(x,y,0); gui_frame(); snprintf(n,sizeof n,"31_tema%d_dock",t); shot(n); gui_mouse_abs(500,300,0); }
        /* minimizar y volver */
        { int top=win_top(); Win *w=&wins[top]; int x,y,cw,ch; ctl_geom(w,2,&x,&y,&cw,&ch); click(x+cw/2,y+ch/2); if(!w->minim){ puts("FALLO minimizar"); return 1; }
          T+=8; snprintf(n,sizeof n,"32_tema%d_minimizando",t); shot(n); T+=40; gui_frame();
          dock(7); if(w->minim||win_top()!=top){ puts("FALLO restaurar minimizada"); return 1; } T+=6; snprintf(n,sizeof n,"33_tema%d_abriendo",t); shot(n); T+=30; gui_frame(); }
        /* cambiar tamano con el borde */
        { int fi=-1; for(int k=0;k<nord;k++) if(wins[order[k]].type==A_FILES) fi=order[k]; Win *w=&wins[fi]; focus(fi); gui_frame(); int ow=w->w, oh=w->h;
          int ex=w->x+w->w-2, ey=w->y+w->h-2; gui_mouse_abs(ex,ey,0); gui_frame(); gui_mouse_abs(ex,ey,1); gui_frame(); gui_mouse_abs(ex+80,ey+40,1); gui_frame(); gui_mouse_abs(ex+80,ey+40,0); gui_frame();
          if(w->w!=ow+80||w->h!=oh+40){ printf("%d %d -> %d %d\n",ow,oh,w->w,w->h); puts("FALLO cambiar tamano"); return 1; } }
        closeall(); }
    puts("TEMAS OK");
    /* usuario y contrasena */
    tema_aplicar(0,1);
    dock(7); { Win *st=&wins[win_top()]; int ox=st->x+1, oy=st->y+TB;
      click(ox+80,oy+86+4*40+18); shot("40_cuenta_vacia");
      click(ox+SBW+20+100,oy+SOY+112+15); typestr("nick\\clave1\\clave1"); shot("41_cuenta_escribiendo"); gui_key('\n');
      if(!cu_user[0]){ puts(cf_msg); puts("FALLO crear usuario"); return 1; }
      shot("42_cuenta_creada"); }
    closeall();
    bloqueado=0; { char u[28], h[8]; if(cfg_get("usuario",u,sizeof u)&&u[0]&&cfg_get("clave",h,sizeof h)&&h[0]) bloqueado=1; }
    if(!bloqueado){ puts("FALLO bloqueo al encender"); return 1; }
    shot("43_bloqueo");
    dock(3); if(nord){ puts("FALLO el bloqueo deja abrir programas"); return 1; }
    typestr("mala\\"); T+=5; shot("44_bloqueo_error"); if(!bloqueado){ puts("FALLO contrasena mala"); return 1; }
    T+=60; typestr("clave1\\"); if(bloqueado){ puts("FALLO contrasena buena"); return 1; } T+=8; shot("45_desbloqueando"); T+=40; shot("46_desbloqueado");
    dock(7); { Win *st=&wins[win_top()]; int ox=st->x+1, oy=st->y+TB; click(ox+80,oy+86+4*40+18);
      click(ox+SBW+20+100,oy+SOY+196+15); typestr("clave1"); click(ox+SBW+200+100,oy+SOY+300+15); if(cu_user[0]){ puts(cf_msg); puts("FALLO quitar usuario"); return 1; } shot("47_cuenta_quitada"); }
    closeall();
    puts("SESION OK");
    /* programas de Linux: los .desktop instalados aparecen solos */
    { FsNode *u=fs_root(); { const char *pp[3]={"usr","share","applications"}; for(int i=0;i<3;i++){ FsNode *n=fs_find(u,pp[i]); u=n?n:fs_mkdir(u,pp[i]); } }
      { const char *dd="[Desktop Entry]\nName=GIMP\nName[es]=Editor de imagenes GIMP\nExec=gimp-2.10 %U\nType=Application\n[Desktop Action x]\nName=Otra\n"; fs_create(u,"gimp.desktop",dd,(int)strlen(dd)); }
      { const char *dd="[Desktop Entry]\nName=Oculto\nExec=nada\nNoDisplay=true\n"; fs_create(u,"oculto.desktop",dd,(int)strlen(dd)); }
      { const char *dd="[Desktop Entry]\nName=Chromium\nExec=/usr/bin/chromium %U\n"; fs_create(u,"chromium.desktop",dd,(int)strlen(dd)); }
      { const char *dd="[Desktop Entry]\nName=Reproductor VLC\nExec=/usr/bin/vlc --started-from-file %U\nType=Application\n"; fs_create(u,"vlc.desktop",dd,(int)strlen(dd)); }
      dock(9); shot("50_programas_linux");
      if(ndapps!=2||strcmp(dapps[0].name,"Editor de imagenes GIMP")&&strcmp(dapps[1].name,"Editor de imagenes GIMP")){ printf("%d\n",ndapps); puts("FALLO .desktop"); return 1; }
      puts("DEBIAN OK"); closeall(); }
    /* programas de Linux en ventanas propias: framebuffer de Xvfb simulado + lista de kng-xinput */
    { FsNode *r=fs_root(), *n; const char *pp[3]={"var","lib","xvfb"}; FsNode *d=r; for(int i=0;i<3;i++){ n=fs_find(d,pp[i]); d=n?n:fs_mkdir(d,pp[i]); }
      int hs=104, sz=hs+W*H*4; unsigned char *x=calloc(1,sz);
      #define PB(o,v) { x[o]=(unsigned char)((v)>>24); x[o+1]=(unsigned char)((v)>>16); x[o+2]=(unsigned char)((v)>>8); x[o+3]=(unsigned char)(v); }
      PB(0,hs); PB(16,W); PB(20,H); PB(44,32); PB(48,W*4); PB(76,0);
      uint32_t *px=(uint32_t*)(x+hs);
      for(int yy=0;yy<H;yy++) for(int xx=0;xx<W;xx++) px[yy*W+xx]=0x000000;
      FsNode *tm=fs_find(r,"tmp"); if(!tm) tm=fs_mkdir(r,"tmp");
      FsNode *fbn=fs_create(d,"Xvfb_screen0",(char*)x,sz);
      /* el programa abre su ventana en 0,0 (sin gestor de ventanas) */
      const char *l1="kngx 1\n400001 0 0 500 320 0 Editor de imagenes GIMP\n";
      fs_create(tm,".kng-xwin",l1,(int)strlen(l1)); nkev=0; T+=10; gui_frame();
      int xi=-1; for(int i=0;i<MAXW;i++) if(wins[i].used&&wins[i].type==A_XWIN) xi=i;
      if(xi<0){ puts("FALLO ventana X no creada"); return 1; }
      Win *xw=&wins[xi]; int cx=xw->x+1, cy=xw->y+TB;
      int ok=0; for(int i=0;i<nkev;i++) if(kev[i][0]==0x30&&kev[i][1]==1&&kev[i][2]==0x400001) ok=1;
      if(!ok||xw->xsx!=cx||xw->xsy!=cy){ puts("FALLO no se pidio mover la ventana X"); return 1; }
      /* X la movio: el contenido se pinta (rojo arriba, verde abajo) y aparece un menu emergente */
      fbn=fs_resolve(r,"/var/lib/xvfb/Xvfb_screen0"); px=(uint32_t*)(fbn->data+hs);
      for(int yy=cy;yy<cy+320;yy++) for(int xx=cx;xx<cx+500;xx++) px[yy*W+xx]=yy<cy+30?0xdddddd:(xx<cx+120?0xf0f0f0:0x3070c0);
      for(int yy=cy+30;yy<cy+130;yy++) for(int xx=cx+10;xx<cx+150;xx++) px[yy*W+xx]=0xffffff;
      char l2[200]; snprintf(l2,sizeof l2,"kngx 1\n400001 %d %d 500 320 0 Editor de imagenes GIMP\n400002 %d %d 140 100 1 \n",cx,cy,cx+10,cy+30);
      fs_create(tm,".kng-xwin",l2,(int)strlen(l2)); T+=10; gui_frame(); T+=30; gui_frame();
      { int pi=-1; for(int i=0;i<MAXW;i++) if(wins[i].used&&wins[i].type==A_XWIN&&wins[i].xpop) pi=i; if(pi<0){ puts("FALLO menu emergente"); return 1; } }
      shot("60_linux_ventana");
      /* clic dentro: se entrega a X en coordenadas de X */
      nkev=0; click(cx+200,cy+200);
      { int got=0; for(int i=0;i+1<nkev;i++) if(kev[i][0]==3&&kev[i][1]==0&&kev[i][2]==cx+200&&kev[i+1][2]==cy+200) got=1; int btn=0; for(int i=0;i<nkev;i++) if(kev[i][0]==1&&kev[i][1]==0x110) btn++;
        if(!got||btn!=2){ printf("got=%d btn=%d\n",got,btn); puts("FALLO raton a X"); return 1; } }
      /* arrastrar la barra de titulo: se pide mover la ventana X */
      nkev=0; gui_mouse_abs(xw->x+200,xw->y+10,0); gui_frame(); gui_mouse_abs(xw->x+200,xw->y+10,1); gui_frame(); gui_mouse_abs(xw->x+260,xw->y+60,1); T+=5; gui_frame(); gui_mouse_abs(xw->x+260,xw->y+60,0); T+=5; gui_frame();
      if(xw->xsx!=xw->x+1||xw->xsy!=xw->y+TB){ puts("FALLO mover ventana X"); return 1; }
      dock_layout(); if(dock_n!=NDOCK+1){ puts("FALLO icono en el dock"); return 1; }
      shot("61_linux_movida");
      /* el programa se cierra: la ventana de kng desaparece */
      fs_create(tm,".kng-xwin","kngx 1\n",7); T+=10; gui_frame(); T+=30; gui_frame();
      for(int i=0;i<MAXW;i++) if(wins[i].used&&wins[i].type==A_XWIN){ puts("FALLO ventana X no se cerro"); return 1; }
      free(x); puts("VENTANAS LINUX OK"); }
    /* programa con su propio marco (Chromium/Brave): sin barra de kng; pide maximizar, minimizar, mover */
    { FsNode *tm=fs_find(fs_root(),"tmp"); launch_set("Brave"); T+=20; shot("62_abriendo_brave");
      const char *l1="kngx 1\n500001 0 0 800 520 2 Brave\n"; fs_create(tm,".kng-xwin",l1,(int)strlen(l1)); nkev=0; T+=10; gui_frame();
      int bi=-1; for(int i=0;i<MAXW;i++) if(wins[i].used&&wins[i].type==A_XWIN&&wins[i].xid==0x500001) bi=i;
      if(bi<0||!wins[bi].xcsd||launch_t){ puts("FALLO ventana con marco propio"); return 1; }
      Win *bw=&wins[bi]; if(bw->xsx!=bw->x||bw->xsy!=bw->y||bw->xsw!=bw->w||bw->xsh!=bw->h){ puts("FALLO tamano sin marco"); return 1; }
      T+=30; gui_frame(); shot("63_brave_sin_marco");
      fs_create(tm,".kng-xreq","1 x 500001 2\n",13); T+=3; gui_frame(); if(!bw->maxed){ puts("FALLO maximizar pedido"); return 1; }
      { int st=0; for(int i=0;i<nkev;i++) if(kev[i][0]==0x30&&kev[i][1]==5) st=1; if(!st){ puts("FALLO estado maximizado a X"); return 1; } }
      fs_create(tm,".kng-xreq","1 x 500001 2\n2 x 500001 2\n",26); T+=3; gui_frame(); if(bw->maxed){ puts("FALLO restaurar pedido"); return 1; }
      fs_create(tm,".kng-xreq","1 x 500001 2\n2 x 500001 2\n3 i 500001 0\n",39); T+=3; gui_frame(); if(!bw->minim){ puts("FALLO minimizar pedido"); return 1; }
      fs_create(tm,".kng-xreq","2 x 500001 2\n3 i 500001 0\n4 a 500001 0\n",39); T+=3; gui_frame(); if(bw->minim){ puts("FALLO activar pedido"); return 1; }
      T+=30; gui_frame();
      int ox=bw->x, oy=bw->y; gui_mouse_abs(bw->x+300,bw->y+15,0); gui_frame(); gui_mouse_abs(bw->x+300,bw->y+15,1); gui_frame();
      fs_create(tm,".kng-xreq","3 i 500001 0\n4 a 500001 0\n5 m 500001 8\n",39); T+=3; gui_frame();
      gui_mouse_abs(ox+300+70,oy+15+40,1); T+=5; gui_frame(); gui_mouse_abs(ox+300+70,oy+15+40,0); T+=5; gui_frame();
      if(bw->x!=ox+70||bw->y!=oy+40){ printf("%d,%d -> %d,%d\n",ox,oy,bw->x,bw->y); puts("FALLO mover pedido"); return 1; }
      T+=40; gui_frame(); shot("64_brave_movida");
      fs_create(tm,".kng-xwin","kngx 1\n",7); T+=10; gui_frame(); T+=30; gui_frame();
      puts("MARCO PROPIO OK"); }
    return 0;
}
