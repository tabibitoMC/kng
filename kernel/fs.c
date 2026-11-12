/* fs.c — sistema de archivos de kng: arbol de nodos en RAM + copia a disco (formato KNGFS, ver disk.c).
 * Operaciones: crear, escribir, borrar, renombrar, copiar, mover, serializar. Cada cambio sube fs_gen (lo usa el guardado automatico). */
#include "fs.h"
static FsNode *root;
static uint32_t gen;
void (*fs_shm_hook)(void *shm);
uint32_t (*fs_now)(void);
int fs_track;
static void touch(FsNode *n){ if(fs_now) n->mtime=fs_now(); if(fs_track) n->chg=1; }
static void chg_rec(FsNode *n){ if(!fs_track) return; n->chg=1; for(FsNode *c=n->child;c;c=c->next) chg_rec(c); }
static void drop_data(FsNode *n);
/* enlaces duros: los nombres de un mismo archivo forman un anillo y despues de cada cambio los demas reciben una copia del contenido */
static void hl_sync(FsNode *n){
    if(!n->hl) return;
    for(FsNode *m=n->hl;m&&m!=n;m=m->hl){
        if(m->shm||m->dir) continue;
        char *d=kmalloc((size_t)n->size+1); if(!d) continue;
        if(n->size) memcpy(d,n->data,(size_t)n->size);
        d[n->size]=0; drop_data(m); m->data=d; m->size=n->size; m->cap=n->size+1; m->mtime=n->mtime;
    }
}
static void hl_leave(FsNode *n){
    if(!n->hl) return;
    FsNode *p=n->hl; while(p->hl!=n) p=p->hl;
    p->hl=n->hl; n->hl=0;
    if(p->hl==p) p->hl=0;                                             /* solo queda un nombre */
}
int fs_nlink(FsNode *n){ int c=1; if(n&&n->hl) for(FsNode *m=n->hl;m!=n;m=m->hl) c++; return c; }
FsNode *fs_link_id(FsNode *n){ FsNode *b=n; if(n&&n->hl) for(FsNode *m=n->hl;m!=n;m=m->hl) if(m<b) b=m; return b; }
uint32_t fs_gen(void){ return gen; }
FsNode *fs_root(void){ return root; }

