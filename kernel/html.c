/* html.c — motor minimo de paginas: parsea y dibuja un subconjunto de HTML con ajuste de linea. */
#include "html.h"
#include "gfx.h"

#define COL_TXT  0xdfe3f0
#define COL_DIM  0x9aa0b8
#define COL_H    0xffffff

typedef struct { int cx, cy, lh, left, right; } Lay;           /* cursor de maquetacion */

static const char P_INICIO[]=
 "<title>Inicio</title><h1>Königreich OS</h1>"
 "<p>Bienvenido al navegador de kng. Abre paginas <b>http://</b> (con red), paginas internas <b>kng://</b> y archivos del sistema.</p><hr>"
 "<h2>Enlaces</h2><ul>"
 "<li><a href=\"kng://ayuda\">Ayuda</a></li>"
 "<li><a href=\"kng://acerca\">Acerca de Königreich OS</a></li>"
 "<li><a href=\"file:///Documentos/pagina.html\">Mi pagina de prueba (archivo del sistema)</a></li>"
 "<li><a href=\"file:///Documentos/bienvenida.txt\">Texto de bienvenida</a></li>"
 "<li><a href=\"http://example.com/\">example.com (necesita red)</a></li>"
 "<li><a href=\"http://neverssl.com/\">neverssl.com (necesita red)</a></li></ul>"
 "<p><b>Buscar:</b> escribe palabras sueltas arriba. Con <b>tools/kng-proxy.py</b> corriendo en tu PC busca en la web (DuckDuckGo); si no, busca en los archivos de kng. Tambien: <b>kng://ayuda</b>, <b>file:///Sistema/version.txt</b> o <b>example.com</b>.</p>";
static const char P_AYUDA[]=
 "<title>Ayuda</title><h1>Ayuda</h1>"
 "<h2>Navegador</h2><p>Direcciones validas: http://sitio/ruta, kng://pagina y file:///ruta/al/archivo. Un nombre suelto como example.com se abre por http. <b>https:// todavia no</b>: kng no tiene TLS, asi que muchos sitios modernos no cargaran; sirven los que aun ofrecen http.</p>"
 "<h2>Buscador</h2><p>Si escribes palabras sin punto (o con espacios) se usa el buscador de /Sistema/kng.cfg (linea buscador=http://10.0.2.2:8080/search?q=%s, que es kng-proxy en tu PC: hace el https por kng y permite leer casi cualquier sitio con /go?u=URL). Si quitas esa linea, la busqueda es local en paginas y archivos.</p>"
 "<p>Los archivos .html se muestran como pagina; el resto como texto. El motor entiende un subconjunto de HTML (sin CSS ni JavaScript ni imagenes).</p>"
 "<p>Teclas: flechas arriba/abajo y Re Pag/Av Pag para desplazar. El boton con la flecha vuelve a la pagina anterior.</p>"
 "<h2>Terminal</h2><p>Comandos: help, ls, cd, pwd, cat, mkdir, touch, rm, write, clear, info, mem, uptime.</p>"
 "<p>Red: <b>red</b> (estado), <b>ping host</b>, <b>dns host</b>, <b>wget http://...</b></p>"
 "<hr><p><a href=\"kng://inicio\">Volver al inicio</a></p>";
static const char P_ACERCA[]=
 "<title>Acerca de</title><h1>Königreich OS</h1>"
 "<p>Sistema operativo propio con kernel propio (kng-kernel). No es Linux ni usa su codigo.</p>"
 "<ul><li>Dos kernels propios: 32 y 64 bits</li><li>Interfaz grafica por software sobre framebuffer lineal</li><li>Gestor de archivos, terminal, navegador y ajustes</li>"
 "<li>Red: Intel e1000 y RTL8139, DHCP, DNS, TCP, HTTP</li><li>Teclado y raton PS/2</li></ul>"
 "<p>Autor: Nick Jose Tolentino Rojas</p><hr><p><a href=\"kng://inicio\">Volver al inicio</a></p>";
static const char P_ERROR[]=
 "<title>Error</title><h1>No se pudo abrir</h1><p>La direccion no existe o no esta soportada.</p>"
 "<p><a href=\"kng://inicio\">Volver al inicio</a></p>";

