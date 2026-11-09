/* gui.c — escritorio de Königreich OS: fondo, barra, dock, ventanas y aplicaciones integradas.
 * Todo se dibuja por software en un backbuffer; el cursor se pinta directo sobre el frontbuffer. */
#include "gui.h"
#include "kbd.h"
#include "gfx.h"
#include "fs.h"
#include "html.h"
#include "net.h"
#include "disk.h"

enum { A_ABOUT, A_BROWSER, A_FILES, A_TERM, A_SETTINGS, A_VIEWER, A_NOTES, A_CALC, A_SNAKE, A_MONITOR };
#define SGW 20
#define SGH 16
#define SCELL 20
#define NOTEMAX 3000
#define KNGFS_CAP (2*1024*1024)
#define MAXW 10
#define TB 30
#define BARH 28
#define TW 80
#define TH 24

typedef struct {
    int used, type, x, y, w, h; char title[40];
    char grid[TH][TW]; int tr, tc; char in[256]; int inlen; FsNode *cwd;          /* terminal */
    int busy, bpid, esc, escp, escv[6], escq, savr, savc;                         /* terminal: programa pegado, secuencias ANSI */
    FsNode *dir, *sel, *last; int scroll; uint32_t lastclick;                    /* archivos / visor */
    FsNode *file;                                                                /* visor */
    char url[192], hist[8][192], ebuf[192]; int nhist, bscroll, editing, elen; HPage page;   /* navegador */
    char *doc; int doclen, loading, selall; char err[100];                               /* pagina descargada por http */
    char note[NOTEMAX+1]; int nlen; char nstat[48];                              /* notas */
    int cacc, ccur, cop, cfresh, cerr;                                           /* calculadora */
    int rx, ry, rw, rh, maxed; uint32_t tclick;                                   /* maximizar / ajustar */
    unsigned char sx[SGW*SGH], sy[SGW*SGH]; int slen, sdir, sndir, fx, fy, score, sover, sstart; uint32_t snext, seed;   /* snake */
} Win;

static Win wins[MAXW];
static int order[MAXW], nord;
static int mx, my, mb, drag=-1, dragdx, dragdy, hover=-1;
static int dirty=1, cursor_dirty=1, cur_x=-100, cur_y=-100;
static int dr0x, dr0y, dr1x, dr1y;                           /* rectangulo sucio pendiente (vacio si dr1x<=dr0x) */
static void dmark(int x,int y,int w,int h){
    if(w<=0||h<=0) return;
    if(x<0){ w+=x; x=0; }
    if(y<0){ h+=y; y=0; }
    if(x+w>gw) w=gw-x;
    if(y+h>gh) h=gh-y;
    if(w<=0||h<=0) return;
    if(dr1x<=dr0x){ dr0x=x; dr0y=y; dr1x=x+w; dr1y=y+h; return; }
    if(x<dr0x) dr0x=x;
    if(y<dr0y) dr0y=y;
    if(x+w>dr1x) dr1x=x+w;
    if(y+h>dr1y) dr1y=y+h;
}
static void dwin(const Win *w){ dmark(w->x-14,w->y-10,w->w+28,w->h+32); }
static uint32_t *wall;
static int acc_i=0, wall_i=0, tz=-5, show24=1;
static int clk_h=12, clk_m=0; static uint32_t clk_next;
static const uint32_t ACC[6]={0x7c5cff,0x14b8a6,0xf59e0b,0xec4899,0x3b82f6,0x22c55e};
#define ACCENT ACC[acc_i]
#define C_BG 0x161826
#define C_BORDER 0x2b2e45
#define C_TXT 0xdfe3f0
#define C_DIM 0x9aa0b8
#define C_BTN 0x2a2e4a

void gui_force_redraw(void){ dirty=1; }

/* ---------------- utilidades de texto ---------------- */
static void cat(char *d,int max,const char *s){ int n=k_strlen(d); while(*s&&n<max-1) d[n++]=*s++; d[n]=0; }
static void cat_int(char *d,int max,int v){
    char b[12]; int i=0; unsigned u=v<0?-(unsigned)v:(unsigned)v;
    if(!u) b[i++]='0';
    while(u){ b[i++]=(char)('0'+u%10); u/=10; }
    if(v<0) cat(d,max,"-");
    char o[12]; int k=0; while(i) o[k++]=b[--i]; o[k]=0; cat(d,max,o);
}
static void l1_to_utf8(const unsigned char *s,int n,char *d){
    int k=0; for(int i=0;i<n;i++){ unsigned char c=s[i];
        if(c<0x80) d[k++]=(char)c; else { d[k++]=(char)(0xC0|(c>>6)); d[k++]=(char)(0x80|(c&0x3F)); } }
    d[k]=0;
}

/* ---------------- fondo de pantalla ---------------- */
/* Fondo: se calcula con 8 bits fraccionarios por canal y se cuantiza con una matriz de Bayer 4x4 (sin bandas). */
static const unsigned char BAYER[16]={0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5};
typedef struct { int c[3]; } Fx;
static Fx fx_of(uint32_t k){ Fx f; f.c[0]=(int)((k>>16)&255)<<8; f.c[1]=(int)((k>>8)&255)<<8; f.c[2]=(int)(k&255)<<8; return f; }
static void fx_mix(Fx *f,uint32_t k,int t){                /* t: 0..4096 */
    int tg[3]={(int)((k>>16)&255)<<8,(int)((k>>8)&255)<<8,(int)(k&255)<<8};
    for(int i=0;i<3;i++) f->c[i]+=((tg[i]-f->c[i])*t)>>12;
}
static void fx_glow(Fx *f,int nx,int ny,int gx,int gy,int R,uint32_t col,int strength){
    int dx=nx-gx, dy=ny-gy, d2=dx*dx+dy*dy, R2=R*R;
    if(d2>=R2) return;
    int k=((R2-d2)>>2)*4096/(R2>>2); k=(k*k)>>12; fx_mix(f,col,k*strength/255);
}
static uint32_t wall_px(int v,int x,int y,int W,int H){
    int nx=x*1024/W, ny=y*1024/H; Fx f;
    if(v==0){ f=fx_of(0x0a0f1f); fx_mix(&f,0x1c1748,k_min(4096,(nx+ny)*2)); fx_glow(&f,nx,ny,950,60,800,0x5b46c8,230); fx_glow(&f,nx,ny,0,1024,780,0x0f8a85,230); }
    else if(v==1){ f=fx_of(0x060f24); fx_mix(&f,0x0b1f4a,k_min(4096,ny*4)); fx_glow(&f,nx,ny,150,950,800,0x1aa7c9,220); fx_glow(&f,nx,ny,900,100,700,0x3c3cc0,200); }
    else { f=fx_of(0x1c0b18); fx_mix(&f,0x2a1020,k_min(4096,(nx+ny)*2)); fx_glow(&f,nx,ny,900,980,800,0xc8582a,220); fx_glow(&f,nx,ny,120,120,700,0x962878,200); }
    int d=BAYER[(y&3)*4+(x&3)]*16+8, o[3];
    for(int i=0;i<3;i++){ o[i]=(f.c[i]+d)>>8; if(o[i]>255) o[i]=255; if(o[i]<0) o[i]=0; }
    return ((uint32_t)o[0]<<16)|((uint32_t)o[1]<<8)|(uint32_t)o[2];
}
#define THW 96
#define THH 60
static uint32_t *thumb[3];                       /* miniaturas de los fondos, se calculan una sola vez */
static uint32_t *get_thumb(int i){
    if(!thumb[i]){ thumb[i]=kmalloc(THW*THH*4); if(!thumb[i]) return 0;
        for(int y=0;y<THH;y++) for(int x=0;x<THW;x++) thumb[i][y*THW+x]=wall_px(i,x*gw/THW,y*gh/THH,gw,gh); }
    return thumb[i];
}
static void build_wall(void){
    for(int y=0;y<gh;y++) for(int x=0;x<gw;x++) wall[y*gw+x]=wall_px(wall_i,x,y,gw,gh);
    uint32_t *save=bb; bb=wall; clip_reset();                                   /* marca de agua: la corona */
    int cw=gw*18/100; crown(gw*74/100,gh*38/100,cw,0xffffff,0xffffff,38);
    bb=save;
}

/* ---------------- ventanas ---------------- */
static int win_top(void){ return nord? order[nord-1] : -1; }
static void focus(int wi){
    int k=0; while(k<nord && order[k]!=wi) k++;
    if(k==nord) return;
    for(;k<nord-1;k++) order[k]=order[k+1];
    order[nord-1]=wi; dirty=1;
}
static void doc_free(Win *w);
static void win_close(int wi){
    if(wins[wi].type==A_TERM&&wins[wi].busy){ char kc[32]="matar "; cat_int(kc,sizeof kc,wins[wi].bpid); kshell_exec(kc); wins[wi].busy=0; }
    int k=0; while(k<nord && order[k]!=wi) k++;
    for(;k<nord-1;k++) order[k]=order[k+1];
    doc_free(&wins[wi]);
    nord--; wins[wi].used=0; dirty=1;
}
static void set_title(Win *w,const char *t){ k_strcpy(w->title,t,sizeof w->title); }
static void crect(Win *w,int *x,int *y,int *cw,int *ch){ *x=w->x+1; *y=w->y+TB; *cw=w->w-2; *ch=w->h-TB-1; }

static int layout_from_name(const char *n);
static void set_layout(int l);
static void term_nl(Win *w){
    w->tc=0; w->tr++;
    if(w->tr>=TH){ for(int r=1;r<TH;r++) for(int c=0;c<TW;c++) w->grid[r-1][c]=w->grid[r][c]; memset(w->grid[TH-1],' ',TW); w->tr=TH-1; }
}
static void mmove(void *d,const void *s,size_t n){ char *dd=d; const char *ss=s; if(dd<ss) while(n--) *dd++=*ss++; else { dd+=n; ss+=n; while(n--) *--dd=*--ss; } }
static void term_clear_rows(Win *w,int r0,int r1){ for(int r=r0;r<=r1&&r<TH;r++) memset(w->grid[r],' ',TW); }
static void term_csi(Win *w,unsigned char f){          /* secuencias de escape ANSI (cursor, borrar). Los colores se ignoran */
    int n=w->escv[0], m=w->escv[1]; int n1=n?n:1;
    switch(f){
    case 'A': w->tr-=n1; if(w->tr<0) w->tr=0; break;
    case 'B': w->tr+=n1; if(w->tr>=TH) w->tr=TH-1; break;
    case 'C': w->tc+=n1; if(w->tc>=TW) w->tc=TW-1; break;
    case 'D': w->tc-=n1; if(w->tc<0) w->tc=0; break;
    case 'E': w->tr+=n1; if(w->tr>=TH) w->tr=TH-1; w->tc=0; break;
    case 'F': w->tr-=n1; if(w->tr<0) w->tr=0; w->tc=0; break;
    case 'G': w->tc=n1-1; if(w->tc>=TW) w->tc=TW-1; break;
    case 'd': w->tr=n1-1; if(w->tr>=TH) w->tr=TH-1; break;
    case 'H': case 'f': w->tr=(n?n:1)-1; w->tc=(m?m:1)-1; if(w->tr>=TH) w->tr=TH-1; if(w->tc>=TW) w->tc=TW-1; break;
    case 'J': if(n==0){ memset(&w->grid[w->tr][w->tc],' ',(size_t)(TW-w->tc)); term_clear_rows(w,w->tr+1,TH-1); }
              else if(n==1){ term_clear_rows(w,0,w->tr-1); memset(w->grid[w->tr],' ',(size_t)w->tc+1); }
              else { term_clear_rows(w,0,TH-1); } break;
    case 'K': if(n==0) memset(&w->grid[w->tr][w->tc],' ',(size_t)(TW-w->tc)); else if(n==1) memset(w->grid[w->tr],' ',(size_t)w->tc+1); else memset(w->grid[w->tr],' ',TW); break;
    case 'X': { int c=n1; if(w->tc+c>TW) c=TW-w->tc; memset(&w->grid[w->tr][w->tc],' ',(size_t)c); break; }
    case 'P': { int c=n1; if(w->tc+c>TW) c=TW-w->tc; mmove(&w->grid[w->tr][w->tc],&w->grid[w->tr][w->tc+c],(size_t)(TW-w->tc-c)); memset(&w->grid[w->tr][TW-c],' ',(size_t)c); break; }
    case '@': { int c=n1; if(w->tc+c>TW) c=TW-w->tc; mmove(&w->grid[w->tr][w->tc+c],&w->grid[w->tr][w->tc],(size_t)(TW-w->tc-c)); memset(&w->grid[w->tr][w->tc],' ',(size_t)c); break; }
    case 'L': for(int k=0;k<n1;k++){ for(int r=TH-1;r>w->tr;r--) memcpy(w->grid[r],w->grid[r-1],TW); memset(w->grid[w->tr],' ',TW); } break;
    case 'M': for(int k=0;k<n1;k++){ for(int r=w->tr;r<TH-1;r++) memcpy(w->grid[r],w->grid[r+1],TW); memset(w->grid[TH-1],' ',TW); } break;
    case 's': w->savr=w->tr; w->savc=w->tc; break;
    case 'u': w->tr=w->savr; w->tc=w->savc; break;
    default: break;                                    /* m (colores), h, l, r, n...: se ignoran */
    }
}
static void term_put(Win *w,char c){
    static unsigned char pend; unsigned char u=(unsigned char)c;
    if(w->esc==1){ if(u=='['){ w->esc=2; w->escp=0; memset(w->escv,0,sizeof w->escv); w->escq=0; } else if(u=='('||u==')'||u=='#'){ w->esc=3; } else { w->esc=0; if(u=='c'){ term_clear_rows(w,0,TH-1); w->tr=w->tc=0; } else if(u=='M'){ if(w->tr>0) w->tr--; } } dwin(w); return; }
    if(w->esc==3){ w->esc=0; return; }                  /* ESC ( B  y similares: un caracter mas que se descarta */
    if(w->esc==2){
        if(u>='0'&&u<='9'){ if(w->escp<6) w->escv[w->escp]=w->escv[w->escp]*10+(u-'0'); return; }
        if(u==';'){ if(w->escp<5) w->escp++; return; }
        if(u=='?'||u=='>'||u=='!'||u=='='||u==' ') { return; }
        w->esc=0; term_csi(w,u); dwin(w); return; }
    if(u==27){ w->esc=1; return; }
    if(pend){ unsigned char l=(unsigned char)(((pend&3)<<6)|(u&0x3F)); pend=0; u=l; }
    else if(u==0xC2||u==0xC3){ pend=u; return; }
    if(u=='\n'){ term_nl(w); }
    else if(u=='\r'){ w->tc=0; }
    else if(u=='\b'){ if(w->tc>0){ w->tc--; w->grid[w->tr][w->tc]=' '; } }
    else if(u=='\t'){ int nt=(w->tc/8+1)*8; while(w->tc<nt&&w->tc<TW) w->grid[w->tr][w->tc++]=' '; }
    else if(u==7||u==0){ }
    else { if(w->tc>=TW) term_nl(w); w->grid[w->tr][w->tc++]=(char)u; }
    dwin(w);
}
static void tp(Win *w,const char *s){ while(*s) term_put(w,*s++); }
static void term_prompt(Win *w){ char p[110]; fs_path(w->cwd,p,sizeof p); cat(p,sizeof p,"> "); tp(w,p); }
static void term_done(int pid,int code){                 /* un programa pegado a una terminal termino: mensaje y prompt otra vez */
    for(int i=0;i<MAXW;i++){ Win *w=&wins[i]; if(!w->used||w->type!=A_TERM||!w->busy||w->bpid!=pid) continue;
        w->busy=0; char b[64]="";
        if(w->tc>0) tp(w,"\n");
        if(code>=0){ cat(b,sizeof b,"[termino con codigo "); cat_int(b,sizeof b,code); cat(b,sizeof b,"]\n"); }
        else { cat(b,sizeof b,"[terminado por la senal "); cat_int(b,sizeof b,-code); cat(b,sizeof b,"]\n"); }
        tp(w,b); term_prompt(w); dirty=1; return; }
}