static FsNode *newnode(FsNode *parent,const char *name,int dir){
    FsNode *n=kmalloc(sizeof *n); if(!n) return 0;
    memset(n,0,sizeof *n); k_strcpy(n->name,name,sizeof n->name); n->dir=dir; n->parent=parent; touch(n);
    if(parent){ FsNode **pp=&parent->child; while(*pp) pp=&(*pp)->next; *pp=n; }
    return n;
}
FsNode *fs_find(FsNode *dir,const char *name){
    if(!dir||!dir->dir) return 0;
    for(FsNode *c=dir->child;c;c=c->next) if(!k_strcmp(c->name,name)) return c;
    return 0;
}
static int valid_name(const char *s){
    if(!*s || k_strlen(s)>=FS_NAME) return 0;
    for(;*s;s++) if(*s=='/') return 0;
    return 1;
}
FsNode *fs_mkdir(FsNode *dir,const char *name){
    if(!dir||!dir->dir||!valid_name(name)||fs_find(dir,name)) return 0;
    gen++; return newnode(dir,name,1);
}
static void drop_data(FsNode *n){
    if(n->shm){ if(fs_shm_hook) fs_shm_hook(n->shm); n->shm=0; }
    else if(!n->ro && n->data) kfree(n->data);
    n->data=0; n->size=0; n->cap=0; n->ro=0;
}
int fs_write(FsNode *n,const char *data,int len){
    if(!n||n->dir||len<0) return -1;
    char *d=kmalloc((size_t)len+1); if(!d) return -1;
    memcpy(d,data,(size_t)len); d[len]=0;
    drop_data(n); n->data=d; n->size=len; n->cap=len+1; touch(n); hl_sync(n); gen++; return 0;
}
void fs_adopt(FsNode *n,const char *data,int len){ drop_data(n); n->data=(char*)data; n->size=len; n->cap=len; n->ro=1; gen++; }
static int ensure_cap(FsNode *n,int need){        /* 'need' bytes de contenido (+1 para el cero final); copia al escribir si no es nuestro */
    if(n->shm){ if(need<=n->cap) return 0; }                   /* en un bloque compartido solo se escribe hasta su tamano */
    else if(!n->ro && need+1<=n->cap) return 0;
    int nc=need+1; if(!n->ro&&!n->shm&&n->cap>0&&nc<n->cap*2) nc=(n->cap>(64<<20))?nc+nc/4:n->cap*2; if(nc<64) nc=64;   /* archivos enormes (navegadores): crecer un 25 % y no el doble, para no necesitar dos bloques gigantes a la vez */
    char *d=kmalloc((size_t)nc); if(!d) return -1;
    if(n->size) memcpy(d,n->data,(size_t)(n->size<nc?n->size:nc));
    memset(d+n->size,0,(size_t)(nc-n->size));
    int osize=n->size; drop_data(n); n->data=d; n->size=osize; n->cap=nc; return 0;
}
int fs_write_at(FsNode *n,int off,const char *src,int len){
    if(!n||n->dir||off<0||len<0) return -1;
    int end=off+len; if(end<off) return -1;
    if(ensure_cap(n,end)) return -1;
    if(off>n->size) memset(n->data+n->size,0,(size_t)(off-n->size));
    if(len) memcpy(n->data+off,src,(size_t)len);
    if(end>n->size){ n->size=end; if(!n->shm||end<n->cap) n->data[end]=0; }
    touch(n); hl_sync(n); gen++; return 0;
}
int fs_resize(FsNode *n,int size){
    if(!n||n->dir||size<0) return -1;
    if(size==n->size) return 0;
    if(size<n->size){ if(n->shm||n->ro){ if(n->ro&&!n->shm){ if(ensure_cap(n,n->size)) return -1; } } if(size<n->size) memset(n->data+size,0,(size_t)(n->size-size)); n->size=size; if(!n->shm||size<n->cap) n->data[size]=0; touch(n); hl_sync(n); gen++; return 0; }
    if(ensure_cap(n,size)) return -1;
    memset(n->data+n->size,0,(size_t)(size-n->size)); n->size=size; if(!n->shm||size<n->cap) n->data[size]=0; touch(n); hl_sync(n); gen++; return 0;
}
FsNode *fs_symlink(FsNode *dir,const char *name,const char *target){
    if(!dir||!dir->dir||!valid_name(name)||fs_find(dir,name)) return 0;
    int l=k_strlen(target); char *t=kmalloc((size_t)l+1); if(!t) return 0; memcpy(t,target,(size_t)l+1);
    FsNode *n=newnode(dir,name,0); if(!n){ kfree(t); return 0; }
    n->link=t; gen++; return n;
}
FsNode *fs_create(FsNode *dir,const char *name,const char *data,int len){
    if(!dir||!dir->dir||!valid_name(name)) return 0;
    FsNode *n=fs_find(dir,name);
    if(n&&n->dir) return 0;
    if(!n){ n=newnode(dir,name,0); if(!n) return 0; }
    if(fs_write(n,data,len)) return 0;
    return n;
}
static void destroy(FsNode *n){ drop_data(n); if(n->link) kfree(n->link); kfree(n); }
void fs_node_ref(FsNode *n){ if(n) n->opens++; }
void fs_node_unref(FsNode *n){ if(n&&--n->opens<=0&&n->dead) destroy(n); }
int fs_remove(FsNode *n){
    if(!n||!n->parent) return -1;                      /* la raiz no se borra */
    while(n->child) fs_remove(n->child);
    hl_leave(n);
    FsNode **pp=&n->parent->child; while(*pp&&*pp!=n) pp=&(*pp)->next;
    if(*pp) *pp=n->next;
    gen++;
    if(n->opens>0){ n->dead=1; n->parent=0; n->next=0; return 0; }      /* sigue abierto: se libera cuando se cierre el ultimo descriptor */
    destroy(n); return 0;
}

FsNode *fs_link(FsNode *src,FsNode *dir,const char *name){
    if(!src||src->dir||src->link||!dir||!dir->dir||!valid_name(name)||fs_find(dir,name)) return 0;
    FsNode *c=newnode(dir,name,0); if(!c) return 0;
    c->sys=src->sys; c->dev=src->dev;
    if(!src->hl) src->hl=src;
    c->hl=src->hl; src->hl=c;
    if(!src->dev){ hl_sync(src); c->mtime=src->mtime; }
    gen++; return c;
}