const char *html_builtin(const char *url,int *len){
    const char *p=0; int n=0;
    if(!k_strcmp(url,"kng://inicio"))      { p=P_INICIO; n=sizeof P_INICIO-1; }
    else if(!k_strcmp(url,"kng://ayuda"))  { p=P_AYUDA;  n=sizeof P_AYUDA-1; }
    else if(!k_strcmp(url,"kng://acerca")) { p=P_ACERCA; n=sizeof P_ACERCA-1; }
    else if(!k_strcmp(url,"kng://error"))  { p=P_ERROR;  n=sizeof P_ERROR-1; }
    if(p) *len=n;
    return p;
}

static int memcmp_(const char *a,const char *b,int n){ for(int i=0;i<n;i++) if(a[i]!=b[i]) return 1; return 0; }
static int ci_pre(const char *s,const char *p){ while(*p){ char c=*s++; if(c>='A'&&c<='Z') c+=32; if(c!=*p++) return 0; } return 1; }
static int skip_close(const char *h,int len,int i,const char *name){        /* salta hasta despues de </name> */
    while(i<len){ if(h[i]=='<' && i+1<len && h[i+1]=='/' && ci_pre(h+i+2,name)){ while(i<len && h[i]!='>') i++; return i<len?i+1:len; } i++; }
    return len;
}
static int put_utf8(char *o,unsigned c){
    if(c<0x80){ o[0]=(char)c; return 1; }
    if(c<0x800){ o[0]=(char)(0xC0|(c>>6)); o[1]=(char)(0x80|(c&63)); return 2; }
    o[0]='?'; return 1;
}
/* Entidad HTML en s (empieza en '&'): escribe su texto en out y devuelve cuantos caracteres consumio (0 si no es entidad). */
static int entity(const char *s,int avail,char *out,int *olen){
    static const struct { const char *n; unsigned c; } T[]={{"lt",'<'},{"gt",'>'},{"amp",'&'},{"quot",'"'},{"apos",'\''},{"nbsp",' '},
        {"copy",0xA9},{"reg",0xAE},{"deg",0xB0},{"middot",0xB7},{"laquo",0xAB},{"raquo",0xBB},{"mdash",'-'},{"ndash",'-'},{"hellip",'.'},
        {"bull",'*'},{"euro",'E'},{"aacute",0xE1},{"eacute",0xE9},{"iacute",0xED},{"oacute",0xF3},{"uacute",0xFA},{"ntilde",0xF1},
        {"Aacute",0xC1},{"Eacute",0xC9},{"Iacute",0xCD},{"Oacute",0xD3},{"Uacute",0xDA},{"Ntilde",0xD1},{"uuml",0xFC},{"ouml",0xF6},{"auml",0xE4},{"szlig",0xDF},
        {"iexcl",0xA1},{"iquest",0xBF},{"times",0xD7},{"rsquo",'\''},{"lsquo",'\''},{"rdquo",'"'},{"ldquo",'"'}};
    int n=0; while(n<avail && n<10 && s[n]!=';' && s[n]!=' ' && s[n]!='<') n++;
    if(n>=avail || s[n]!=';' || n<2) return 0;
    unsigned c=0; int ok=0;
    if(s[1]=='#'){
        if(s[2]=='x'||s[2]=='X'){ for(int i=3;i<n;i++){ char d=s[i]; int v=d>='0'&&d<='9'?d-'0':(d>='a'&&d<='f'?d-'a'+10:(d>='A'&&d<='F'?d-'A'+10:-1)); if(v<0){ ok=0; break; } c=c*16+(unsigned)v; ok=1; } }
        else { for(int i=2;i<n;i++){ if(s[i]<'0'||s[i]>'9'){ ok=0; break; } c=c*10+(unsigned)(s[i]-'0'); ok=1; } }
        if(ok && (c==0x2014||c==0x2013||c==0x2212)) c='-';
        if(ok && (c==0x2018||c==0x2019)) c='\'';
        if(ok && (c==0x201C||c==0x201D)) c='"';
        if(ok && c==0x2026) c='.';
        if(ok && c==0x2022) c='*';
    } else {
        for(unsigned k=0;k<sizeof T/sizeof T[0];k++){ int l=k_strlen(T[k].n); if(l==n-1 && !memcmp_(s+1,T[k].n,l)){ c=T[k].c; ok=1; break; } }
    }
    if(!ok) return 0;
    if(c<32) c=' ';
    *olen=put_utf8(out,c);
    return n+1;
}
static int is_sp(char c){ return c==' '||c=='\n'||c=='\t'||c=='\r'; }
static int tag_is(const char *t,int n,const char *name){      /* compara nombre de etiqueta sin distinguir mayusculas */
    int i=0; for(;name[i];i++){ char c=t[i]; if(i>=n) return 0; if(c>='A'&&c<='Z') c+=32; if(c!=name[i]) return 0; }
    return i==n || is_sp(t[i]) || t[i]=='/';
}
static int get_attr(const char *t,int n,const char *attr,char *out,int max){
    int al=k_strlen(attr);
    for(int i=0;i+al+1<n;i++){
        if(t[i]==attr[0] && k_startswith(t+i,attr) && t[i+al]=='='){
            int j=i+al+1; char q=0; if(t[j]=='"'||t[j]=='\''){ q=t[j]; j++; }
            int k=0; while(j<n && (q? t[j]!=q : !is_sp(t[j])) && k<max-1) out[k++]=t[j++];
            out[k]=0; return 1;
        }
    }
    return 0;
}