static void nav(Win *w,const char *url,int push);
static void snake_reset(Win *w);
static Win *open_app(int type);
static void open_file(FsNode *n);

static Win *open_app(int type){
    for(int i=0;i<nord;i++){ Win *e=&wins[order[i]]; if(e->type==type && type!=A_VIEWER){ focus(order[i]); return e; } }
    int wi=-1; for(int i=0;i<MAXW;i++) if(!wins[i].used){ wi=i; break; }
    if(wi<0) return 0;
    Win *w=&wins[wi]; memset(w,0,sizeof *w); w->used=1; w->type=type;
    int off=(nord%5)*28;
    switch(type){
    case A_ABOUT:    w->w=440; w->h=320; set_title(w,"Acerca de Königreich OS"); break;
    case A_BROWSER:  w->w=720; w->h=500; set_title(w,"Navegador"); break;
    case A_FILES:    w->w=620; w->h=420; set_title(w,"Archivos"); w->dir=fs_root(); break;
    case A_TERM:     w->w=TW*8+2; w->h=TH*16+TB+1; set_title(w,"Terminal"); w->cwd=fs_root();
                     memset(w->grid,' ',sizeof w->grid); tp(w,"Königreich OS - terminal. Escribe 'help'.\n"); term_prompt(w); break;
    case A_SETTINGS: w->w=520; w->h=600; set_title(w,"Ajustes"); break;
    case A_VIEWER:   w->w=600; w->h=420; set_title(w,"Visor de texto"); break;
    case A_NOTES:    w->w=520; w->h=400; set_title(w,"Notas"); break;
    case A_CALC:     w->w=272; w->h=TB+1+316; set_title(w,"Calculadora"); break;
    case A_MONITOR:  w->w=420; w->h=TB+1+300; set_title(w,"Monitor del sistema"); break;
    case A_SNAKE:    w->w=SGW*SCELL+2; w->h=TB+1+34+SGH*SCELL; set_title(w,"Snake"); snake_reset(w); break;
    }
    w->x=60+off+(gw>800?60:0); w->y=BARH+24+off;
    if(w->x+w->w>gw) w->x=k_max(0,gw-w->w);
    if(w->y+w->h>gh-80) w->y=k_max(BARH,gh-80-w->h);
    order[nord++]=wi; dirty=1;
    if(type==A_BROWSER) nav(w,"kng://inicio",0);
    return w;
}
static void open_file(FsNode *n){
    if(n->dir) return;
    int l=k_strlen(n->name);
    if(l>5 && !k_strcmp(n->name+l-5,".html")){
        Win *b=open_app(A_BROWSER); char u[192]="file://"; char p[80]; fs_path(n,p,sizeof p); cat(u,sizeof u,p); if(b) nav(b,u,1);
    } else if(l>4 && !k_strcmp(n->name+l-4,".txt") && n->size<NOTEMAX){
        Win *nw=open_app(A_NOTES); if(nw){ nw->file=n; nw->nlen=n->size; if(n->size) memcpy(nw->note,n->data,(size_t)n->size); nw->note[nw->nlen]=0; nw->nstat[0]=0; set_title(nw,n->name); dirty=1; }
    } else { Win *v=open_app(A_VIEWER); if(v){ v->file=n; v->scroll=0; set_title(v,n->name); } }
}

/* ---------------- terminal: comandos ---------------- */
static Win *term_target;
static void term_hook(char c){ if(term_target && term_target->used && term_target->type==A_TERM) term_put(term_target,c); }
static void term_cmd(Win *w,char *line){
    while(*line==' ') line++;
    if(!*line) return;
    char *arg=line; while(*arg&&*arg!=' ') arg++;
    char cmd[16]; int n=(int)(arg-line); if(n>15) n=15; memcpy(cmd,line,(size_t)n); cmd[n]=0;
    while(*arg==' ') arg++;
    if(!k_strcmp(cmd,"help")){
        tp(w,"Archivos: ls cd pwd cat mkdir touch rm write cp mv ren df\nDisco:    disco  disco formatear si  guardar\nSistema:  info mem heap ps uptime echo clear teclado reboot halt\n"); }
    else if(!k_strcmp(cmd,"teclado")){ int l=layout_from_name(arg);
        if(l<0){ tp(w,"Uso: teclado us | latam | es   (ahora: "); tp(w,kbd_layout_name(kbd_layout())); tp(w,")\n"); }
        else { set_layout(l); tp(w,"Teclado: "); tp(w,kbd_layout_name(l)); tp(w,"\n"); } }
    else if(!k_strcmp(cmd,"clear")){ memset(w->grid,' ',sizeof w->grid); w->tr=0; w->tc=0; }
    else if(!k_strcmp(cmd,"pwd")){ char p[110]; fs_path(w->cwd,p,sizeof p); tp(w,p); tp(w,"\n"); }
    else if(!k_strcmp(cmd,"ls")){
        FsNode *d=*arg?fs_resolve(w->cwd,arg):w->cwd;
        if(!d||!d->dir){ tp(w,"ls: ruta no valida\n"); }
        else { FsNode *l[64]; int c=fs_list(d,l,64);
            for(int i=0;i<c;i++){ tp(w,l[i]->name); if(l[i]->dir) tp(w,"/"); else { char b[24]=" ("; cat_int(b,24,l[i]->size); cat(b,24," B)"); tp(w,b); } tp(w,"\n"); }
            if(!c) tp(w,"(vacio)\n"); } }
    else if(!k_strcmp(cmd,"cd")){ FsNode *d=fs_resolve(w->cwd,*arg?arg:"/"); if(d&&d->dir) w->cwd=d; else tp(w,"cd: carpeta no encontrada\n"); }
    else if(!k_strcmp(cmd,"cat")){ FsNode *f=fs_resolve(w->cwd,arg); if(f&&!f->dir){ tp(w,f->data?f->data:""); if(f->size&&f->data[f->size-1]!='\n') tp(w,"\n"); } else tp(w,"cat: archivo no encontrado\n"); }
    else if(!k_strcmp(cmd,"mkdir")){ if(!fs_mkdir(w->cwd,arg)) tp(w,"mkdir: nombre invalido o ya existe\n"); }
    else if(!k_strcmp(cmd,"touch")){ if(!fs_find(w->cwd,arg) && !fs_create(w->cwd,arg,"",0)) tp(w,"touch: nombre invalido\n"); }
    else if(!k_strcmp(cmd,"rm")){ FsNode *f=fs_resolve(w->cwd,arg); if(!f||fs_remove(f)) tp(w,"rm: no se pudo borrar\n"); else { if(w->cwd==f) w->cwd=fs_root(); } }
    else if(!k_strcmp(cmd,"cp")||!k_strcmp(cmd,"mv")||!k_strcmp(cmd,"ren")){
        char *b2=arg; while(*b2&&*b2!=' ') b2++; char a1[100]; int l1=(int)(b2-arg); if(l1>99) l1=99; memcpy(a1,arg,(size_t)l1); a1[l1]=0; while(*b2==' ') b2++;
        FsNode *src=fs_resolve(w->cwd,a1);
        if(!src||!*b2){ tp(w,"uso: cp|mv <origen> <destino>   ren <nombre> <nuevo>\n"); }
        else if(!k_strcmp(cmd,"ren")){ if(fs_rename(src,b2)) tp(w,"ren: nombre invalido o ya existe\n"); }
        else { FsNode *dd=fs_resolve(w->cwd,b2); const char *nm=src->name; FsNode *dir=dd;
            if(!dd||!dd->dir){ char pth[100]; k_strcpy(pth,b2,sizeof pth); int q=k_strlen(pth); while(q>0&&pth[q-1]!='/') q--; nm=b2+q; pth[q]=0; dir=q?fs_resolve(w->cwd,pth):w->cwd; if(q==1) dir=fs_root(); }
            if(!dir||!dir->dir||(cmd[0]=='c'?!fs_copy(src,dir,nm):fs_move(src,dir,nm))) tp(w,"no se pudo (destino invalido, ya existe o es una subcarpeta)\n"); } }
    else if(!k_strcmp(cmd,"df")){ int f,d2,by; fs_usage(&f,&d2,&by); char b[100]=""; cat_int(b,100,f); cat(b,100," archivos, "); cat_int(b,100,d2); cat(b,100," carpetas, "); cat_int(b,100,by); cat(b,100," bytes\n"); tp(w,b); }
    else if(!k_strcmp(cmd,"guardar")){ int r=kngfs_save(); tp(w,r==0?"guardado en disco\n":kngfs_active()?"no se pudo guardar (disco lleno o error)\n":"no hay disco KNGFS montado (usa: disco formatear si)\n"); }
    else if(!k_strcmp(cmd,"disco")){
        if(!k_strcmp(arg,"formatear si")){ int r=kngfs_format(); tp(w,r==0?"disco listo: los archivos se guardan solos\n":"no se pudo (no hay disco ATA o error de escritura)\n"); }
        else { char b[120]=""; if(disk_init()==0){ cat(b,120,disk_model()); cat(b,120,", "); cat_int(b,120,(int)(disk_sectors()/2048)); cat(b,120," MB, "); cat(b,120,kngfs_active()?"montado (guardado automatico)":"SIN formato kng"); }
            else cat(b,120,"no hay disco duro ATA (agrega un disco IDE a la VM)");
            tp(w,b); tp(w,"\n"); if(disk_init()==0 && !kngfs_active()) tp(w,"Para usarlo: disco formatear si   (ATENCION: borra el inicio de ese disco)\n"); } }
    else if(!k_strcmp(cmd,"write")){
        char *t=arg; while(*t&&*t!=' ') t++; char nm[32]; int l=(int)(t-arg); if(l>31) l=31; memcpy(nm,arg,(size_t)l); nm[l]=0; while(*t==' ') t++;
        if(!l||!fs_create(w->cwd,nm,t,k_strlen(t))) tp(w,"write: uso: write <archivo> <texto>\n"); }
    else { term_target=w; kcon_hook=term_hook; kshell_exec(line); }     /* el gancho queda puesto: los comandos de red responden mas tarde */
    dirty=1;
}

