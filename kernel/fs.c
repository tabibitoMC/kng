/* fs.c — sistema de archivos de kng: arbol de nodos en RAM + copia a disco (formato KNGFS, ver disk.c).
 * Operaciones: crear, escribir, borrar, renombrar, copiar, mover, serializar. Cada cambio sube fs_gen (lo usa el guardado automatico). */
#include "fs.h"
static FsNode *root;
static uint32_t gen;
uint32_t fs_gen(void){ return gen; }
FsNode *fs_root(void){ return root; }

static FsNode *newnode(FsNode *parent,const char *name,int dir){
    FsNode *n=kmalloc(sizeof *n); if(!n) return 0;
    memset(n,0,sizeof *n); k_strcpy(n->name,name,sizeof n->name); n->dir=dir; n->parent=parent;
    if(parent){ FsNode **pp=&parent->child; while(*pp) pp=&(*pp)->next; *pp=n; }
    return n;
}
FsNode *fs_find(FsNode *dir,const char *name){
    if(!dir||!dir->dir) return 0;
    for(FsNode *c=dir->child;c;c=c->next) if(!k_strcmp(c->name,name)) return c;
    return 0;
}
static int valid_name(const char *s){
    if(!*s || k_strlen(s)>=32) return 0;
    for(;*s;s++) if(*s=='/') return 0;
    return 1;
}
FsNode *fs_mkdir(FsNode *dir,const char *name){
    if(!dir||!dir->dir||!valid_name(name)||fs_find(dir,name)) return 0;
    gen++; return newnode(dir,name,1);
}
int fs_write(FsNode *n,const char *data,int len){
    if(!n||n->dir) return -1;
    char *d=kmalloc((size_t)len+1); if(!d) return -1;
    memcpy(d,data,(size_t)len); d[len]=0;
    kfree(n->data); n->data=d; n->size=len; gen++; return 0;
}
FsNode *fs_create(FsNode *dir,const char *name,const char *data,int len){
    if(!dir||!dir->dir||!valid_name(name)) return 0;
    FsNode *n=fs_find(dir,name);
    if(n&&n->dir) return 0;
    if(!n){ n=newnode(dir,name,0); if(!n) return 0; }
    if(fs_write(n,data,len)) return 0;
    return n;
}
int fs_remove(FsNode *n){
    if(!n||!n->parent) return -1;                      /* la raiz no se borra */
    while(n->child) fs_remove(n->child);
    FsNode **pp=&n->parent->child; while(*pp&&*pp!=n) pp=&(*pp)->next;
    if(*pp) *pp=n->next;
    kfree(n->data); kfree(n); gen++; return 0;
}

/* ---- renombrar, copiar y mover ---- */
int fs_rename(FsNode *n,const char *name){
    if(!n||!n->parent||!valid_name(name)) return -1;
    FsNode *e=fs_find(n->parent,name); if(e&&e!=n) return -1;
    k_strcpy(n->name,name,sizeof n->name); gen++; return 0;
}
static int is_inside(FsNode *n,FsNode *anc){ for(;n;n=n->parent) if(n==anc) return 1; return 0; }
static FsNode *copy_rec(FsNode *src,FsNode *dir,const char *name){
    if(!src->dir) return fs_create(dir,name,src->data?src->data:"",src->size);
    FsNode *d=fs_mkdir(dir,name); if(!d) return 0;
    for(FsNode *c=src->child;c;c=c->next) if(!copy_rec(c,d,c->name)) return 0;
    return d;
}
FsNode *fs_copy(FsNode *src,FsNode *dir,const char *name){        /* copia un archivo o una carpeta entera */
    if(!src||!dir||!dir->dir||!valid_name(name)||fs_find(dir,name)||is_inside(dir,src)) return 0;
    return copy_rec(src,dir,name);
}
int fs_move(FsNode *n,FsNode *dir,const char *name){
    if(!n||!n->parent||!dir||!dir->dir||!valid_name(name)||fs_find(dir,name)||is_inside(dir,n)) return -1;
    FsNode **pp=&n->parent->child; while(*pp&&*pp!=n) pp=&(*pp)->next; if(*pp) *pp=n->next;
    n->next=0; n->parent=dir; k_strcpy(n->name,name,sizeof n->name);
    pp=&dir->child; while(*pp) pp=&(*pp)->next; *pp=n; gen++; return 0;
}
static void usage_rec(FsNode *d,int *f,int *dr,int *b){ for(FsNode *c=d->child;c;c=c->next){ if(c->dir){ (*dr)++; usage_rec(c,f,dr,b); } else { (*f)++; *b+=c->size; } } }
void fs_usage(int *files,int *dirs,int *bytes){ *files=*dirs=*bytes=0; usage_rec(root,files,dirs,bytes); }