/* ---- renombrar, copiar y mover ---- */
int fs_rename(FsNode *n,const char *name){
    if(!n||!n->parent||!valid_name(name)) return -1;
    FsNode *e=fs_find(n->parent,name); if(e&&e!=n) return -1;
    k_strcpy(n->name,name,sizeof n->name); chg_rec(n); gen++; return 0;
}
static int is_inside(FsNode *n,FsNode *anc){ for(;n;n=n->parent) if(n==anc) return 1; return 0; }
static FsNode *copy_rec(FsNode *src,FsNode *dir,const char *name){
    if(src->link) return fs_symlink(dir,name,src->link);
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
    pp=&dir->child; while(*pp) pp=&(*pp)->next; *pp=n; chg_rec(n); gen++; return 0;
}
static void usage_rec(FsNode *d,int *f,int *dr,int *b){ for(FsNode *c=d->child;c;c=c->next){ if(c->dir){ (*dr)++; usage_rec(c,f,dr,b); } else { (*f)++; *b+=c->size; } } }
void fs_usage(int *files,int *dirs,int *bytes){ *files=*dirs=*bytes=0; usage_rec(root,files,dirs,bytes); }

/* ---- serializacion (formato KNGFS): por cada nodo: tipo(1=archivo,2=carpeta,0=fin de carpeta) largo-nombre nombre [tamano(4) datos] ---- */
/* Se guarda: lo que no es del sistema y lo que cambio desde el arranque (aunque sea del sistema: lo instalado con apt).
 * Las carpetas del sistema sin nada guardable dentro no se escriben. Con out=0 solo se cuenta el tamano. */
static void sput(uint8_t *o,int k,const void *s,int n){ if(o) memcpy(o+k,s,(size_t)n); }
static void sput1(uint8_t *o,int k,int v){ if(o) o[k]=(uint8_t)v; }
static int ser_rec(FsNode *d,uint8_t *o,int max,int k,int *any){
    for(FsNode *c=d->child;c;c=c->next){
        if(c->dev) continue;
        int keep=!c->sys||c->chg, nl=k_strlen(c->name);
        if(c->link){ if(!keep) continue;
            int tl=k_strlen(c->link); if(o&&k+2+nl+2+tl+1>max) return -1;
            sput1(o,k++,3); sput1(o,k++,nl); sput(o,k,c->name,nl); k+=nl; sput1(o,k++,tl); sput1(o,k++,tl>>8); sput(o,k,c->link,tl); k+=tl; *any=1; continue; }
        if(c->dir){
            if(o&&k+2+nl+1>max) return -1;
            int k0=k, sub=0; sput1(o,k++,2); sput1(o,k++,nl); sput(o,k,c->name,nl); k+=nl;
            k=ser_rec(c,o,max,k,&sub); if(k<0) return -1;
            if(!sub&&!keep) k=k0; else *any=1;
            continue; }
        if(!keep) continue;
        if(o&&k+2+nl+4+c->size+1>max) return -1;
        sput1(o,k++,1); sput1(o,k++,nl); sput(o,k,c->name,nl); k+=nl;
        sput1(o,k++,c->size); sput1(o,k++,c->size>>8); sput1(o,k++,c->size>>16); sput1(o,k++,c->size>>24);
        if(c->size) sput(o,k,c->data,c->size);
        k+=c->size; *any=1;
        if(k<0) return -1;
    }
    if(o&&k+1>max) return -1;
    sput1(o,k++,0); return k;
}
int fs_serialize(uint8_t *out,int max){ int any=0; return ser_rec(root,out,max,0,&any); }   /* out=0: devuelve el tamano necesario */
static int de_rec(FsNode *d,const uint8_t *in,int len,int k){
    for(;;){
        if(k>=len) return -1;
        int t=in[k++]; if(t==0) return k;
        if(k>=len) return -1;
        int nl=in[k++]; if(k+nl>len||nl>=FS_NAME||nl==0) return -1;
        char nm[FS_NAME]; memcpy(nm,in+k,(size_t)nl); nm[nl]=0; k+=nl;
        if(t==2){ FsNode *c=newnode(d,nm,1); if(!c) return -1; k=de_rec(c,in,len,k); if(k<0) return -1; }
        else if(t==3){
            if(k+2>len) return -1;
            int tl=in[k]|(in[k+1]<<8); k+=2; if(k+tl>len) return -1;
            char tg[300]; if(tl>=(int)sizeof tg) return -1; memcpy(tg,in+k,(size_t)tl); tg[tl]=0; k+=tl; if(!fs_symlink(d,nm,tg)) return -1;
        } else if(t==1){
            if(k+4>len) return -1;
            int sz=in[k]|(in[k+1]<<8)|(in[k+2]<<16)|(in[k+3]<<24); k+=4;
            if(sz<0||k+sz>len) return -1;
            FsNode *c=newnode(d,nm,0); if(!c||fs_write(c,(const char*)in+k,sz)) return -1; k+=sz;
        } else return -1;
    }
}
static int ov_rec(FsNode *d,const uint8_t *in,int len,int k){
    for(;;){
        if(k>=len) return -1;
        int t=in[k++]; if(t==0) return k;
        if(k>=len) return -1;
        int nl=in[k++]; if(k+nl>len||nl>=FS_NAME||nl==0) return -1;
        char nm[FS_NAME]; memcpy(nm,in+k,(size_t)nl); nm[nl]=0; k+=nl;
        FsNode *e=fs_find(d,nm);
        if(t==2){ if(e&&!e->dir){ fs_remove(e); e=0; } if(!e){ e=newnode(d,nm,1); if(!e) return -1; } e->chg=1; k=ov_rec(e,in,len,k); if(k<0) return -1; }
        else if(t==3){
            if(k+2>len) return -1;
            int tl=in[k]|(in[k+1]<<8); k+=2; if(k+tl>len) return -1;
            char tg[300]; if(tl>=(int)sizeof tg) return -1; memcpy(tg,in+k,(size_t)tl); tg[tl]=0; k+=tl;
            if(e) fs_remove(e);
            FsNode *l=fs_symlink(d,nm,tg); if(!l) return -1; l->chg=1;
        } else if(t==1){
            if(k+4>len) return -1;
            int sz=in[k]|(in[k+1]<<8)|(in[k+2]<<16)|(in[k+3]<<24); k+=4;
            if(sz<0||k+sz>len) return -1;
            if(e&&(e->dir||e->link)){ fs_remove(e); e=0; }
            if(!e){ e=newnode(d,nm,0); if(!e) return -1; }
            if(fs_write(e,(const char*)in+k,sz)) return -1;
            e->chg=1; k+=sz;
        } else return -1;
    }
}
int fs_overlay(const uint8_t *in,int len){ int r=ov_rec(root,in,len,0); gen++; return r<0?-1:0; }
int fs_deserialize(const uint8_t *in,int len){          /* reemplaza TODO el arbol; solo antes de abrir ventanas */
    FsNode *old=root; root=newnode(0,"",1); if(!root){ root=old; return -1; }
    if(de_rec(root,in,len,0)<0){ while(root->child) fs_remove(root->child); kfree(root); root=old; return -1; }
    while(old->child) fs_remove(old->child);
    kfree(old); gen++; return 0;
}