/* ---------------- navegador ---------------- */
static char rawbuf[4600];
static void esc_into(char *o,int max,const char *s2);
static char srcbuf[16000], srckey[192];                       /* paginas dinamicas (buscador) */
static int ci_eq(char a,char b){ if(a>='A'&&a<='Z') a+=32; if(b>='A'&&b<='Z') b+=32; return a==b; }
static int ci_find(const char *h,int hl,const char *n){       /* n en h, sin distinguir mayusculas; devuelve posicion o -1 */
    int nl=k_strlen(n); if(!nl) return -1;
    for(int i=0;i+nl<=hl;i++){ int j=0; while(j<nl && ci_eq(h[i+j],n[j])) j++; if(j==nl) return i; }
    return -1;
}
static void urlenc(char *o,int max,const char *q){
    static const char HX[]="0123456789ABCDEF"; int k=k_strlen(o);
    for(;*q && k<max-4;q++){ unsigned char c=(unsigned char)*q;
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') o[k++]=(char)c;
        else if(c==' ') o[k++]='+';
        else { o[k++]='%'; o[k++]=HX[c>>4]; o[k++]=HX[c&15]; } }
    o[k]=0;
}
static int hexv(char c){ return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; }
static void urldec(char *o,int max,const char *q){
    int k=0; for(;*q && *q!='&' && k<max-1;q++){
        if(*q=='+') o[k++]=' ';
        else if(*q=='%' && hexv(q[1])>=0 && hexv(q[2])>=0){ o[k++]=(char)(hexv(q[1])*16+hexv(q[2])); q+=2; }
        else o[k++]=*q; }
    o[k]=0;
}
static void a_html(const char *s2){ esc_into(srcbuf,sizeof srcbuf,s2); }
static void a_raw(const char *s2){ cat(srcbuf,sizeof srcbuf,s2); }
static int s_found;
static void s_walk(FsNode *d,const char *q){
    for(FsNode *c=d->child;c && s_found<30;c=c->next){
        if(c->dir){ s_walk(c,q); continue; }
        if((int)sizeof srcbuf-k_strlen(srcbuf)<700) return;
        int byname=ci_find(c->name,k_strlen(c->name),q)>=0, at=-1;
        if(c->data && c->size>0) at=ci_find(c->data,c->size,q);
        if(!byname && at<0) continue;
        char p[100]; fs_path(c,p,sizeof p);
        a_raw("<li><a href=\"file://"); a_html(p); a_raw("\">"); a_html(p); a_raw("</a>");
        if(at>=0){ int b=at, e=at; while(b>0 && c->data[b-1]!='\n' && at-b<30) b--; while(e<c->size && c->data[e]!='\n' && e-at<50) e++;
            char sn[90]; int l=e-b; if(l>88) l=88; memcpy(sn,c->data+b,(size_t)l); sn[l]=0; a_raw("<br>"); a_html(sn); }
        a_raw("</li>"); s_found++;
    }
}
/* /Sistema/kng.cfg: lineas clave=valor. cfg_set cambia o agrega una sola linea y conserva las demas. */
static void cfg_set(const char *key,const char *val){
    FsNode *d=fs_resolve(fs_root(),"/Sistema"); if(!d||!d->dir) return;
    FsNode *f=fs_find(d,"kng.cfg"); char out[1024]; int o=0, kl=k_strlen(key), done=0;
    if(f&&f->data){ const char *p=f->data; int i=0;
        while(i<f->size){ int e=i; while(e<f->size&&p[e]!='\n') e++;
            int len=e-i, same=len>kl && ci_find(p+i,kl,key)==0 && p[i+kl]=='=';
            if(same){ if(!done){ done=1; for(int k=0;key[k]&&o<1000;k++) out[o++]=key[k]; if(o<1000) out[o++]='='; for(int k=0;val[k]&&o<1000;k++) out[o++]=val[k]; if(o<1000) out[o++]='\n'; } }
            else if(len>0 && o+len+1<1000){ memcpy(out+o,p+i,(size_t)len); o+=len; out[o++]='\n'; }
            i=e+1; } }
    if(!done){ for(int k=0;key[k]&&o<1000;k++) out[o++]=key[k]; if(o<1000) out[o++]='='; for(int k=0;val[k]&&o<1000;k++) out[o++]=val[k]; if(o<1000) out[o++]='\n'; }
    fs_create(d,"kng.cfg",out,o);
}
static int cfg_get(const char *key,char *val,int max){
    val[0]=0; FsNode *f=fs_resolve(fs_root(),"/Sistema/kng.cfg"); if(!f||!f->data) return 0;
    int kl=k_strlen(key), i=0;
    while(i<f->size){ int e=i; while(e<f->size&&f->data[e]!='\n') e++;
        if(e-i>kl && ci_find(f->data+i,kl,key)==0 && f->data[i+kl]=='='){ int l=e-i-kl-1; if(l>max-1) l=max-1; memcpy(val,f->data+i+kl+1,(size_t)l); val[l]=0; for(int k=0;val[k];k++) if(val[k]=='\r') val[k]=0; return 1; }
        i=e+1; }
    return 0;
}
static int layout_from_name(const char *n){ return !k_strcmp(n,"latam")?KBD_LATAM:!k_strcmp(n,"es")?KBD_ES:!k_strcmp(n,"us")?KBD_US:-1; }
static const char *layout_code(int l){ return l==KBD_LATAM?"latam":l==KBD_ES?"es":"us"; }
static void set_layout(int l){ kbd_set_layout(l); cfg_set("teclado",layout_code(l)); dirty=1; }
static void cfg_search_url(const char *q,char *out,int max){      /* buscador=http://.../?q=%s en /Sistema/kng.cfg */
    out[0]=0; FsNode *f=fs_resolve(fs_root(),"/Sistema/kng.cfg"); if(!f||!f->data) return;
    const char *d=f->data; int i=0;
    while(i<f->size){ int e=i; while(e<f->size && d[e]!='\n') e++;
        if(e-i>9 && ci_find(d+i,9,"buscador=")==0){
            char t[160]; int l=e-i-9; if(l>158) l=158; memcpy(t,d+i+9,(size_t)l); t[l]=0;
            for(int k=0;t[k];k++){ if(t[k]=='\r') t[k]=0; }
            for(int k=0;t[k];k++){
                if(t[k]=='%'&&t[k+1]=='s'){ urlenc(out,max,q); k++; }
                else { int n=k_strlen(out); if(n<max-1){ out[n]=t[k]; out[n+1]=0; } } }
            return; }
        i=e+1; }
}
static const char *build_search(const char *url,int *len){
    if(!k_strcmp(srckey,url)){ *len=k_strlen(srcbuf); return srcbuf; }
    k_strcpy(srckey,url,sizeof srckey); srcbuf[0]=0;
    char q[100]; q[0]=0; { const char *p=url; while(*p && !(p[0]=='q'&&p[1]=='=') ) p++; if(*p) urldec(q,sizeof q,p+2); }
    a_raw("<title>Buscar</title><h1>Buscar</h1>");
    if(!q[0]){ a_raw("<p>Escribe una o varias palabras en la barra de direcciones y pulsa Enter.</p><hr><p><a href=\"kng://inicio\">Volver al inicio</a></p>"); *len=k_strlen(srcbuf); return srcbuf; }
    a_raw("<p>Resultados para: <b>"); a_html(q); a_raw("</b></p>");
    static const struct { const char *u, *t, *kw; } PG[]={
        {"kng://inicio","Inicio","inicio bienvenida enlaces home principal"},
        {"kng://ayuda","Ayuda","ayuda help navegador terminal comandos teclas buscar busqueda"},
        {"kng://acerca","Acerca de Königreich OS","acerca kernel autor sistema version nick kng"}};
    int any=0;
    for(int i=0;i<3;i++) if(ci_find(PG[i].t,k_strlen(PG[i].t),q)>=0 || ci_find(PG[i].kw,k_strlen(PG[i].kw),q)>=0){
        if(!any){ a_raw("<h2>Paginas del sistema</h2><ul>"); any=1; }
        a_raw("<li><a href=\""); a_raw(PG[i].u); a_raw("\">"); a_html(PG[i].t); a_raw("</a></li>"); }
    if(any) a_raw("</ul>");
    a_raw("<h2>Archivos</h2>"); s_found=0;
    { int mark=k_strlen(srcbuf); a_raw("<ul>"); s_walk(fs_root(),q);
      if(!s_found){ srcbuf[mark]=0; a_raw("<p>Ningun archivo contiene eso.</p>"); } else a_raw("</ul>"); }
    a_raw("<h2>Web</h2>");
    { char eu[192]; cfg_search_url(q,eu,sizeof eu);
      if(eu[0]){ a_raw("<p><a href=\""); a_html(eu); a_raw("\">Buscar en el servidor configurado</a></p>");
                 if(k_startswith(eu,"https://")) a_raw("<p>Aviso: esa direccion usa https y kng aun no tiene TLS.</p>"); }
      else a_raw("<p>No hay buscador web configurado. Los buscadores grandes usan https (kng aun no tiene TLS). Para usar uno por http, agrega en /Sistema/kng.cfg una linea como: buscador=http://10.0.2.2:8080/search?q=%s</p>"); }
    a_raw("<hr><p><a href=\"kng://inicio\">Volver al inicio</a></p>");
    *len=k_strlen(srcbuf); return srcbuf;
}
static int is_http(const char *u);
static void esc_into(char *o,int max,const char *s2);
static const char *page_src(Win *w,int *len){
    const char *b=html_builtin(w->url,len); if(b) return b;
    if(k_startswith(w->url,"kng://buscar")) return build_search(w->url,len);
    if(is_http(w->url)){
        if(w->doc){ *len=w->doclen; return w->doc; }
        rawbuf[0]=0;
        if(w->loading){ cat(rawbuf,sizeof rawbuf,"<title>Cargando...</title><h1>Cargando...</h1><p>"); esc_into(rawbuf,sizeof rawbuf,w->url); cat(rawbuf,sizeof rawbuf,"</p><p>Esperando respuesta del servidor.</p>"); }
        else { cat(rawbuf,sizeof rawbuf,"<title>Error</title><h1>No se pudo abrir la pagina</h1><p>"); esc_into(rawbuf,sizeof rawbuf,w->err[0]?w->err:"Error desconocido");
               cat(rawbuf,sizeof rawbuf,"</p><p>"); esc_into(rawbuf,sizeof rawbuf,w->url); cat(rawbuf,sizeof rawbuf,"</p><hr><p>Si buscabas en la web: ejecuta <b>tools/kng-proxy.py</b> en tu PC (puerto 8080).</p><p><a href=\"kng://inicio\">Ir al inicio</a></p>"); }
        *len=k_strlen(rawbuf); return rawbuf;
    }
    if(k_startswith(w->url,"file://")){
        FsNode *f=fs_resolve(fs_root(),w->url+7);
        if(f && !f->dir){
            int l=k_strlen(f->name);
            if(l>5 && !k_strcmp(f->name+l-5,".html")){ *len=f->size; return f->data?f->data:""; }
            int k=0; const char *pre="<title>Texto</title><pre>"; for(const char *p=pre;*p;p++) rawbuf[k++]=*p;
            for(int i=0;i<f->size && k<4500;i++){ char c=f->data[i];
                if(c=='<'){ rawbuf[k++]='&'; rawbuf[k++]='l'; rawbuf[k++]='t'; rawbuf[k++]=';'; }
                else if(c=='>'){ rawbuf[k++]='&'; rawbuf[k++]='g'; rawbuf[k++]='t'; rawbuf[k++]=';'; }
                else if(c=='&'){ rawbuf[k++]='&'; rawbuf[k++]='a'; rawbuf[k++]='m'; rawbuf[k++]='p'; rawbuf[k++]=';'; }
                else rawbuf[k++]=c; }
            for(const char *p="</pre>";*p;p++) rawbuf[k++]=*p;
            *len=k; return rawbuf;
        }
    }
    return html_builtin("kng://error",len);
}
static Win *http_owner;
static void doc_free(Win *w){
    if(w->type!=A_BROWSER) return;
    if(w->doc){ kfree(w->doc); w->doc=0; }
    w->doclen=0; w->loading=0; w->err[0]=0;
    if(http_owner==w){ http_cancel(); http_owner=0; }
}
static int is_http(const char *u){ return k_startswith(u,"http://")||k_startswith(u,"https://"); }
static void load(Win *w){                       /* inicia la descarga si la direccion es http */
    doc_free(w); srckey[0]=0;
    if(!is_http(w->url)) return;
    if(http_owner && http_owner!=w){ Win *o=http_owner; o->loading=0; k_strcpy(o->err,"Cancelado: otra ventana pidio una pagina",sizeof o->err); }
    http_owner=0;
    if(http_get(w->url)<0){ k_strcpy(w->err,http_error(),sizeof w->err); return; }
    http_owner=w; w->loading=1;
}
static void browser_poll(void){
    Win *w=http_owner; if(!w) return;
    if(!w->used || w->type!=A_BROWSER || !w->loading){ http_cancel(); http_owner=0; return; }
    int r=http_poll(); if(!r) return;
    http_owner=0; w->loading=0; dirty=1;
    if(r<0){ k_strcpy(w->err,http_error(),sizeof w->err); return; }
    int n=0; char *b=http_take(&n); if(!b){ k_strcpy(w->err,"Sin datos",sizeof w->err); return; }
    k_strcpy(w->url,http_final_url(),sizeof w->url);
    if(!http_is_html()){                         /* texto plano: se envuelve en <pre> con las entidades escapadas */
        char *o=kmalloc((size_t)n*5+64);
        if(!o){ kfree(b); k_strcpy(w->err,"Sin memoria",sizeof w->err); return; }
        int k=0; const char *pre="<title>Texto</title><pre>"; for(const char *p=pre;*p;p++) o[k++]=*p;
        for(int i=0;i<n;i++){ char c=b[i];
            if(c=='<'){ o[k++]='&'; o[k++]='l'; o[k++]='t'; o[k++]=';'; }
            else if(c=='>'){ o[k++]='&'; o[k++]='g'; o[k++]='t'; o[k++]=';'; }
            else if(c=='&'){ o[k++]='&'; o[k++]='a'; o[k++]='m'; o[k++]='p'; o[k++]=';'; }
            else o[k++]=c; }
        for(const char *p="</pre>";*p;p++) o[k++]=*p;
        o[k]=0; kfree(b); b=o; n=k;
    }
    w->doc=b; w->doclen=n; w->bscroll=0;
}
static void esc_into(char *o,int max,const char *s2){
    int k=k_strlen(o);
    for(;*s2 && k<max-6;s2++){
        if(*s2=='<'){ o[k++]='&'; o[k++]='l'; o[k++]='t'; o[k++]=';'; }
        else if(*s2=='>'){ o[k++]='&'; o[k++]='g'; o[k++]='t'; o[k++]=';'; }
        else if(*s2=='&'){ o[k++]='&'; o[k++]='a'; o[k++]='m'; o[k++]='p'; o[k++]=';'; }
        else o[k++]=*s2;
    }
    o[k]=0;
}
static const char *page_src(Win *w,int *len);
static void norm_path(char *p){                  /* quita . y .. de una ruta que empieza en '/' */
    char out[192]; int k=0; int i=0;
    while(p[i]){
        int j=i+1; while(p[j] && p[j]!='/') j++;       /* segmento p[i+1..j) */
        int sl=j-i-1;
        if(sl==2 && p[i+1]=='.' && p[i+2]=='.'){ while(k>0 && out[k-1]!='/') k--; if(k>0) k--; }
        else if(sl==1 && p[i+1]=='.'){ }
        else { if(k+sl+1<190){ out[k++]='/'; for(int q=0;q<sl;q++) out[k++]=p[i+1+q]; } }
        i=j;
    }
    if(k==0) out[k++]='/';
    else if(p[k_strlen(p)-1]=='/' && out[k-1]!='/' && k<190) out[k++]='/';
    out[k]=0; k_strcpy(p,out,192);
}
static void resolve(const char *base,const char *href,char *out,int max){
    int has=0; for(int i=0;href[i] && href[i]!='/' && href[i]!='?' && href[i]!='#';i++) if(href[i]==':'){ has=1; break; }
    if(has){ k_strcpy(out,href,max); return; }
    if(href[0]=='/' && href[1]=='/'){ out[0]=0; cat(out,max,"http:"); cat(out,max,href); return; }
    if(is_http(base)){
        int pre=k_startswith(base,"https://")?8:7;
        int e=pre; while(base[e] && base[e]!='/' && base[e]!='?' && base[e]!='#') e++;         /* fin del host[:puerto] */
        char path[192]; path[0]=0;
        if(href[0]=='/'){ k_strcpy(path,href,sizeof path); }
        else {
            int pe=e; while(base[pe] && base[pe]!='?' && base[pe]!='#') pe++;
            if(href[0]=='?'||href[0]=='#'||!href[0]){ int l=pe-e; if(l>190) l=190; memcpy(path,base+e,(size_t)l); path[l]=0; if(!path[0]) k_strcpy(path,"/",sizeof path); cat(path,sizeof path,href); }
            else { int l=pe-e; if(l>190) l=190; memcpy(path,base+e,(size_t)l); path[l]=0; if(!path[0]) k_strcpy(path,"/",sizeof path);
                   int q=k_strlen(path); while(q>0 && path[q-1]!='/') q--; path[q]=0; cat(path,sizeof path,href); }
        }
        { char tail[192]; int t=0; while(path[t] && path[t]!='?' && path[t]!='#') t++; k_strcpy(tail,path+t,sizeof tail); path[t]=0; norm_path(path); cat(path,sizeof path,tail); }
        int l=e; if(l>max-2) l=max-2; memcpy(out,base,(size_t)l); out[l]=0; cat(out,max,path); return;
    }
    if(k_startswith(base,"file://")){
        char path[192]; path[0]=0;
        if(href[0]=='/') k_strcpy(path,href,sizeof path);
        else { k_strcpy(path,base+7,sizeof path); int q=k_strlen(path); while(q>0 && path[q-1]!='/') q--; path[q]=0; cat(path,sizeof path,href); }
        norm_path(path); out[0]=0; cat(out,max,"file://"); cat(out,max,path); return;
    }
    k_strcpy(out,href,max);
}
static void nav(Win *w,const char *url,int push){
    if(push && w->url[0]){
        if(w->nhist==8){ for(int i=1;i<8;i++) memcpy(w->hist[i-1],w->hist[i],192); w->nhist=7; }
        k_strcpy(w->hist[w->nhist++],w->url,192);
    }
    k_strcpy(w->url,url,sizeof w->url); w->bscroll=0; w->editing=0; dirty=1;
    load(w);
}
static void go_back(Win *w){
    if(w->nhist<=0) return;
    k_strcpy(w->url,w->hist[--w->nhist],192); w->bscroll=0; w->editing=0; dirty=1; load(w);
}
static void browser_go_typed(Win *w){
    char u[192]; w->ebuf[w->elen]=0;
    if(k_strlen(w->ebuf)==0){ w->editing=0; return; }
    int has=0; for(int i=0;w->ebuf[i]&&w->ebuf[i+2];i++) if(w->ebuf[i]==':'&&w->ebuf[i+1]=='/'&&w->ebuf[i+2]=='/') has=1;
    int dot=0, sp=0; for(int i=0;w->ebuf[i];i++){ if(w->ebuf[i]=='.') dot=1; if(w->ebuf[i]==' ') sp=1; }
    u[0]=0;
    if(has && k_startswith(w->ebuf,"https://")){                                   /* https: a traves de kng-proxy si esta configurado */
        char b[192]; cfg_search_url("",b,sizeof b);
        if(k_startswith(b,"http://")){ int e=7; while(b[e]&&b[e]!='/') e++; b[e]=0; k_strcpy(u,b,sizeof u); cat(u,sizeof u,"/go?u="); urlenc(u,sizeof u,w->ebuf); }
        else k_strcpy(u,w->ebuf,sizeof u);
    }
    else if(has) k_strcpy(u,w->ebuf,sizeof u);
    else if(w->ebuf[0]=='/'){ cat(u,sizeof u,"file://"); cat(u,sizeof u,w->ebuf); }
    else if((dot||k_startswith(w->ebuf,"localhost")) && !sp){ cat(u,sizeof u,"http://"); cat(u,sizeof u,w->ebuf); }
    else if(!k_strcmp(w->ebuf,"inicio")||!k_strcmp(w->ebuf,"ayuda")||!k_strcmp(w->ebuf,"acerca")||!k_strcmp(w->ebuf,"error")){ cat(u,sizeof u,"kng://"); cat(u,sizeof u,w->ebuf); }
    else { cfg_search_url(w->ebuf,u,sizeof u);                                          /* palabras sueltas: buscador web (kng-proxy) o busqueda local */
           if(!u[0]){ cat(u,sizeof u,"kng://buscar?q="); urlenc(u,sizeof u,w->ebuf); } }
    nav(w,u,1);
}