void html_render(const char *html,int len,int x,int y,int w,int h,int scroll,uint32_t link_col,int draw,HPage *out){
    Lay L={0,0,16,0,w-14};
    int bold=0, big=0, h2=0, inlink=-1, pre=0, listd=0, pending_nl=0;
    int i=0;
    out->nlinks=0; out->nhrefs=0; out->title[0]=0;
    if(draw){ clip_set(x,y,w,h); }
    #define NEWLINE() do{ L.cx=L.left; L.cy+=L.lh; pending_nl=0; }while(0)
    while(i<len){
        char c=html[i];
        if(c=='<' && i+3<len && html[i+1]=='!' && html[i+2]=='-' && html[i+3]=='-'){          /* comentario */
            i+=4; while(i+2<len && !(html[i]=='-'&&html[i+1]=='-'&&html[i+2]=='>')) i++;
            i=i+3<=len?i+3:len; continue;
        }
        if(c=='<'){                                            /* etiqueta */
            int j=i+1; while(j<len && html[j]!='>') j++;
            const char *t=html+i+1; int n=j-i-1; i=j+1;
            int close=(n>0&&t[0]=='/'); if(close){ t++; n--; }
            if(!close && (tag_is(t,n,"script")||tag_is(t,n,"style")||tag_is(t,n,"noscript")||tag_is(t,n,"svg")||tag_is(t,n,"template"))){
                char nm[10]; int q=0; while(q<9 && t[q] && t[q]!=' ' && t[q]!='>' && q<n){ nm[q]=(char)(t[q]>='A'&&t[q]<='Z'?t[q]+32:t[q]); q++; } nm[q]=0;
                i=skip_close(html,len,i,nm); continue;
            }
            if(tag_is(t,n,"title")){                           /* texto del titulo: se guarda, no se dibuja */
                if(!close){ int k=0; while(i<len && html[i]!='<' && k<47){ out->title[k++]=html[i++]; } out->title[k]=0; }
            } else if(tag_is(t,n,"h1")){
                if(!close){ if(L.cx>L.left) NEWLINE(); L.cy+=10; big=1; L.lh=28; }
                else { big=0; NEWLINE(); L.lh=16; L.cy+=6; }
            } else if(tag_is(t,n,"h3")||tag_is(t,n,"h4")||tag_is(t,n,"h5")||tag_is(t,n,"h6")){
                if(!close){ if(L.cx>L.left) NEWLINE(); L.cy+=6; bold=1; } else { bold=0; NEWLINE(); L.cy+=2; }
            } else if(tag_is(t,n,"div")||tag_is(t,n,"section")||tag_is(t,n,"article")||tag_is(t,n,"header")||tag_is(t,n,"footer")||tag_is(t,n,"nav")||tag_is(t,n,"main")||
                      tag_is(t,n,"table")||tag_is(t,n,"tr")||tag_is(t,n,"form")||tag_is(t,n,"blockquote")||tag_is(t,n,"dl")||tag_is(t,n,"dt")||tag_is(t,n,"dd")||tag_is(t,n,"center")||tag_is(t,n,"address")){
                if(L.cx>L.left) NEWLINE();
            } else if(tag_is(t,n,"td")||tag_is(t,n,"th")){ if(close) pending_nl=1;
            } else if(tag_is(t,n,"strong")||tag_is(t,n,"em")||tag_is(t,n,"i")||tag_is(t,n,"th")){ bold=!close;
            } else if(tag_is(t,n,"img")){
                char alt[40]; if(!close && get_attr(t,n,"alt",alt,sizeof alt) && alt[0]){ char lbl[64]="[imagen: "; k_strcpy(lbl+9,alt,44); int ll=k_strlen(lbl); lbl[ll]=']'; lbl[ll+1]=0;
                    int ww=text_w(lbl); if(L.cx+ww>L.right && L.cx>L.left) NEWLINE(); if(draw) text(x+L.cx,y+L.cy-scroll,lbl,COL_DIM); L.cx+=ww+8; }
            } else if(tag_is(t,n,"h2")){
                if(!close){ if(L.cx>L.left) NEWLINE(); L.cy+=8; h2=1; bold=1; }
                else { h2=0; bold=0; NEWLINE(); L.cy+=2; }
            } else if(tag_is(t,n,"p")){
                if(L.cx>L.left) NEWLINE();
                if(!close) L.cy+=4; else L.cy+=6;
            } else if(tag_is(t,n,"br")){ NEWLINE(); }
            else if(tag_is(t,n,"b")){ bold=!close; }
            else if(tag_is(t,n,"pre")){ if(!close){ if(L.cx>L.left) NEWLINE(); pre=1; } else { pre=0; NEWLINE(); } }
            else if(tag_is(t,n,"ul")||tag_is(t,n,"ol")){ if(L.cx>L.left) NEWLINE(); listd=!close; L.left=close?0:22; L.cx=L.left; }
            else if(tag_is(t,n,"li")){
                if(!close){ if(L.cx>L.left) NEWLINE(); if(draw) circle(x+8,y+L.cy-scroll+8,3,COL_DIM,255); L.cx=L.left; }
                else NEWLINE();
            } else if(tag_is(t,n,"hr")){
                if(L.cx>L.left){ NEWLINE(); } L.cy+=6;
                if(draw) fill(x,y+L.cy-scroll,w-14,1,0x3a3d55,255);
                L.cy+=8;
            } else if(tag_is(t,n,"a")){
                if(!close){ char hr[192]; inlink=-1;
                    if(get_attr(t,n,"href",hr,sizeof hr) && out->nhrefs<64){
                        k_strcpy(out->hrefs[out->nhrefs],hr,192); inlink=out->nhrefs++; }
                } else inlink=-1;
            }
            continue;
        }
        if(pre){                                               /* texto preformateado: respeta saltos de linea */
            if(c=='\n'){ NEWLINE(); i++; continue; }
            char s[4]={c,0}; int adv=1, ol=0;
            if(c=='&'){ char eo[4]; int a=entity(html+i,len-i,eo,&ol); if(a){ for(int q=0;q<ol;q++) s[q]=eo[q]; s[ol]=0; adv=a; } }
            if(c=='\t'){ L.cx+=32; i++; continue; }
            if(L.cx+8>L.right){ NEWLINE(); }
            if(draw) text(x+L.cx,y+L.cy-scroll,s,COL_TXT);
            L.cx+=8; i+=adv; continue;
        }
        if(is_sp(c)){ i++; pending_nl=1; continue; }           /* los espacios se colapsan */
        /* palabra */
        char word[64]; int wn=0;
        while(i<len && !is_sp(html[i]) && html[i]!='<' && wn<62){
            if(html[i]=='&'){ char eo[4]; int ol=0; int a2=entity(html+i,len-i,eo,&ol); if(a2 && wn+ol<62){ for(int q=0;q<ol;q++) word[wn++]=eo[q]; i+=a2; continue; } }
            word[wn++]=html[i++];
        }
        word[wn]=0;
        int cw=big?12:8, ww=text_w(word); if(big) ww=ww/8*12;
        int space=(pending_nl && L.cx>L.left)?cw:0;
        if(L.cx+space+ww>L.right && L.cx>L.left){ NEWLINE(); space=0; }
        L.cx+=space; pending_nl=0;
        uint32_t col=big?COL_H:(h2?COL_H:(inlink>=0?link_col:COL_TXT));
        if(draw){
            int px=x+L.cx, py=y+L.cy-scroll;
            if(big) text_big(px,py,word,col);
            else if(bold) text_bold(px,py,word,col);
            else text(px,py,word,col);
            if(inlink>=0) fill(px,py+15,ww,1,col,255);
        }
        if(inlink>=0 && out->nlinks<HTML_MAXLINKS){
            HLink *lk=&out->links[out->nlinks++]; lk->x=L.cx-space; lk->y=L.cy; lk->w=ww+space; lk->h=L.lh; lk->href=inlink; }
        L.cx+=ww;
    }
    (void)listd;
    out->content_h=L.cy+L.lh+10;
    if(draw) clip_reset();
}