/* ---- serializacion (formato KNGFS): por cada nodo: tipo(1=archivo,2=carpeta,0=fin de carpeta) largo-nombre nombre [tamano(4) datos] ---- */
static int ser_rec(FsNode *d,uint8_t *o,int max,int k){
    for(FsNode *c=d->child;c;c=c->next){
        if(c->sys) continue;                       /* archivos del sistema Linux (rootfs): se recargan al arrancar, no se guardan en el disco */
        int nl=k_strlen(c->name);
        if(k+2+nl+(c->dir?0:4+c->size)+1>max) return -1;
        o[k++]=c->dir?2:1; o[k++]=(uint8_t)nl; memcpy(o+k,c->name,(size_t)nl); k+=nl;
        if(c->dir){ k=ser_rec(c,o,max,k); if(k<0) return -1; }
        else { o[k++]=(uint8_t)c->size; o[k++]=(uint8_t)(c->size>>8); o[k++]=(uint8_t)(c->size>>16); o[k++]=(uint8_t)(c->size>>24); if(c->size) memcpy(o+k,c->data,(size_t)c->size); k+=c->size; }
    }
    if(k+1>max) return -1;
    o[k++]=0; return k;
}
int fs_serialize(uint8_t *out,int max){ return ser_rec(root,out,max,0); }
static int de_rec(FsNode *d,const uint8_t *in,int len,int k){
    for(;;){
        if(k>=len) return -1;
        int t=in[k++]; if(t==0) return k;
        if(k>=len) return -1;
        int nl=in[k++]; if(k+nl>len||nl>31||nl==0) return -1;
        char nm[32]; memcpy(nm,in+k,(size_t)nl); nm[nl]=0; k+=nl;
        if(t==2){ FsNode *c=newnode(d,nm,1); if(!c) return -1; k=de_rec(c,in,len,k); if(k<0) return -1; }
        else if(t==1){
            if(k+4>len) return -1;
            int sz=in[k]|(in[k+1]<<8)|(in[k+2]<<16)|(in[k+3]<<24); k+=4;
            if(sz<0||k+sz>len) return -1;
            FsNode *c=newnode(d,nm,0); if(!c||fs_write(c,(const char*)in+k,sz)) return -1; k+=sz;
        } else return -1;
    }
}
int fs_deserialize(const uint8_t *in,int len){          /* reemplaza TODO el arbol; solo antes de abrir ventanas */
    FsNode *old=root; root=newnode(0,"",1); if(!root){ root=old; return -1; }
    if(de_rec(root,in,len,0)<0){ while(root->child) fs_remove(root->child); kfree(root); root=old; return -1; }
    while(old->child) fs_remove(old->child);
    kfree(old); gen++; return 0;
}

FsNode *fs_resolve(FsNode *cwd,const char *path){
    FsNode *cur=(path[0]=='/')?root:cwd;
    char part[32];
    while(*path){
        while(*path=='/') path++;
        if(!*path) break;
        int n=0; while(*path&&*path!='/'){ if(n<31) part[n++]=*path; path++; } part[n]=0;
        if(!k_strcmp(part,".")) continue;
        if(!k_strcmp(part,"..")){ if(cur->parent) cur=cur->parent; continue; }
        cur=fs_find(cur,part); if(!cur) return 0;
    }
    return cur;
}
void fs_path(FsNode *n,char *out,int max){
    if(!n->parent){ k_strcpy(out,"/",max); return; }
    char tmp[128]; fs_path(n->parent,tmp,sizeof tmp);
    int l=k_strlen(tmp); if(tmp[l-1]!='/'&&l<126){ tmp[l++]='/'; tmp[l]=0; }
    int i=0; for(;tmp[i]&&i<max-1;i++) out[i]=tmp[i];
    for(const char *s=n->name;*s&&i<max-1;s++) out[i++]=*s;
    out[i]=0;
}
int fs_list(FsNode *dir,FsNode **out,int max){
    int n=0;
    for(int pass=0;pass<2;pass++)
        for(FsNode *c=dir->child;c&&n<max;c=c->next) if(c->dir==(pass==0)) out[n++]=c;
    /* ordenar por nombre dentro de cada grupo (insercion) */
    for(int i=1;i<n;i++){ FsNode *v=out[i]; int j=i-1;
        while(j>=0 && out[j]->dir==v->dir && k_strcmp(out[j]->name,v->name)>0){ out[j+1]=out[j]; j--; }
        out[j+1]=v; }
    return n;
}
static void put(FsNode *d,const char *name,const char *txt){ fs_create(d,name,txt,k_strlen(txt)); }
void fs_init(void){
    root=newnode(0,"",1);
    FsNode *esc=fs_mkdir(root,"Escritorio"), *doc=fs_mkdir(root,"Documentos"), *img=fs_mkdir(root,"Imagenes"),
           *sys=fs_mkdir(root,"Sistema"), *prog=fs_mkdir(root,"Programas");
    (void)esc; (void)img;
    put(doc,"bienvenida.txt","Bienvenido a Königreich OS\n\nEste es un sistema operativo propio, con kernel propio (kng-kernel).\nNo es Linux ni usa codigo de Linux.\n\nPuedes explorar los archivos, abrir la terminal,\ncambiar colores y fondos en Ajustes, y probar el navegador.\n\nLos archivos viven en memoria: al reiniciar se pierden\n(todavia no hay controlador de disco).\n");
    put(doc,"notas.txt","Notas\n-----\n- Crear controlador de disco\n- Pasar el kernel a 64 bits\n- Red (tarjeta virtual) para el navegador\n");
    put(doc,"pagina.html","<title>Mi pagina</title><h1>Hola, Königreich</h1><p>Esta pagina es un archivo del sistema, abierto con el navegador de kng.</p><p><a href=\"kng://inicio\">Volver al inicio</a></p>");
    put(sys,"version.txt","Königreich OS\nkernel: kng-kernel 0.5\narquitectura: i386 (x86_64 en desarrollo)\n");
    put(sys,"kng.cfg","# configuracion de ejemplo\nidioma=es\nteclado=us\n# buscador web: kng-proxy (tools/kng-proxy.py en tu PC, host = 10.0.2.2 en NAT). Borra la linea para buscar solo en kng:\nbuscador=http://10.0.2.2:8080/search?q=%s\n");
    put(prog,"terminal","aplicacion integrada\n");
    put(prog,"archivos","aplicacion integrada\n");
    put(prog,"navegador","aplicacion integrada\n");
    put(prog,"ajustes","aplicacion integrada\n");
    put(prog,"notas","aplicacion integrada\n");
    put(prog,"calculadora","aplicacion integrada\n");
    put(prog,"snake","aplicacion integrada\n");
}