/* ---------------- apps simples: Notas, Calculadora, Snake ---------------- */
static void notes_save(Win *w){
    w->note[w->nlen]=0;
    if(w->file){ fs_write(w->file,w->note,w->nlen); k_strcpy(w->nstat,"Guardado",sizeof w->nstat); return; }
    FsNode *d=fs_resolve(fs_root(),"/Documentos"); if(!d) d=fs_root();
    char nm[32]; for(int k=1;k<100;k++){ nm[0]=0; cat(nm,32,"nota"); if(k>1) cat_int(nm,32,k); cat(nm,32,".txt"); if(!fs_find(d,nm)) break; }
    FsNode *f=fs_create(d,nm,w->note,w->nlen);
    if(f){ w->file=f; set_title(w,nm); k_strcpy(w->nstat,"Guardado en /Documentos",sizeof w->nstat); } else k_strcpy(w->nstat,"No se pudo guardar",sizeof w->nstat);
}
/* texto internamente en UTF-8: el teclado entrega Latin-1 (0xA0..0xFF) y aqui se guarda como 2 bytes */
static int u8_enc(int c,char *o){ if(c>=0xA0&&c<=0xFF){ o[0]=(char)(0xC0|(c>>6)); o[1]=(char)(0x80|(c&0x3F)); return 2; } if(c>=32&&c<127){ o[0]=(char)c; return 1; } return 0; }
static int u8_last(const char *b,int len){ int n=1; while(n<len && ((unsigned char)b[len-n]&0xC0)==0x80) n++; return n; }   /* bytes del ultimo caracter */
static void notes_new(Win *w){ w->file=0; w->nlen=0; w->note[0]=0; w->nstat[0]=0; set_title(w,"Notas"); }
static void notes_key(Win *w,int c){
    char e[2]; int en=(c=='\n')?0:u8_enc(c,e);
    if(c=='\b'){ if(w->nlen>0){ w->nlen-=u8_last(w->note,w->nlen); w->note[w->nlen]=0; } }
    else if(c=='\n' && w->nlen<NOTEMAX){ w->note[w->nlen++]='\n'; w->note[w->nlen]=0; }
    else if(en && w->nlen+en<NOTEMAX){ for(int i=0;i<en;i++) w->note[w->nlen++]=e[i]; w->note[w->nlen]=0; }
    w->nstat[0]=0; dwin(w);
}
/* calculadora */
static void calc_apply(Win *w){
    int a=w->cacc, b=w->ccur, r=b; int aa=a<0?-a:a, ab=b<0?-b:b;
    switch(w->cop){
    case '+': if(aa>1000000000||ab>1000000000){ w->cerr=1; return; } r=a+b; break;
    case '-': if(aa>1000000000||ab>1000000000){ w->cerr=1; return; } r=a-b; break;
    case '*': if(aa && ab>999999999/aa){ w->cerr=1; return; } r=a*b; break;
    case '/': if(!b){ w->cerr=1; return; } r=a/b; break;
    default: break; }
    w->ccur=r;
}
static void calc_press(Win *w,int k){
    if(w->cerr && k!='C') return;
    if(k>='0'&&k<='9'){
        if(w->cfresh){ w->ccur=0; w->cfresh=0; }
        if(w->ccur>99999999||w->ccur<-99999999) return;
        w->ccur=w->ccur<0?w->ccur*10-(k-'0'):w->ccur*10+(k-'0');
    }
    else if(k=='C'){ w->cacc=w->ccur=w->cop=0; w->cfresh=1; w->cerr=0; w->ccur=0; w->cfresh=0; }
    else if(k=='<'){ if(!w->cfresh) w->ccur/=10; }
    else if(k=='n'){ w->ccur=-w->ccur; }
    else if(k=='='){ if(w->cop){ calc_apply(w); w->cop=0; } w->cfresh=1; }
    else if(k=='+'||k=='-'||k=='*'||k=='/'){ if(w->cop && !w->cfresh) calc_apply(w); w->cacc=w->ccur; w->cop=k; w->cfresh=1; }
    dwin(w);
}
typedef struct { const char *l; int k, col, row, span; } CKey;
static const CKey CKEYS[]={
    {"C",'C',0,0,1},{"<",'<',1,0,1},{"+/-",'n',2,0,1},{"/",'/',3,0,1},
    {"7",'7',0,1,1},{"8",'8',1,1,1},{"9",'9',2,1,1},{"x",'*',3,1,1},
    {"4",'4',0,2,1},{"5",'5',1,2,1},{"6",'6',2,2,1},{"-",'-',3,2,1},
    {"1",'1',0,3,1},{"2",'2',1,3,1},{"3",'3',2,3,1},{"+",'+',3,3,1},
    {"0",'0',0,4,2},{"=",'=',2,4,2}};