static FsNode *res(FsNode *cwd,const char *path,int follow_last){
    char buf[600]; int l=k_strlen(path); if(l>=(int)sizeof buf) return 0; memcpy(buf,path,(size_t)l+1);
    const char *p=buf; FsNode *cur=(buf[0]=='/')?root:cwd; int hops=0;
    char part[FS_NAME];
    while(*p){
        while(*p=='/') p++;
        if(!*p) break;
        int n=0; while(*p&&*p!='/'){ if(n<FS_NAME-1) part[n++]=*p; p++; } part[n]=0;
        if(!k_strcmp(part,".")) continue;
        if(!k_strcmp(part,"..")){ if(cur->parent) cur=cur->parent; continue; }
        FsNode *c=fs_find(cur,part); if(!c) return 0;
        const char *rest=p; while(*rest=='/') rest++;
        if(c->link && (*rest || follow_last)){
            if(++hops>24) return 0;
            char nb[600]; int tl=k_strlen(c->link), rl=k_strlen(rest); if(tl+rl+2>=(int)sizeof nb) return 0;
            memcpy(nb,c->link,(size_t)tl); if(rl){ nb[tl]='/'; memcpy(nb+tl+1,rest,(size_t)rl+1); } else nb[tl]=0;
            memcpy(buf,nb,(size_t)(tl+(rl?1+rl:0))+1); p=buf;
            if(buf[0]=='/') cur=root;                           /* si es relativo, parte de la carpeta que contenia el enlace (cur) */
            continue; }
        cur=c;
    }
    return cur;
}
FsNode *fs_resolve(FsNode *cwd,const char *path){ return res(cwd,path,1); }
FsNode *fs_resolve_nf(FsNode *cwd,const char *path){ return res(cwd,path,0); }
void fs_path(FsNode *n,char *out,int max){
    if(n->dead){ int i=0; out[i++]='/'; for(const char *q=n->name;*q&&i<max-12;q++) out[i++]=*q; for(const char *q=" (deleted)";*q&&i<max-1;q++) out[i++]=*q; out[i]=0; return; }
    if(!n->parent){ k_strcpy(out,"/",max); return; }
    char tmp[256]; fs_path(n->parent,tmp,sizeof tmp);
    int l=k_strlen(tmp); if(tmp[l-1]!='/'&&l<254){ tmp[l++]='/'; tmp[l]=0; }
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
