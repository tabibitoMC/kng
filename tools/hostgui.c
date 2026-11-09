/* hostgui.c — simulador del escritorio en el host: compila el MISMO codigo de la interfaz (gfx, fs, html, gui),
 * simula teclado/raton y guarda capturas PPM. Uso: tools/hostgui.py (convierte a PNG). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../kernel/kng.h"
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
#include "../kernel/net.h"
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
#include "../kernel/gfx.c"
#include "../kernel/fs.c"
#define HOSTDISK 1
#include "../kernel/disk.c"
#include "../kernel/html.c"
#include "../kernel/kbd.c"
#include "../kernel/gui.c"

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
static void dock(int i){ int bw=9*52+8*14+28, bx=(W-bw)/2, by=H-(52+16)-10; click(bx+14+i*66+26,by+8+26); }
static void closeall(void){ for(int k=0;k<14;k++){ int top=win_top(); if(top<0) break; Win *w=&wins[top]; click(w->x+w->w-20,w->y+TB/2); } }
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
    dock(7); { Win *st=&wins[win_top()]; click(st->x+1+20+44+14,st->y+TB+54); click(st->x+1+20+112,st->y+TB+114+30); } shot("08_ajustes_cambio");
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
    { Win *c=&wins[win_top()]; if(c->ccur!=408){ puts("FALLO calc 12*34"); return 1; } typestr("C7/0="); if(!c->cerr){ puts("FALLO calc /0"); return 1; } typestr("C100-1="); if(c->ccur!=99){ puts("FALLO calc 100-1"); return 1; } }
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
    { FsNode *z=fs_resolve(fs_root(),"/Documentos/z.txt"); fs_write(z,"CAMBIADO",8); fs_remove(fs_resolve(fs_root(),"/Escritorio/b.txt")); T+=1000; kngfs_tick(); }
    { fs_write(fs_resolve(fs_root(),"/Documentos/z.txt"),"basura",6); fs_create(fs_root(),"temporal.txt","x",1); }       /* cambios sin guardar: se pierden al "reiniciar" */
    if(kngfs_mount()){ puts("FALLO montar"); return 1; }
    { FsNode *z=fs_resolve(fs_root(),"/Documentos/z.txt"); if(!z||z->size!=8||memcmp(z->data,"CAMBIADO",8)||fs_resolve(fs_root(),"/temporal.txt")||fs_resolve(fs_root(),"/Escritorio/b.txt")){ puts("FALLO persistencia"); return 1; } puts("DISCO OK"); }
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
      dock(7); { Win *st=&wins[win_top()]; click(st->x+1+170+60,st->y+TB+322+13); if(kbd_layout()!=KBD_LATAM){ puts("FALLO boton teclado"); return 1; } } shot("20_ajustes_teclado"); closeall();
      kbd_set_layout(KBD_US); { char kv[16]; cfg_get("teclado",kv,sizeof kv); kbd_set_layout(layout_from_name(kv)); if(kbd_layout()!=KBD_LATAM){ puts("FALLO leer cfg"); return 1; } }
      gui_panic("Falta de pagina","eip",0x1234abcdULL,0x2,0xdeadbeefULL,1); shot("21_panic"); gui_force_redraw(); puts("TECLADO GUI OK"); }
    /* escritorio lleno */
    dock(0); dock(1); dock(2); dock(3); dock(4); dock(5); dock(6); shot("13_escritorio_lleno");
    return 0;
}