#define NCKEYS 18
static void ckey_rect(const CKey *k,int *x,int *y,int *w,int *h){ *x=14+k->col*62; *y=78+k->row*46; *w=k->span*56+(k->span-1)*6; *h=40; }
/* snake */
static uint32_t srnd(Win *w){ w->seed=w->seed*1103515245u+12345u; return (w->seed>>16)&0x7fff; }
static void snake_food(Win *w){
    for(int tries=0;tries<400;tries++){
        int x=(int)(srnd(w)%SGW), y=(int)(srnd(w)%SGH), ok=1;
        for(int i=0;i<w->slen;i++) if(w->sx[i]==x&&w->sy[i]==y){ ok=0; break; }
        if(ok){ w->fx=x; w->fy=y; return; } }
    w->fx=-1;
}
static void snake_reset(Win *w){
    w->slen=3; for(int i=0;i<3;i++){ w->sx[i]=(unsigned char)(8-i); w->sy[i]=8; }
    w->sdir=w->sndir=1; w->score=0; w->sover=0; w->sstart=0; w->seed^=kticks()*2654435761u+7; snake_food(w); dirty=1;
}
static void snake_step(Win *w){
    w->sdir=w->sndir;
    int nx=w->sx[0]+(w->sdir==1)-(w->sdir==3), ny=w->sy[0]+(w->sdir==2)-(w->sdir==0);
    int eat=(nx==w->fx&&ny==w->fy);
    if(nx<0||ny<0||nx>=SGW||ny>=SGH){ w->sover=1; return; }
    for(int i=0;i<w->slen-(eat?0:1);i++) if(w->sx[i]==nx&&w->sy[i]==ny){ w->sover=1; return; }
    if(eat && w->slen<SGW*SGH) w->slen++;
    for(int i=w->slen-1;i>0;i--){ w->sx[i]=w->sx[i-1]; w->sy[i]=w->sy[i-1]; }
    w->sx[0]=(unsigned char)nx; w->sy[0]=(unsigned char)ny;
    if(eat){ w->score++; snake_food(w); if(w->fx<0) w->sover=1; }
}
static void snake_key(Win *w,int c){
    int d=-1;
    if(c==KEY_UP||c=='w'||c=='W') d=0; else if(c==KEY_RIGHT||c=='d'||c=='D') d=1; else if(c==KEY_DOWN||c=='s'||c=='S') d=2; else if(c==KEY_LEFT||c=='a'||c=='A') d=3;
    if(c==' '||c=='\n'){ if(w->sover||!w->sstart){ snake_reset(w); w->sstart=1; w->snext=kticks()+12; } return; }
    if(d>=0 && ((d^2)!=w->sdir)){ w->sndir=d; if(!w->sstart && !w->sover){ w->sstart=1; w->snext=kticks()+12; } }
}
static void snake_tick(uint32_t t){
    for(int i=0;i<nord;i++){ Win *w=&wins[order[i]]; if(w->type!=A_SNAKE||i!=nord-1) continue;
        if(w->sstart && !w->sover && t>=w->snext){ snake_step(w); int sp=14-w->score/2; if(sp<5) sp=5; w->snext=t+(uint32_t)sp; dwin(w); } }
}

/* ---------------- dibujo comun ---------------- */
static void button(int x,int y,int w,int h,const char *label,int primary){
    rrect(x,y,w,h,6,0xffffff,primary?40:20); rrect(x+1,y+1,w-2,h-2,5,primary?ACCENT:C_BTN,255);
    clip_set(x+1,y+1,w-2,h/2); rrect(x+1,y+1,w-2,h-2,5,0xffffff,14); clip_reset();
    text(x+(w-text_w(label))/2,y+(h-16)/2,label,0xffffff);
}
static void draw_folder(int x,int y){ rrect(x,y+2,10,6,2,0xe0a62d,255); rrect(x,y+5,22,14,3,0xf5b83d,255); }
static void draw_doc(int x,int y){ rrect(x+2,y+1,16,20,2,0xe8eaf4,255); fill(x+5,y+8,10,1,0x8a8fa8,255); fill(x+5,y+12,10,1,0x8a8fa8,255); fill(x+5,y+16,7,1,0x8a8fa8,255); }

static int win_resizable(const Win *w){ return w->type==A_BROWSER||w->type==A_FILES||w->type==A_NOTES||w->type==A_VIEWER; }
static void win_restore(Win *w){ if(!w->maxed) return; dwin(w); w->x=w->rx; w->y=w->ry; w->w=w->rw; w->h=w->rh; w->maxed=0; dirty=1; }
static void win_set_max(Win *w,int x,int y,int ww,int hh){
    if(!w->maxed){ w->rx=w->x; w->ry=w->y; w->rw=w->w; w->rh=w->h; }
    w->x=x; w->y=y; w->w=ww; w->h=hh; w->maxed=1; dirty=1;
}
static void win_toggle_max(Win *w){ if(w->maxed) win_restore(w); else win_set_max(w,0,BARH,gw,gh-BARH-86); }
static int text_w_big(const char *s){ return text_w(s)*3/2; }
static void draw_window(int wi,int focused){
    Win *w=&wins[wi]; int x=w->x,y=w->y; int focused_cursor=focused;
    { int X0=x-8,Y0=y-6,X1=x+w->w+8,Y1=y+w->h+14;                                     /* sombra suave, solo fuera de la ventana */
      for(int bnd=0;bnd<4;bnd++){
          if(bnd==0) clip_set(X0,Y0,X1-X0,y-Y0); else if(bnd==1) clip_set(X0,y+w->h,X1-X0,Y1-(y+w->h));
          else if(bnd==2) clip_set(X0,y,x-X0,w->h); else clip_set(x+w->w,y,X1-(x+w->w),w->h);
          for(int i=3;i>=1;i--) rrect(x-i*2,y-i*2+6,w->w+4*i,w->h+4*i,11+i*2,0x000000,16);
      }
      clip_reset(); }
    rrect(x,y,w->w,w->h,10,focused?mix(C_BORDER,ACCENT,60):C_BORDER,255);
    rrect(x+1,y+1,w->w-2,w->h-2,9,C_BG,255);
    clip_set(x+1,y+1,w->w-2,TB-1);
    rrect(x+1,y+1,w->w-2,TB+10,9,focused?mix(0x20233a,ACCENT,70):0x1b1d30,255);
    { static const unsigned char INS[9]={6,4,3,2,1,1,0,0,0};                         /* brillo degradado del titulo */
      for(int j=0;j<TB-1;j++){ int in=j<9?INS[j]:0; fill(x+1+in,y+1+j,w->w-2-2*in,1,0xffffff,(focused?20:10)-j*(focused?20:10)/(TB-1)); } }
    clip_reset();
    fill(x+1,y+TB-1,w->w-2,1,focused?ACCENT:0x000000,focused?110:60);
    circle(x+w->w-20,y+TB/2,7,focused?0xe5534b:0x4a4d63,255);                     /* boton cerrar */
    line(x+w->w-23,y+TB/2-3,x+w->w-17,y+TB/2+3,1,0xffffff,focused?255:120);
    line(x+w->w-17,y+TB/2-3,x+w->w-23,y+TB/2+3,1,0xffffff,focused?255:120);

    int cx,cy,cw,ch; crect(w,&cx,&cy,&cw,&ch);
    clip_set(cx,cy,cw,ch);
    switch(w->type){
    case A_ABOUT:{
        crown(cx+cw/2-60,cy+24,120,0xe9c46a,ACCENT,255);
        int tw=12*13; text_big(cx+(cw-tw)/2,cy+130,"Königreich OS",0xffffff);
        { char vb[80]="kng-kernel " KNG_VERSION " (" KNG_ARCH ")  -  sistema operativo propio"; text(cx+(cw-text_w(vb))/2,cy+166,vb,C_DIM); }
        text(cx+(cw-text_w("No es Linux ni usa su codigo."))/2,cy+190,"No es Linux ni usa su codigo.",C_TXT);
        text(cx+(cw-text_w("Autor: Nick Jose Tolentino Rojas"))/2,cy+226,"Autor: Nick Jose Tolentino Rojas",C_DIM);
        break; }
    case A_TERM:{
        fill(cx,cy,cw,ch,0x0d0e16,255);
        for(int r=0;r<TH;r++){ char u[TW*2+1]; l1_to_utf8((unsigned char*)w->grid[r],TW,u); text(cx,cy+r*16,u,0xd6dbe8); }
        if(focused && ((kticks()/50)&1)) fill(cx+w->tc*8,cy+w->tr*16+13,8,3,ACCENT,255);
        break; }
    case A_FILES:{
        static const int FX[4]={8,74,164,254}, FW[4]={60,84,84,76}; static const char *FL[4]={"Subir","Carpeta+","Archivo+","Eliminar"};
        for(int i=0;i<4;i++) button(cx+FX[i],cy+6,FW[i],24,FL[i],0);
        rrect(cx+8,cy+36,cw-16,24,6,0x10111b,255);
        char p[110]; fs_path(w->dir,p,sizeof p); text(cx+16,cy+40,p,C_TXT);
        FsNode *l[96]; int n=fs_list(w->dir,l,96);
        int ly=cy+68, rows=(ch-68-22)/26;
        for(int i=0;i<rows && i+w->scroll<n;i++){
            FsNode *f=l[i+w->scroll]; int ry=ly+i*26;
            if(f==w->sel) rrect(cx+6,ry,cw-12,25,5,ACCENT,90);
            if(f->dir) draw_folder(cx+14,ry+2); else draw_doc(cx+16,ry+2);
            text(cx+46,ry+4,f->name,C_TXT);
            if(!f->dir){ char b[24]=""; cat_int(b,24,f->size); cat(b,24," B"); text(cx+cw-16-text_w(b),ry+4,b,C_DIM); }
        }
        if(!n) text(cx+16,ly+6,"(carpeta vacia)",C_DIM);
        fill(cx,cy+ch-22,cw,22,0x10111b,255);
        char b[40]=""; cat_int(b,40,n); cat(b,40,n==1?" elemento":" elementos"); text(cx+10,cy+ch-19,b,C_DIM);
        break; }
    case A_VIEWER:{
        fill(cx,cy,cw,ch,0x10111b,255);
        if(w->file && w->file->data){
            const char *d=w->file->data; int line=0, i=0, rows=(ch-8)/16;
            while(d[i] && line<w->scroll+rows){
                int s=i; while(d[i]&&d[i]!='\n') i++;
                if(line>=w->scroll){ char tmp[200]; int l=i-s; if(l>199) l=199; memcpy(tmp,d+s,(size_t)l); tmp[l]=0; text(cx+10,cy+6+(line-w->scroll)*16,tmp,C_TXT); }
                line++; if(d[i]=='\n') i++;
            }
        }
        break; }
    case A_BROWSER:{
        button(cx+8,cy+6,32,24,"<",0);
        rrect(cx+48,cy+6,cw-56,24,6,0x10111b,255);
        char shown[100]; if(w->editing){ k_strcpy(shown,w->ebuf,sizeof shown); } else k_strcpy(shown,w->url,sizeof shown);
        if(w->editing && w->selall) fill(cx+57,cy+9,text_w(shown)+2,18,ACCENT,150);
        text(cx+58,cy+10,shown,w->editing?0xffffff:C_DIM);
        if(w->editing && ((kticks()/50)&1)) fill(cx+58+text_w(shown),cy+10,2,16,ACCENT,255);
        int src_len; const char *src=page_src(w,&src_len);
        int vx=cx+12, vy=cy+38, vw=cw-24, vh=ch-38;
        html_render(src,src_len,vx,vy,vw,vh,w->bscroll,mix(0x8ab4ff,ACCENT,60),1,&w->page);
        { int maxs=k_max(0,w->page.content_h-vh); if(w->bscroll>maxs) w->bscroll=maxs; }
        if(w->page.title[0]){ char t[40]="Navegador - "; cat(t,40,w->page.title); k_strcpy(w->title,t,40); }
        if(w->page.content_h>vh){ int bh=vh*vh/w->page.content_h; int by=vy+w->bscroll*(vh-bh)/k_max(1,w->page.content_h-vh); rrect(cx+cw-8,by,4,bh,2,C_DIM,160); }
        break; }
    case A_MONITOR:{
        char b[100]; int f,dr,by; fs_usage(&f,&dr,&by); int tot=(int)(kram_kb()/1024);
        text_bold(cx+20,cy+14,"Memoria",C_TXT);
        b[0]=0; cat(b,100,"RAM detectada: "); cat_int(b,100,tot); cat(b,100," MB"); text(cx+20,cy+40,b,C_TXT);
        text_bold(cx+20,cy+88,"Archivos",C_TXT);
        b[0]=0; cat_int(b,100,f); cat(b,100," archivos, "); cat_int(b,100,dr); cat(b,100," carpetas, "); cat_int(b,100,by); cat(b,100," bytes"); text(cx+20,cy+110,b,C_TXT);
        rrect(cx+20,cy+134,cw-40,14,7,0x10111b,255); { int pct=by*100/(KNGFS_CAP); if(pct<1&&by>0) pct=1; if(pct>100) pct=100; if(pct) rrect(cx+20,cy+134,(cw-40)*pct/100,14,7,ACCENT,255); }
        text_bold(cx+20,cy+164,"Disco",C_TXT);
        b[0]=0; if(disk_sectors()){ cat(b,100,disk_model()); cat(b,100,kngfs_active()?"  (guardado automatico)":"  (sin formato kng)"); } else cat(b,100,"sin disco (los archivos se pierden al apagar)");
        text(cx+20,cy+186,b,disk_sectors()&&kngfs_active()?0x7ee787:C_DIM);
        text_bold(cx+20,cy+216,"Red",C_TXT);
        { char nb[90]; net_info(nb,sizeof nb); nb[44]=0; text(cx+20,cy+238,nb,net_state()>=2?0x7ee787:C_DIM); }
        b[0]=0; cat(b,100,"Ventanas: "); cat_int(b,100,nord); cat(b,100,"   Activo: "); cat_int(b,100,(int)(kticks()/6000)); cat(b,100," min"); text(cx+20,cy+266,b,C_DIM);
        break; }
    case A_NOTES:{
        button(cx+8,cy+6,70,24,"Nueva",0); button(cx+86,cy+6,80,24,"Guardar",1);
        if(w->nstat[0]) text(cx+178,cy+10,w->nstat,0x7ee787);
        else { char b[40]=""; cat_int(b,40,w->nlen); cat(b,40," / "); cat_int(b,40,NOTEMAX); text(cx+cw-16-text_w(b),cy+10,b,C_DIM); }
        rrect(cx+8,cy+38,cw-16,ch-46,8,0x10111b,255);
        int cols=(cw-40)/8, rows=(ch-46-16)/16, total=1, col=0;
        for(int i=0;i<w->nlen;i++){ if(w->note[i]=='\n'||col>=cols){ total++; col=0; if(w->note[i]=='\n') continue; } col++; }
        int skip=total>rows?total-rows:0, row=0; col=0;
        clip_set(cx+8,cy+38,cw-16,ch-46);
        for(int i=0;i<=w->nlen;i++){
            if(i<w->nlen && w->note[i]=='\n'){ row++; col=0; continue; }
            if(col>=cols){ row++; col=0; }
            if(i<w->nlen){ char t2[2]={w->note[i],0}; if(row>=skip) text(cx+20+col*8,cy+46+(row-skip)*16,t2,C_TXT); col++; }
            else if(focused_cursor && ((kticks()/50)&1) && row>=skip) fill(cx+20+col*8,cy+46+(row-skip)*16+13,8,3,ACCENT,255);
        }
        clip_reset();
        break; }
    case A_CALC:{
        rrect(cx+14,cy+10,cw-28,58,10,0x10111b,255);
        char b[16]=""; if(w->cerr) k_strcpy(b,"Error",16); else cat_int(b,16,w->ccur);
        text_big(cx+cw-28-text_w_big(b),cy+27,b,w->cerr?0xff7b72:0xffffff);
        if(w->cop){ char o[2]={w->cop=='*'?'x':(char)w->cop,0}; text(cx+24,cy+16,o,ACCENT); }
        for(int i=0;i<NCKEYS;i++){ int x,y,kw,kh; ckey_rect(&CKEYS[i],&x,&y,&kw,&kh); int k=CKEYS[i].k;
            uint32_t bc=(k>='0'&&k<='9')?0x2a2e4a:(k=='=')?ACCENT:(k=='C'||k=='<'||k=='n')?0x3a3550:mix(0x2a2e4a,ACCENT,90);
            rrect(x,y,kw,kh,8,0xffffff,16); rrect(x+1,y+1,kw-2,kh-2,7,bc,255);
            text_bold(x+(kw-text_w(CKEYS[i].l))/2,y+12,CKEYS[i].l,0xffffff); }
        break; }
    case A_SNAKE:{
        fill(cx,cy,cw,34,0x141626,255);
        { char b[40]="Puntos: "; cat_int(b,40,w->score); text_bold(cx+14,cy+9,b,0xffffff); }
        text(cx+cw-14-text_w(w->sover?"Espacio: reiniciar":w->sstart?"Flechas / WASD":"Pulsa una flecha"),cy+9,w->sover?"Espacio: reiniciar":w->sstart?"Flechas / WASD":"Pulsa una flecha",C_DIM);
        int ox=cx, oy=cy+34;
        for(int j=0;j<SGH;j++) for(int i=0;i<SGW;i++) fill(ox+i*SCELL,oy+j*SCELL,SCELL,SCELL,((i+j)&1)?0x12141f:0x161826,255);
        if(w->fx>=0){ circle(ox+w->fx*SCELL+10,oy+w->fy*SCELL+11,7,0xef4444,255); fill(ox+w->fx*SCELL+10,oy+w->fy*SCELL+1,2,4,0x4ade80,255); }
        for(int i=w->slen-1;i>=0;i--){ uint32_t c=mix(0x4ade80,0x166534,i*160/k_max(1,w->slen)); if(w->sover) c=mix(c,0x888888,150);
            rrect(ox+w->sx[i]*SCELL+1,oy+w->sy[i]*SCELL+1,SCELL-2,SCELL-2,6,c,255); }
        { int hx=ox+w->sx[0]*SCELL+10, hy=oy+w->sy[0]*SCELL+10; circle(hx-4,hy-3,2,0x000000,255); circle(hx+4,hy-3,2,0x000000,255); }
        if(w->sover){ rrect(cx+cw/2-110,oy+SGH*SCELL/2-34,220,68,12,0x05060c,225); text_big(cx+cw/2-text_w_big("Fin del juego")/2,oy+SGH*SCELL/2-24,"Fin del juego",0xffffff);
            text(cx+cw/2-text_w("Espacio para reintentar")/2,oy+SGH*SCELL/2+10,"Espacio para reintentar",C_DIM); }
        break; }
    case A_SETTINGS:{
        text_bold(cx+20,cy+14,"Color de acento",C_TXT);
        for(int i=0;i<6;i++){ int sx=cx+20+i*44+14, sy=cy+54; if(i==acc_i) circle(sx,sy,18,0xffffff,255); circle(sx,sy,14,ACC[i],255); }
        text_bold(cx+20,cy+88,"Fondo de pantalla",C_TXT);
        for(int i=0;i<3;i++){ int tx2=cx+20+i*112, ty=cy+114;
            if(i==wall_i) rrect(tx2-3,ty-3,102,66,7,0xffffff,255);
            rrect(tx2,ty,96,60,5,0x000000,255);
            { uint32_t *th=get_thumb(i); if(th) for(int yy=2;yy<58;yy++) for(int xx=2;xx<94;xx++) px_blend(tx2+xx,ty+yy,th[yy*THW+xx],255); } }
        text_bold(cx+20,cy+192,"Hora",C_TXT);
        button(cx+20,cy+218,28,24,"-",0); button(cx+56,cy+218,28,24,"+",0);
        { char b[40]="Zona horaria: UTC"; if(tz>=0) cat(b,40,"+"); cat_int(b,40,tz); text(cx+98,cy+222,b,C_TXT); }
        button(cx+20,cy+252,190,24,show24?"Reloj de 24 horas: si":"Reloj de 24 horas: no",0);
        text_bold(cx+20,cy+296,"Teclado",C_TXT);
        for(int i=0;i<3;i++) button(cx+20+i*150,cy+322,140,26,kbd_layout_name(i),kbd_layout()==i);
        text(cx+20,cy+354,"Tambien: escribe  teclado latam | es | us  en la terminal",C_DIM);
        text_bold(cx+20,cy+380,"Sistema",C_TXT);
        { char b[80];
          b[0]=0; cat(b,80,"Pantalla: "); cat_int(b,80,gw); cat(b,80," x "); cat_int(b,80,gh); text(cx+20,cy+404,b,C_DIM);
          b[0]=0; cat(b,80,"Memoria: "); cat_int(b,80,(int)(kram_kb()/1024)); cat(b,80," MB"); text(cx+20,cy+424,b,C_DIM);
          text(cx+20,cy+444,"Kernel: kng-kernel " KNG_VERSION " (" KNG_ARCH ")",C_DIM);
          b[0]=0; cat(b,80,"Tiempo activo: "); cat_int(b,80,(int)(kticks()/6000)); cat(b,80," min"); text(cx+20,cy+464,b,C_DIM); }
        { char nb[90]; net_info(nb,sizeof nb); text(cx+20,cy+484,nb,C_DIM); }
        button(cx+20,cy+ch-44,110,28,"Reiniciar",0); button(cx+142,cy+ch-44,110,28,"Apagar",0);
        break; }
    }
    clip_reset();
    { int tx=x+14; if(w->type==A_ABOUT){ crown(x+12,y+9,18,0xe9c46a,ACCENT,255); tx=x+38; }
      clip_set(x+1,y+1,w->w-40,TB-1); text_bold(tx,y+7,w->title,focused?0xffffff:C_DIM); clip_reset(); }
}

/* ---------------- barra superior y dock ---------------- */
static void draw_topbar(void){
    for(int j=0;j<BARH;j++) fill(0,j,gw,1,0x05060c,190-j*70/BARH);
    fill(0,BARH-1,gw,1,0xffffff,14);
    crown(12,6,22,0xe9c46a,ACCENT,255); text(40,6,"Königreich OS",C_TXT);
    char t[16]=""; int h=clk_h;
    if(!show24){ int am=h>=12; h=h%12; if(!h) h=12; if(h<10) cat(t,16," "); cat_int(t,16,h); cat(t,16,":"); if(clk_m<10) cat(t,16,"0"); cat_int(t,16,clk_m); cat(t,16,am?" PM":" AM"); }
    else { if(h<10) cat(t,16,"0"); cat_int(t,16,h); cat(t,16,":"); if(clk_m<10) cat(t,16,"0"); cat_int(t,16,clk_m); }
    text_bold((gw-text_w(t))/2,6,t,0xffffff);
    { int st=net_state(), nx=gw-132; for(int b=0;b<4;b++) fill(nx+b*5,BARH/2+6-(b+1)*3,3,(b+1)*3,st>=2?0x7ee787:C_DIM,st>=2?255:(b==0?200:70)); }
    { const char *lc=layout_code(kbd_layout()); char up[4]={(char)(lc[0]-32),(char)(lc[1]?lc[1]-32:0),(char)(lc[1]&&lc[2]?lc[2]-32:0),0}; text(gw-86-(k_strlen(up)-2)*4,6,up,C_DIM); } circle(gw-26,BARH/2,7,0x000000,0); circle(gw-26,BARH/2,6,C_DIM,255); circle(gw-26,BARH/2,4,0x05060c,255); fill(gw-27,BARH/2-9,2,8,C_DIM,255);
}
#define NDOCK 9
#define TILE 52
static void dock_geom(int *bx,int *by,int *bw,int *bh){ *bw=NDOCK*TILE+(NDOCK-1)*14+28; *bh=TILE+16; *bx=(gw-*bw)/2; *by=gh-*bh-10; }
static const int DOCK_APP[NDOCK]={A_ABOUT,A_BROWSER,A_FILES,A_TERM,A_NOTES,A_CALC,A_SNAKE,A_SETTINGS,A_MONITOR};
static const char *DOCK_LBL[NDOCK]={"Inicio","Navegador","Archivos","Terminal","Notas","Calculadora","Snake","Ajustes","Monitor"};
static void draw_icon(int i,int x,int y){
    rrect(x,y,TILE,TILE,13,0x1a1d33,255);
    clip_set(x,y,TILE,TILE/2); rrect(x,y,TILE,TILE,13,0xffffff,13); clip_reset();           /* brillo superior */
    rrect(x,y,TILE,TILE,13,0xffffff,10);
    int cx=x+TILE/2, cy=y+TILE/2;
    switch(i){
    case 0: crown(x+9,y+11,34,0xe9c46a,ACCENT,255); break;
    case 1: circle(cx,cy,19,0xf97316,255); circle(cx,cy,15,0x1a1d33,255); ellipse_ring(cx,cy,7,15,0xf97316);
            line(cx-15,cy,cx+15,cy,2,0xf97316,255); line(cx-12,cy-8,cx+12,cy-8,1,0xf97316,255); line(cx-12,cy+8,cx+12,cy+8,1,0xf97316,255); break;
    case 2: rrect(x+9,y+14,16,9,3,0xe0a62d,255); rrect(x+9,y+19,34,22,4,0xf5b83d,255); fill(x+13,y+25,26,2,0xffffff,70); break;
    case 3: rrect(x+7,y+11,38,30,5,0x0d0e16,255); line(x+14,y+19,x+21,y+25,2,0x7ee787,255); line(x+21,y+25,x+14,y+31,2,0x7ee787,255); fill(x+25,y+31,10,3,0x7ee787,255); break;
    case 4: rrect(x+11,y+8,28,36,5,0xf4f1e6,255); rrect(x+11,y+8,28,9,4,0xf59e0b,255); fill(x+11,y+13,28,4,0xf59e0b,255);
            for(int k=0;k<3;k++) fill(x+16,y+22+k*7,18,2,0x8a8fa8,255);
            line(x+34,y+42,x+44,y+30,3,0x60a5fa,255); break;
    case 5: rrect(x+10,y+7,32,38,6,0x2b2f4d,255); rrect(x+14,y+11,24,10,3,0x7ee787,230);
            for(int r=0;r<3;r++){
                for(int c2=0;c2<3;c2++) rrect(x+15+c2*8,y+26+r*6,6,4,2,c2==2?ACCENT:0xc9cde0,255);
            }
            break;
    case 6: line(x+12,y+40,x+12,y+26,6,0x4ade80,255); line(x+12,y+26,x+30,y+26,6,0x22c55e,255); line(x+30,y+26,x+30,y+14,6,0x4ade80,255);
            circle(x+30,y+13,5,0x86efac,255); circle(x+28,y+12,1,0x000000,255); circle(x+32,y+12,1,0x000000,255); circle(x+40,y+38,5,0xef4444,255); break;
    case 7:{ static const short C8[8]={1000,707,0,-707,-1000,-707,0,707}, S8[8]={0,707,1000,707,0,-707,-1000,-707};
            for(int k=0;k<8;k++) circle(cx+17*C8[k]/1000,cy+17*S8[k]/1000,4,0x14b8a6,255);
            circle(cx,cy,15,0x14b8a6,255); circle(cx,cy,6,0x1a1d33,255); break; }
    case 8: rrect(x+9,y+10,34,32,5,0x0d0e16,255); fill(x+14,y+30,5,9,0x4ade80,255); fill(x+23,y+22,5,17,0x60a5fa,255); fill(x+32,y+16,5,23,0xf59e0b,255); break;
    }
}
static void draw_dock(void){
    int bx,by,bw,bh; dock_geom(&bx,&by,&bw,&bh);
    rrect(bx,by,bw,bh,18,0x05060c,150); rrect(bx,by,bw,bh,18,0xffffff,16);
    for(int i=0;i<NDOCK;i++){
        int x=bx+14+i*(TILE+14), y=by+8-(hover==i?4:0);
        if(hover==i) rrect(x-3,y-3,TILE+6,TILE+6,15,ACCENT,70);
        draw_icon(i,x,y);
        for(int k=0;k<nord;k++) if(wins[order[k]].type==DOCK_APP[i]){ circle(x+TILE/2,by+bh-5,2,0xffffff,255); break; }
        if(hover==i){ int lw=text_w(DOCK_LBL[i])+16; rrect(x+TILE/2-lw/2,by-30,lw,22,6,0x05060c,220); text(x+TILE/2-lw/2+8,by-27,DOCK_LBL[i],0xffffff); }
    }
}

/* ---------------- escritorio modular: iconos y widgets que se arrastran ---------------- */
typedef struct { int x, y, w, h, kind; } DObj;           /* kind 0..7 = icono de la app del dock; 100 reloj; 101 sistema */
#define NDO 7
static DObj dobj[NDO];
static int ddrag=-1, ddx, ddy, dmoved, dlast=-1; static uint32_t dlastt;
static void dobj_init(void){
    static const int ik[4]={1,2,4,6};
    for(int i=0;i<4;i++){ dobj[i].kind=ik[i]; dobj[i].w=76; dobj[i].h=84; dobj[i].x=22; dobj[i].y=BARH+18+i*96; }
    dobj[4].kind=100; dobj[4].w=210; dobj[4].h=92; dobj[4].x=gw-232; dobj[4].y=BARH+18;
    dobj[5].kind=101; dobj[5].w=210; dobj[5].h=108; dobj[5].x=gw-232; dobj[5].y=BARH+124;
    dobj[6].kind=-1;
}
static void draw_dobj(const DObj *o){
    if(o->kind>=0 && o->kind<8){
        draw_icon(o->kind,o->x+12,o->y);
        int lw=text_w(DOCK_LBL[o->kind]); rrect(o->x+o->w/2-lw/2-5,o->y+TILE+6,lw+10,20,6,0x05060c,130);
        text(o->x+o->w/2-lw/2,o->y+TILE+8,DOCK_LBL[o->kind],0xffffff);
    } else if(o->kind==100){
        rrect(o->x,o->y,o->w,o->h,16,0x05060c,130); rrect(o->x,o->y,o->w,o->h,16,0xffffff,12);
        char t[16]=""; int h=clk_h; if(h<10) cat(t,16,"0"); cat_int(t,16,h); cat(t,16,":"); if(clk_m<10) cat(t,16,"0"); cat_int(t,16,clk_m);
        { int tw=k_strlen(t)*12; text_big(o->x+(o->w-tw)/2,o->y+22,t,0xffffff); text_big(o->x+(o->w-tw)/2+1,o->y+22,t,0xffffff); }
        { char z[40]="Zona horaria UTC"; if(tz>=0) cat(z,40,"+"); cat_int(z,40,tz); text(o->x+(o->w-text_w(z))/2,o->y+62,z,C_DIM); }
    } else if(o->kind==101){
        rrect(o->x,o->y,o->w,o->h,16,0x05060c,130); rrect(o->x,o->y,o->w,o->h,16,0xffffff,12);
        text_bold(o->x+14,o->y+10,"Sistema",0xffffff);
        char b[48]; b[0]=0; cat(b,48,"RAM: "); cat_int(b,48,(int)(kram_kb()/1024)); cat(b,48," MB"); text(o->x+14,o->y+34,b,C_TXT);
        b[0]=0; cat(b,48,"Activo: "); cat_int(b,48,(int)(kticks()/6000)); cat(b,48," min"); text(o->x+14,o->y+54,b,C_TXT);
        b[0]=0; cat(b,48,net_state()>=2?"Red: conectada":"Red: sin conexion"); text(o->x+14,o->y+74,b,net_state()>=2?0x7ee787:C_DIM);
    }
}
static int dobj_hit(int px,int py){ for(int i=NDO-1;i>=0;i--) if(dobj[i].kind>=0 && px>=dobj[i].x&&px<dobj[i].x+dobj[i].w&&py>=dobj[i].y&&py<dobj[i].y+dobj[i].h) return i; return -1; }

/* ---------------- cursor ---------------- */
#define CW 12
#define CH 19
static const char *CUR[CH]={
"X           ","XX          ","X.X         ","X..X        ","X...X       ","X....X      ","X.....X     ","X......X    ",
"X.......X   ","X........X  ","X.....XXXXX ","X..X..X     ","X.X X..X    ","XX  X..X    ","X    X..X   ","     X..X   ","      X..X  ","      X..X  ","       XX   "};
static void cursor_restore(void){ gfx_present_rect(cur_x,cur_y,CW+1,CH+1); }
static void cursor_draw(void){
    for(int j=0;j<CH;j++) for(int i=0;i<CW;i++){ char c=CUR[j][i]; if(c=='X') gfx_front_px(mx+i,my+j,0x000000); else if(c=='.') gfx_front_px(mx+i,my+j,0xffffff); }
    cur_x=mx; cur_y=my;
}

/* ---------------- render ---------------- */
static int rect_in(int ax,int ay,int aw,int ah,int bx,int by,int bw,int bh){ return ax>=bx&&ay>=by&&ax+aw<=bx+bw&&ay+ah<=by+bh; }
static int rect_hit(int ax,int ay,int aw,int ah,int bx,int by,int bw,int bh){ return ax<bx+bw&&ax+aw>bx&&ay<by+bh&&ay+ah>by; }
static void render(void){
    int x0=0,y0=0,x1=gw,y1=gh;
    if(!dirty){ x0=dr0x; y0=dr0y; x1=dr1x; y1=dr1y; }
    dr0x=dr0y=dr1x=dr1y=0;
    if(x1<=x0||y1<=y0){ dirty=0; return; }
    int rw=x1-x0, rh=y1-y0;
    clip_base(x0,y0,rw,rh);
    for(int j=y0;j<y1;j++) memcpy(bb+j*gw+x0,wall+j*gw+x0,(size_t)rw*4);
    for(int i=0;i<NDO;i++) if(dobj[i].kind>=0 && rect_hit(dobj[i].x-6,dobj[i].y-6,dobj[i].w+12,dobj[i].h+12,x0,y0,rw,rh)) draw_dobj(&dobj[i]);
    for(int k=0;k<nord;k++){
        Win *w=&wins[order[k]];
        if(!rect_hit(w->x-14,w->y-10,w->w+28,w->h+32,x0,y0,rw,rh)) continue;
        int hidden=0; for(int j=k+1;j<nord;j++){ Win *o=&wins[order[j]]; if(rect_in(w->x-14,w->y-10,w->w+28,w->h+32,o->x,o->y,o->w,o->h)){ hidden=1; break; } }
        if(!hidden) draw_window(order[k],k==nord-1);
    }
    draw_topbar(); draw_dock();
    gfx_present_rect(x0,y0,rw,rh);
    clip_base(0,0,gw,gh);
    cur_x=-100; cursor_draw(); dirty=0; cursor_dirty=0;
}
void gui_frame(void){
    uint32_t t=kticks();
    browser_poll();
    if(t>=clk_next){ int h,m; clk_next=t+100; if(!krtc(&h,&m)){ h=((h+tz)%24+24)%24; if(h!=clk_h||m!=clk_m){ clk_h=h; clk_m=m; dmark(0,0,gw,BARH); dmark(gw-240,BARH,240,230); } } }
    { int top=win_top(); static int ph; if(top>=0){ Win *tw=&wins[top];
        if(tw->type==A_TERM||tw->type==A_NOTES||(tw->type==A_BROWSER&&tw->editing)){ int p=(t/50)&1; if(p!=ph){ ph=p; dwin(tw); } } } }
    { static uint32_t nm; if(t>=nm){ nm=t+100; for(int i=0;i<nord;i++) if(wins[order[i]].type==A_MONITOR) dwin(&wins[order[i]]); } }
    snake_tick(t); kngfs_tick();
    if(dirty||dr1x>dr0x) render();
    else if(cursor_dirty){ cursor_restore(); cursor_draw(); cursor_dirty=0; }
}

/* ---------------- entrada ---------------- */
static int in_rect(int px,int py,int x,int y,int w,int h){ return px>=x&&px<x+w&&py>=y&&py<y+h; }
static int dock_hit(int px,int py){
    int bx,by,bw,bh; dock_geom(&bx,&by,&bw,&bh);
    if(!in_rect(px,py,bx,by-6,bw,bh+6)) return -1;
    for(int i=0;i<NDOCK;i++) if(in_rect(px,py,bx+14+i*(TILE+14)-7,by,TILE+14,bh)) return i;
    return -2;                                                       /* sobre el dock, pero entre iconos */
}
static void files_click(Win *w,int lx,int ly,int cw,int ch){
    static const int FX[4]={8,74,164,254}, FW[4]={60,84,84,76};
    (void)ch;
    if(ly>=6&&ly<30){
        for(int i=0;i<4;i++) if(lx>=FX[i]&&lx<FX[i]+FW[i]){
            if(i==0){ if(w->dir->parent) w->dir=w->dir->parent; w->sel=0; w->scroll=0; }
            else if(i==1){ char nm[32]; for(int k=1;k<100;k++){ nm[0]=0; cat(nm,32,"Carpeta"); if(k>1){ cat(nm,32," "); cat_int(nm,32,k); } if(!fs_find(w->dir,nm)) break; } fs_mkdir(w->dir,nm); }
            else if(i==2){ char nm[32]; for(int k=1;k<100;k++){ nm[0]=0; cat(nm,32,"nuevo"); if(k>1) cat_int(nm,32,k); cat(nm,32,".txt"); if(!fs_find(w->dir,nm)) break; } fs_create(w->dir,nm,"",0); }
            else if(w->sel){ fs_remove(w->sel); w->sel=0; }
            dirty=1; }
        return;
    }
    (void)cw;
    int row=(ly-68)/26; if(ly<68||row<0) return;
    FsNode *l[96]; int n=fs_list(w->dir,l,96); int idx=row+w->scroll;
    if(idx>=n) { w->sel=0; dirty=1; return; }
    FsNode *f=l[idx]; uint32_t t=kticks();
    if(w->sel==f && w->last==f && t-w->lastclick<40){
        if(f->dir){ w->dir=f; w->sel=0; w->scroll=0; } else open_file(f);
    } else { w->sel=f; }
    w->last=f; w->lastclick=t; dirty=1;
}
static void settings_click(int lx,int ly,int ch){
    for(int i=0;i<6;i++){ int dx=lx-(20+i*44+14), dy=ly-54; if(dx*dx+dy*dy<=18*18){ acc_i=i; dirty=1; return; } }
    for(int i=0;i<3;i++) if(in_rect(lx,ly,20+i*112,114,96,60)){ if(wall_i!=i){ wall_i=i; build_wall(); } dirty=1; return; }
    if(in_rect(lx,ly,20,218,28,24)){ if(tz>-12) tz--; clk_next=0; dirty=1; }
    else if(in_rect(lx,ly,56,218,28,24)){ if(tz<14) tz++; clk_next=0; dirty=1; }
    else if(in_rect(lx,ly,20,252,190,24)){ show24=!show24; dirty=1; }
    else if(in_rect(lx,ly,20,322,140,26)) set_layout(KBD_US);
    else if(in_rect(lx,ly,170,322,140,26)) set_layout(KBD_LATAM);
    else if(in_rect(lx,ly,320,322,140,26)) set_layout(KBD_ES);
    else if(in_rect(lx,ly,20,ch-44,110,28)) kreboot();
    else if(in_rect(lx,ly,142,ch-44,110,28)) khalt();
}
static void browser_click(Win *w,int lx,int ly,int cw,int ch){
    (void)ch;
    if(in_rect(lx,ly,8,6,32,24)){ go_back(w); return; }
    if(in_rect(lx,ly,48,6,cw-56,24)){ w->editing=1; w->selall=1; k_strcpy(w->ebuf,w->url,192); w->elen=k_strlen(w->ebuf); dirty=1; return; }
    w->editing=0;
    for(int i=0;i<w->page.nlinks;i++){
        HLink *k=&w->page.links[i]; int px=12+k->x, py=38+k->y-w->bscroll;
        if(ly>=38 && in_rect(lx,ly,px,py,k->w,k->h)){ const char *hr=w->page.hrefs[k->href]; if(hr[0]=='#') return; char u[192]; resolve(w->url,hr,u,sizeof u); nav(w,u,1); return; }
    }
    dirty=1;
}
static void app_click(Win *w,int lx,int ly){
    int cx,cy,cw,ch; crect(w,&cx,&cy,&cw,&ch);
    switch(w->type){
    case A_FILES: files_click(w,lx,ly,cw,ch); break;
    case A_SETTINGS: settings_click(lx,ly,ch); break;
    case A_BROWSER: browser_click(w,lx,ly,cw,ch); break;
    case A_NOTES:
        if(in_rect(lx,ly,8,6,70,24)){ notes_new(w); dirty=1; }
        else if(in_rect(lx,ly,86,6,80,24)){ notes_save(w); dirty=1; }
        break;
    case A_CALC:
        for(int i=0;i<NCKEYS;i++){ int x,y,kw,kh; ckey_rect(&CKEYS[i],&x,&y,&kw,&kh); if(in_rect(lx,ly,x,y,kw,kh)){ calc_press(w,CKEYS[i].k); break; } }
        break;
    case A_SNAKE:
        if(w->sover||!w->sstart) snake_key(w,' ');
        break;
    default: break;
    }
}
static void on_press(void){
    int d=dock_hit(mx,my);
    if(d>=0){ open_app(DOCK_APP[d]); dirty=1; return; }
    if(d==-2) return;
    if(my<BARH){ if(mx>gw-48) open_app(A_SETTINGS); return; }
    for(int k=nord-1;k>=0;k--){
        int wi=order[k]; Win *w=&wins[wi];
        if(!in_rect(mx,my,w->x,w->y,w->w,w->h)) continue;
        focus(wi);
        if(my<w->y+TB){
            int dx=mx-(w->x+w->w-20), dy=my-(w->y+TB/2);
            if(dx*dx+dy*dy<=100){ win_close(wi); return; }
            { uint32_t t=kticks(); int rs=win_resizable(w);
              if(rs && t-w->tclick<40){ w->tclick=0; win_toggle_max(w); return; }
              w->tclick=t;
              if(w->maxed){ win_restore(w); w->x=mx-w->w/2<0?0:mx-w->w/2; dirty=1; }
              drag=wi; dragdx=mx-w->x; dragdy=my-w->y; return; }
        }
        app_click(w,mx-(w->x+1),my-(w->y+TB));
        return;
    }
    { int di=dobj_hit(mx,my); if(di>=0){ ddrag=di; ddx=mx-dobj[di].x; ddy=my-dobj[di].y; dmoved=0; } }
}
static void mouse_set(int x,int y,int b){
    x=k_max(0,k_min(gw-1,x)); y=k_max(0,k_min(gh-1,y));
    int pressed=(b&1)&&!(mb&1), released=!(b&1)&&(mb&1);
    if(x!=mx||y!=my) cursor_dirty=1;
    mx=x; my=y; mb=b;
    int d=dock_hit(mx,my); if(d<0) d=-1; if(d!=hover){ hover=d; { int bx,by,bw,bh; dock_geom(&bx,&by,&bw,&bh); dmark(bx-10,by-40,bw+20,bh+52); } }
    if(drag>=0){
        if(b&1){ Win *w=&wins[drag]; dwin(w); w->x=mx-dragdx; w->y=k_max(BARH,my-dragdy); dwin(w); }
        if(released){ Win *w=&wins[drag]; drag=-1;
            if(win_resizable(w)){
                if(my<=BARH+3){ win_set_max(w,0,BARH,gw,gh-BARH-86); }
                else if(mx<=3){ win_set_max(w,0,BARH,gw/2,gh-BARH-86); }
                else if(mx>=gw-4){ win_set_max(w,gw/2,BARH,gw-gw/2,gh-BARH-86); } } }
    }
    if(ddrag>=0){
        DObj *o=&dobj[ddrag];
        if(b&1){ int nx=mx-ddx, ny=my-ddy; if(nx<0) nx=0; if(ny<BARH) ny=BARH; if(nx+o->w>gw) nx=gw-o->w; if(ny+o->h>gh-70) ny=gh-70-o->h;
                 if(nx!=o->x||ny!=o->y){ dmark(o->x-6,o->y-6,o->w+12,o->h+12); o->x=nx; o->y=ny; dmark(o->x-6,o->y-6,o->w+12,o->h+12); dmoved=1; } }
        if(released){ int i=ddrag; ddrag=-1;
            if(!dmoved && o->kind>=0 && o->kind<8){ uint32_t t=kticks(); if(dlast==i && t-dlastt<40){ open_app(DOCK_APP[o->kind]); dirty=1; dlast=-1; } else { dlast=i; dlastt=t; } } }
    }
    if(pressed) on_press();
}
void gui_mouse_abs(int x,int y,int b){ mouse_set(x,y,b); }
void gui_mouse_event(int dx,int dy,int b){ mouse_set(mx+dx,my+dy,b); }

static void scroll_by(int *s,int d,int max){ *s+=d; if(*s<0) *s=0; if(*s>max) *s=max; { int t=win_top(); if(t>=0) dwin(&wins[t]); else dirty=1; } }
void gui_key(int c){
    int top=win_top(); if(top<0) return;
    Win *w=&wins[top];
    switch(w->type){
    case A_TERM:
        if(w->busy){                                           /* hay un programa pegado a esta terminal: las teclas son suyas */
            if(c>=0x400||c<0x80) kproc_key(c);
            else { char e[2]; int en=u8_enc(c,e); for(int i=0;i<en;i++) kproc_key((unsigned char)e[i]); }
            break; }
        if(c>=0x400) break;
        if(c=='\n'){ term_put(w,'\n'); w->in[w->inlen]=0; char line[256]; memcpy(line,w->in,256); w->inlen=0;
            fs_path(w->cwd,kshell_cwd,sizeof kshell_cwd); kattach_pid=0;
            term_cmd(w,line);
            if(kattach_pid){ w->busy=1; w->bpid=kattach_pid; kattach_pid=0; } else term_prompt(w); }
        else if(c=='\b'){ if(w->inlen>0){ w->inlen-=u8_last(w->in,w->inlen); term_put(w,'\b'); } }
        else { char e[2]; int en=u8_enc(c,e); if(en && w->inlen+en<250){ for(int i=0;i<en;i++){ w->in[w->inlen++]=e[i]; term_put(w,e[i]); } } }
        break;
    case A_FILES: {
        FsNode *l[96]; int n=fs_list(w->dir,l,96), idx=-1;
        for(int i=0;i<n;i++) if(l[i]==w->sel) idx=i;
        if(c==KEY_DOWN){ if(idx<n-1) w->sel=l[idx+1]; dirty=1; }
        else if(c==KEY_UP){ if(idx>0) w->sel=l[idx-1]; else if(idx<0&&n) w->sel=l[0]; dirty=1; }
        else if(c=='\n' && w->sel){ if(w->sel->dir){ w->dir=w->sel; w->sel=0; w->scroll=0; } else open_file(w->sel); dirty=1; }
        else if(c=='\b'){ if(w->dir->parent){ w->dir=w->dir->parent; w->sel=0; w->scroll=0; dirty=1; } }
        for(int i=0;i<n;i++) if(l[i]==w->sel) idx=i;
        { int rows=(w->h-TB-1-68-22)/26; if(idx>=0){ if(idx<w->scroll) w->scroll=idx; if(idx>=w->scroll+rows) w->scroll=idx-rows+1; } }
        break; }
    case A_NOTES: notes_key(w,c); break;
    case A_CALC:
        if(c>='0'&&c<='9') calc_press(w,c);
        else if(c=='+'||c=='-'||c=='*'||c=='/') calc_press(w,c);
        else if(c=='x'||c=='X') calc_press(w,'*');
        else if(c=='='||c=='\n') calc_press(w,'=');
        else if(c=='c'||c=='C'||c==27) calc_press(w,'C');
        else if(c=='\b') calc_press(w,'<');
        break;
    case A_SNAKE: snake_key(w,c); break;
    case A_VIEWER:
        if(c==KEY_DOWN) scroll_by(&w->scroll,1,100000); else if(c==KEY_UP) scroll_by(&w->scroll,-1,100000);
        else if(c==KEY_PGDN) scroll_by(&w->scroll,20,100000); else if(c==KEY_PGUP) scroll_by(&w->scroll,-20,100000);
        break;
    case A_BROWSER:
        if(w->editing){
            if(c=='\n') browser_go_typed(w);
            else if(c==27){ w->editing=0; dwin(w); }
            else if(c=='\b'){ if(w->selall){ w->elen=0; w->selall=0; } else if(w->elen>0) w->elen-=u8_last(w->ebuf,w->elen); w->ebuf[w->elen]=0; dwin(w); }
            else { char e[2]; int en=u8_enc(c,e); if(en && w->elen+en<186){ if(w->selall){ w->elen=0; w->selall=0; } for(int i=0;i<en;i++) w->ebuf[w->elen++]=e[i]; w->ebuf[w->elen]=0; dwin(w); } }
        } else {
            if(c==KEY_DOWN) scroll_by(&w->bscroll,24,100000); else if(c==KEY_UP) scroll_by(&w->bscroll,-24,100000);
            else if(c==KEY_PGDN) scroll_by(&w->bscroll,300,100000); else if(c==KEY_PGUP) scroll_by(&w->bscroll,-300,100000);
            else if(c=='\b') go_back(w);
        }
        break;
    default: break;
    }
}

int gui_init(uint32_t *fb,int w,int h,int pitch_bytes){
    if(gfx_init(fb,w,h,pitch_bytes)) return -1;
    wall=kmalloc((size_t)w*h*4); if(!wall) return -1;
    if(!fs_root()){ fs_init(); }
    (void)kngfs_mount(); build_wall();
    { char kv[16]; if(cfg_get("teclado",kv,sizeof kv)){ int l=layout_from_name(kv); if(l>=0) kbd_set_layout(l); } }
    dobj_init(); mx=w/2; my=h/2; dirty=1; cursor_dirty=1; clk_next=0;
    kproc_done=term_done;
    return 0;
}

/* Pantalla de error fatal: se dibuja directo al backbuffer (sin reservar memoria) para que una excepcion del kernel
 * no deje la pantalla "congelada" sin explicacion. La llaman los manejadores de excepcion de los dos kernels. */
static void hex_into(char *o,int max,uint64_t v){ static const char H[]="0123456789abcdef"; int n=k_strlen(o); if(n+19>=max) return; o[n++]='0'; o[n++]='x'; int st=0;
    for(int s=60;s>=0;s-=4){ int d=(int)((v>>s)&15); if(d||st||s==0){ st=1; o[n++]=H[d]; } } o[n]=0; }
int gui_ready(void){ return bb!=0 && gw>0; }
void gui_panic(const char *name,const char *ipname,uint64_t ip,uint64_t err,uint64_t cr2,int has_cr2){
    if(!gui_ready()) return;
    clip_base(0,0,gw,gh); clip_reset();
    fill(0,0,gw,gh,0x2a0a0e,255);
    int x=gw/2-260, y=gh/2-130; if(x<20) x=20; if(y<20) y=20;
    crown(x,y,48,0xe9c46a,0xb5373c,255);
    text_big(x+64,y+6,"Error fatal del kernel",0xffffff);
    char b[120]; b[0]=0; cat(b,120,"Excepcion: "); cat(b,120,name); text_bold(x,y+72,b,0xffd6d6);
    b[0]=0; cat(b,120,ipname); cat(b,120," = "); hex_into(b,120,ip); text(x,y+104,b,0xffffff);
    b[0]=0; cat(b,120,"codigo de error = "); hex_into(b,120,err); text(x,y+126,b,0xffffff);
    if(has_cr2){ b[0]=0; cat(b,120,"direccion (cr2) = "); hex_into(b,120,cr2); text(x,y+148,b,0xffffff); }
    text(x,y+190,"El sistema se detuvo para no danar nada. Reinicia la maquina virtual.",0xe8b4b4);
    text(x,y+212,"Si se repite, copia estos datos (tambien salen por el puerto serie).",0xe8b4b4);
    gfx_present();
}
