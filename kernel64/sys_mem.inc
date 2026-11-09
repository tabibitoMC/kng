/* sys_mem.inc — memoria virtual de los programas: regiones (VMA) con paginas que se crean al primer uso, mmap/munmap/mprotect/mremap/
 * madvise, memoria compartida (memfd, shm_open, mmap compartido de archivos, shmget/shmat de System V). Se incluye en proc64.c. */

#define VK_ANON   1          /* anonima privada: paginas de cero al primer uso */
#define VK_SHARED 2          /* respaldada por un KObj de memoria compartida (las paginas se comparten entre procesos) */
#define VK_FILE   3          /* archivo privado: copia al primer uso del contenido del archivo */
#define PROT_W 2

/* ---------- objetos de memoria compartida (KObj tipo 12) ---------- */
static KObj *shm_new(u64 size,int contiguous){
    KObj *o=obj_new(12); if(!o) return 0;
    o->ssize=size; u64 np=(size+4095)/4096; if(!np) np=1;
    if(contiguous){ o->craw=kmalloc(np*4096+4096); if(!o->craw){ kfree(o); return 0; } o->contig=(u8*)(uintptr_t)(((u64)(uintptr_t)o->craw+4095)&~4095ull); memset(o->contig,0,np*4096); }
    else { o->ntab=(int)(np/64+1); o->tab=kmalloc((size_t)o->ntab*sizeof(u8*)); o->traw=kmalloc((size_t)o->ntab*sizeof(u8*)); if(!o->tab||!o->traw){ if(o->tab) kfree(o->tab); if(o->traw) kfree(o->traw); kfree(o); return 0; }
           memset(o->tab,0,(size_t)o->ntab*sizeof(u8*)); memset(o->traw,0,(size_t)o->ntab*sizeof(u8*)); }
    return o;
}
static void shm_free(KObj *o){
    if(o->craw) kfree(o->craw);
    if(o->tab){ for(int i=0;i<o->ntab;i++) if(o->traw[i]) kfree(o->traw[i]); kfree(o->tab); kfree(o->traw); }
}
static int shm_resize(KObj *o,u64 size){                            /* solo crece o se recorta el tamano; las paginas ya creadas no se mueven */
    if(o->contig){ if(size>((o->ssize+4095)&~4095ull)) return -1; o->ssize=size; return 0; }
    int nt=(int)((size+4095)/4096/64+1);
    if(nt>o->ntab){ u8 **t=kmalloc((size_t)nt*sizeof(u8*)), **r=kmalloc((size_t)nt*sizeof(u8*)); if(!t||!r){ if(t) kfree(t); if(r) kfree(r); return -1; }
        memset(t,0,(size_t)nt*sizeof(u8*)); memset(r,0,(size_t)nt*sizeof(u8*)); memcpy(t,o->tab,(size_t)o->ntab*sizeof(u8*)); memcpy(r,o->traw,(size_t)o->ntab*sizeof(u8*));
        kfree(o->tab); kfree(o->traw); o->tab=t; o->traw=r; o->ntab=nt; }
    o->ssize=size; return 0;
}
/* pagina 'idx' del objeto (direccion fisica = virtual, mapa de identidad); se crea al primer uso. 0 si esta fuera del tamano */
static u8 *shm_page(KObj *o,u64 idx){
    if(idx*4096>=((o->ssize+4095)&~4095ull)) return 0;
    if(o->contig) return o->contig+idx*4096;
    int c=(int)(idx/64); if(c>=o->ntab) return 0;
    if(!o->tab[c]){ u8 *raw=kmalloc(65*4096); if(!raw) return 0; u8 *al=(u8*)(uintptr_t)(((u64)(uintptr_t)raw+4095)&~4095ull); memset(al,0,64*4096); o->traw[c]=raw; o->tab[c]=al; }
    return o->tab[c]+(idx%64)*4096;
}
static u64 shm_rw(KObj *o,u64 off,u8 *buf,u64 n,int write){      /* lectura/escritura por llamadas read/write; devuelve bytes tratados */
    u64 done=0;
    while(done<n){ u64 idx=(off+done)/4096, po=(off+done)%4096, c=4096-po; if(c>n-done) c=n-done;
        if(off+done>=o->ssize) break;
        if(off+done+c>o->ssize) c=o->ssize-(off+done);
        u8 *p=shm_page(o,idx); if(!p) break;
        if(write) memcpy(p+po,buf+done,c); else memcpy(buf+done,p+po,c);
        done+=c; }
    return done;
}
/* un archivo normal que se va a mapear de forma compartida pasa a vivir en paginas alineadas (el FS apunta a ellas) */
static KObj *file_shm(FsNode *n){
    if(n->shm) return (KObj*)n->shm;
    u64 sz=(u64)n->size; KObj *o=shm_new(sz?sz:4096,1); if(!o) return 0;
    if(sz) memcpy(o->contig,n->data,sz);
    if(!n->ro) kfree(n->data);
    n->data=(char*)o->contig; n->ro=1; n->shm=o; o->refs++;               /* una referencia es la del archivo */
    return o;
}

/* ---------- regiones ---------- */
static int vma_find(Proc *pr,u64 va){                    /* indice de la region que contiene va, o -1 */
    int lo=0, hi=pr->nvma-1;
    while(lo<=hi){ int m=(lo+hi)/2; struct Vma *v=&pr->vma[m]; if(va<v->s) hi=m-1; else if(va>=v->e) lo=m+1; else return m; }
    return -1;
}
static int vma_grow(Proc *pr){
    if(pr->nvma<pr->cvma) return 0;
    int nc=pr->cvma?pr->cvma*2:64; struct Vma *n=kmalloc((size_t)nc*sizeof *n); if(!n) return -1;
    if(pr->nvma) memcpy(n,pr->vma,(size_t)pr->nvma*sizeof *n);
    if(pr->vma) kfree(pr->vma);
    pr->vma=n; pr->cvma=nc; return 0;
}
static u64 *pte_ptr(Proc *pr,u64 va){
    static const int sh[3]={39,30,21}; u64 *t=pr->pml4;
    if(!t) return 0;
    for(int l=0;l<3;l++){ u64 e=t[(va>>sh[l])&511]; if(!(e&1)) return 0; t=(u64*)(uintptr_t)(e&~0xFFFull); }
    return &t[(va>>12)&511];
}
static int umap_set(Proc *pr,u64 va,u64 phys,u64 fl){           /* pone una entrada final de tabla de paginas */
    static const int sh[3]={39,30,21}; u64 *t=pr->pml4;
    if(va<USER_MIN||va>=USER_MAX) return -1;
    for(int l=0;l<3;l++){ int i=(int)((va>>sh[l])&511); if(!(t[i]&1)){ u64 *n=pg_new(pr); if(!n) return -1; t[i]=(u64)(uintptr_t)n|7; } t=(u64*)(uintptr_t)(t[i]&~0xFFFull); }
    t[(va>>12)&511]=phys|fl; return 0;
}
/* recorre solo las paginas ya creadas de [s,e): salta de golpe los huecos de las tablas (una reserva de 1 TB cuesta poco) */
static void pt_walk(Proc *pr,u64 s,u64 e,void (*fn)(Proc*,u64*,u64),u64 arg){
    for(u64 va=s;va<e;){
        u64 *t=pr->pml4; if(!t) return;
        u64 e4=t[(va>>39)&511]; if(!(e4&1)){ va=(va|((1ull<<39)-1))+1; continue; }
        t=(u64*)(uintptr_t)(e4&~0xFFFull);
        u64 e3=t[(va>>30)&511]; if(!(e3&1)){ va=(va|((1ull<<30)-1))+1; continue; }
        t=(u64*)(uintptr_t)(e3&~0xFFFull);
        u64 e2=t[(va>>21)&511]; if(!(e2&1)){ va=(va|((1ull<<21)-1))+1; continue; }
        t=(u64*)(uintptr_t)(e2&~0xFFFull);
        u64 *p=&t[(va>>12)&511]; if(*p&1) fn(pr,p,arg);
        va+=4096; }
}
static void pg_release(Proc *pr,u64 *p,u64 arg){          /* paginas propias vuelven a la lista del proceso; las compartidas (bit 9) solo se desenlazan */
    (void)arg; u64 e=*p; *p=0;
    if(!(e&0x200)){ u64 phys=e&~0xFFFull; *(u64*)(uintptr_t)phys=(u64)(uintptr_t)pr->freepg; pr->freepg=(u64*)(uintptr_t)phys; }
}
static void vma_drop_pages(Proc *pr,struct Vma *v,u64 s,u64 e){ (void)v; pt_walk(pr,s,e,pg_release,0); }
static void pg_setprot(Proc *pr,u64 *p,u64 prot){ (void)pr; if(prot&PROT_W) *p|=2; else *p&=~2ull; }
/* quita el rango [s,e) de las regiones (recorta o parte las que lo cubren). Devuelve 0 */
static int vma_cut(Proc *pr,u64 s,u64 e,int drop){
    for(int i=0;i<pr->nvma;){
        struct Vma *v=&pr->vma[i];
        if(v->e<=s||v->s>=e){ i++; continue; }
        u64 cs=v->s>s?v->s:s, ce=v->e<e?v->e:e;
        if(drop) vma_drop_pages(pr,v,cs,ce);
        if(v->s<s&&v->e>e){                                      /* el rango cae en medio: se parte en dos */
            if(vma_grow(pr)) return -1;
            v=&pr->vma[i];
            for(int k=pr->nvma;k>i+1;k--) pr->vma[k]=pr->vma[k-1];
            pr->vma[i+1]=*v; pr->vma[i+1].s=e; if(v->kind==VK_FILE||v->kind==VK_SHARED) pr->vma[i+1].off=v->off+(e-v->s);
            if(v->obj) v->obj->refs++;
            v->e=s; pr->nvma++; i+=2; }
        else if(v->s<s){ v->e=s; i++; }
        else if(v->e>e){ if(v->kind==VK_FILE||v->kind==VK_SHARED) v->off+=e-v->s; v->s=e; i++; }
        else { if(v->obj) obj_unref(v->obj); for(int k=i;k+1<pr->nvma;k++) pr->vma[k]=pr->vma[k+1]; pr->nvma--; }
    }
    return 0;
}
static int vma_add(Proc *pr,u64 s,u64 e,u32 prot,u32 kind,KObj *obj,FsNode *node,u64 off){
    if(vma_cut(pr,s,e,1)) return -1;
    if(vma_grow(pr)) return -1;
    int i=0; while(i<pr->nvma&&pr->vma[i].s<s) i++;
    if(kind==VK_ANON&&i>0){ struct Vma *p=&pr->vma[i-1]; if(p->kind==VK_ANON&&p->e==s&&p->prot==prot){ p->e=e; return 0; } }   /* se une con la region anonima vecina */
    for(int k=pr->nvma;k>i;k--) pr->vma[k]=pr->vma[k-1];
    struct Vma *v=&pr->vma[i]; v->s=s; v->e=e; v->prot=prot; v->kind=kind; v->obj=obj; v->node=node; v->off=off; pr->nvma++;
    return 0;
}
static void vma_release(Proc *pr){
    for(int i=0;i<pr->nvma;i++) if(pr->vma[i].obj) obj_unref(pr->vma[i].obj);
    if(pr->vma) kfree(pr->vma);
    pr->vma=0; pr->nvma=pr->cvma=0; pr->freepg=0;
}
/* falta de pagina del usuario (o acceso del kernel a memoria de usuario aun sin crear). 0 = resuelta, -1 = acceso invalido */
static int vma_fault(Proc *pr,u64 va,int write){
    int i=vma_find(pr,va); if(i<0) return -1;
    struct Vma *v=&pr->vma[i];
    if(!v->prot) return -1;
    if(write&&!(v->prot&PROT_W)) return -1;
    u64 pg=va&~0xFFFull, fl=5|((v->prot&PROT_W)?2:0);
    if(v->kind==VK_SHARED){
        u8 *p=shm_page(v->obj,(pg-v->s+v->off)/4096); if(!p) return -1;
        return umap_set(pr,pg,(u64)(uintptr_t)p,fl|0x200);              /* bit 9: la pagina pertenece al objeto, no al proceso */
    }
    u64 *p=pg_new(pr); if(!p) return -1;
    if(v->kind==VK_FILE&&v->node){ u64 fo=pg-v->s+v->off; if(fo<(u64)v->node->size){ u64 n=(u64)v->node->size-fo; if(n>4096) n=4096; memcpy(p,v->node->data+fo,n); } }
    return umap_set(pr,pg,(u64)(uintptr_t)p,fl);
}
static void vma_protect(Proc *pr,u64 s,u64 e,u32 prot){
    for(int i=0;i<pr->nvma;i++){
        struct Vma *v=&pr->vma[i]; if(v->e<=s||v->s>=e) continue;
        u64 cs=v->s>s?v->s:s, ce=v->e<e?v->e:e;
        if(cs>v->s||ce<v->e){                                  /* parte la region para cambiar solo el pedazo */
            struct Vma orig=*v;
            if(vma_cut(pr,cs,ce,0)) return;
            if(orig.obj) orig.obj->refs++;
            if(vma_grow(pr)) return;
            int k=0; while(k<pr->nvma&&pr->vma[k].s<cs) k++;
            for(int m=pr->nvma;m>k;m--) pr->vma[m]=pr->vma[m-1];
            struct Vma *nv=&pr->vma[k]; *nv=orig; nv->s=cs; nv->e=ce; if(orig.kind==VK_FILE||orig.kind==VK_SHARED) nv->off=orig.off+(cs-orig.s); pr->nvma++;
            v=nv; i=k; }
        v->prot=prot;
        pt_walk(pr,cs,ce,pg_setprot,prot);
    }
}

/* ---------- mmap & cia ---------- */
#define MAP_SHARED_  0x01
#define MAP_FIXED_   0x10
#define MAP_ANON_    0x20
static u64 sys_mmap(u64 addr,u64 len,u64 prot,u64 flags,u64 fdn,u64 off){
    if(!len) return RET_ERR(EINVAL);
    len=(len+4095)&~4095ull;
    u64 va;
    if((flags&MAP_FIXED_) && addr>=USER_MIN && addr+len<=USER_MAX && !(addr&0xFFF)) va=addr;
    else if(!(flags&MAP_FIXED_) && addr>=USER_MIN && addr+len<=MMAP_BASE && !(addr&0xFFF) && vma_find(CP,addr)<0 && vma_find(CP,addr+len-1)<0 && 0) va=addr;
    else { va=CP->mmap_next; if(va+len>=STACK_TOP-STACK_SIZE-(1ull<<20)) return RET_ERR(ENOMEM); CP->mmap_next+=len+4096; }
    u32 pr=(u32)(prot&7);
    if(va<MMAP_BASE) pt_walk(CP,va,va+len,pg_release,0);                  /* encima de zonas cargadas de golpe (programa, brk): se descartan sus paginas */
    if(flags&MAP_ANON_){
        if(flags&MAP_SHARED_){ KObj *o=shm_new(len,0); if(!o) return RET_ERR(ENOMEM); if(vma_add(CP,va,va+len,pr,VK_SHARED,o,0,0)){ obj_unref(o); return RET_ERR(ENOMEM); } return va; }
        if(vma_add(CP,va,va+len,pr,VK_ANON,0,0,0)) return RET_ERR(ENOMEM);
        return va;
    }
    if(fdn>=MAXFD||!CP->fd[fdn].type) return RET_ERR(EBADF);
    FD *f=&CP->fd[fdn];
    if(f->type==2){
        FsNode *nd=f->node;
        if(flags&MAP_SHARED_){ KObj *o=file_shm(nd); if(!o) return RET_ERR(ENOMEM); o->refs++; if(vma_add(CP,va,va+len,pr,VK_SHARED,o,0,off)){ obj_unref(o); return RET_ERR(ENOMEM); } return va; }
        if(vma_add(CP,va,va+len,pr,VK_FILE,0,nd,off)) return RET_ERR(ENOMEM);
        return va;
    }
    if(f->type==12||(f->type==10&&f->obj)){ f->obj->refs++; if(vma_add(CP,va,va+len,pr,VK_SHARED,f->obj,0,off)){ obj_unref(f->obj); return RET_ERR(ENOMEM); } return va; }
    return RET_ERR(19);
}
static u64 sys_munmap(u64 a,u64 len){
    if(a&0xFFF||!len) return RET_ERR(EINVAL);
    len=(len+4095)&~4095ull; if(a<USER_MIN||a+len>USER_MAX) return RET_ERR(EINVAL);
    vma_cut(CP,a,a+len,1); return 0;
}
static u64 sys_mprotect(u64 a,u64 len,u64 prot){
    if(a&0xFFF) return RET_ERR(EINVAL);
    len=(len+4095)&~4095ull; if(!len) return 0;
    vma_protect(CP,a,a+len,(u32)(prot&7));
    if(a<MMAP_BASE&&(prot&7)) for(u64 va=a;va<a+len&&va<MMAP_BASE;va+=4096) if(vma_find(CP,va)<0&&va>=USER_MIN&&va<USER_MAX) umap(CP,va);        /* zonas cargadas de golpe (ELF, brk) */
    return 0;
}
static u64 sys_madvise(u64 a,u64 len,u64 adv){
    if(adv!=4&&adv!=8) return 0;                                /* DONTNEED / FREE: las paginas vuelven a ser de cero */
    len=(len+4095)&~4095ull;
    for(int i=0;i<CP->nvma;i++){ struct Vma *v=&CP->vma[i]; if(v->kind!=VK_ANON||v->e<=a||v->s>=a+len) continue;
        u64 s=v->s>a?v->s:a, e=v->e<a+len?v->e:a+len; vma_drop_pages(CP,v,s&~0xFFFull,e); }
    return 0;
}
static u64 sys_mremap(u64 old,u64 olen,u64 nlen,u64 flags){
    olen=(olen+4095)&~4095ull; nlen=(nlen+4095)&~4095ull;
    int i=vma_find(CP,old); if(i<0||old&0xFFF||!nlen) return RET_ERR(EINVAL);
    struct Vma v=CP->vma[i];
    if(nlen<=olen){ vma_cut(CP,old+nlen,old+olen,1); return old; }
    if(v.kind==VK_ANON&&vma_find(CP,old+olen)<0&&old+nlen<MMAP_BASE+(1ull<<40)&&old+olen==v.e){                    /* crece en su lugar si el hueco esta libre */
        int free_gap=1; for(int k=0;k<CP->nvma;k++){ struct Vma *o=&CP->vma[k]; if(o->s<old+nlen&&o->e>old+olen){ free_gap=0; break; } }
        if(free_gap&&old+nlen<CP->mmap_next){ CP->vma[i].e=old+nlen; return old; }
        if(free_gap&&old+olen==CP->mmap_next-4096){ CP->vma[i].e=old+nlen; CP->mmap_next=old+nlen+4096; return old; }
    }
    if(!(flags&1)) return RET_ERR(ENOMEM);                                                                         /* MREMAP_MAYMOVE */
    u64 nva=CP->mmap_next; if(nva+nlen>=STACK_TOP-STACK_SIZE-(1ull<<20)) return RET_ERR(ENOMEM); CP->mmap_next+=nlen+4096;
    u64 oo=v.off+(old-v.s);
    if(v.obj) v.obj->refs++;
    if(vma_add(CP,nva,nva+nlen,v.prot,v.kind,v.obj,v.node,oo)) return RET_ERR(ENOMEM);
    for(u64 o=0;o<olen;o+=4096){ u64 *p=pte_ptr(CP,old+o); if(p&&(*p&1)){ u64 e=*p; *p=0; umap_set(CP,nva+o,e&~0xFFFull,e&0xFFF); } }       /* las paginas se mueven, no se copian */
    vma_cut(CP,old,old+olen,0);
    return nva;
}

/* ---------- memfd y System V ---------- */
static u64 sys_memfd_create(u64 name_va,u64 flags){
    (void)name_va; int fd=fd_alloc(CP,0); if(fd<0) return RET_ERR(EMFILE);
    KObj *o=shm_new(0,0); if(!o) return RET_ERR(ENOMEM);
    FD *f=&CP->fd[fd]; memset(f,0,sizeof *f); f->type=12; f->obj=o; f->cloexec=(flags&1)?1:0; return (u64)fd;
}
static struct { int used, key, id, removed, nattach; KObj *o; u64 size; } shm_tab[256];
static int shm_next_id=1;
static u64 sys_shmget(u64 key,u64 size,u64 flags){
    if((long)key!=0) for(int i=0;i<256;i++) if(shm_tab[i].used&&shm_tab[i].key==(int)key&&!shm_tab[i].removed){ if(size>shm_tab[i].size) return RET_ERR(EINVAL); return (u64)shm_tab[i].id; }
    if(!(flags&01000)&&(long)key!=0) return RET_ERR(ENOENT);
    if(!size||size>(1ull<<30)) return RET_ERR(EINVAL);
    for(int i=0;i<256;i++) if(!shm_tab[i].used){ KObj *o=shm_new(size,0); if(!o) return RET_ERR(ENOMEM);
        shm_tab[i].used=1; shm_tab[i].key=(int)key; shm_tab[i].id=shm_next_id++; shm_tab[i].removed=0; shm_tab[i].nattach=0; shm_tab[i].o=o; shm_tab[i].size=size; return (u64)shm_tab[i].id; }
    return RET_ERR(ENOMEM);
}
static u64 sys_shmat(u64 id,u64 addr,u64 flags){
    for(int i=0;i<256;i++) if(shm_tab[i].used&&shm_tab[i].id==(int)id){
        u64 len=(shm_tab[i].size+4095)&~4095ull, va;
        if(addr) va=addr&~0xFFFull; else { va=CP->mmap_next; CP->mmap_next+=len+4096; }
        KObj *o=shm_tab[i].o; o->refs++;
        if(vma_add(CP,va,va+len,(flags&010000)?1:3,VK_SHARED,o,0,0)){ obj_unref(o); return RET_ERR(ENOMEM); }
        shm_tab[i].nattach++; return va; }
    return RET_ERR(EINVAL);
}
static u64 sys_shmdt(u64 addr){
    int i=vma_find(CP,addr); if(i<0) return RET_ERR(EINVAL);
    KObj *o=CP->vma[i].obj; u64 s=CP->vma[i].s, e=CP->vma[i].e;
    vma_cut(CP,s,e,1);
    for(int k=0;k<64;k++) if(shm_tab[k].used&&shm_tab[k].o==o){ if(shm_tab[k].nattach>0) shm_tab[k].nattach--; if(shm_tab[k].removed&&!shm_tab[k].nattach){ obj_unref(o); shm_tab[k].used=0; } }
    return 0;
}
static u64 sys_shmctl(u64 id,u64 cmd,u64 buf){
    for(int i=0;i<256;i++) if(shm_tab[i].used&&shm_tab[i].id==(int)id){
        if(cmd==0){ shm_tab[i].removed=1; if(!shm_tab[i].nattach){ obj_unref(shm_tab[i].o); shm_tab[i].used=0; } return 0; }
        if(cmd==2||cmd==13){ u8 ds[112]; memset(ds,0,sizeof ds); *(u64*)(ds+48)=shm_tab[i].size; *(u64*)(ds+88)=(u64)shm_tab[i].nattach; if(buf&&ucopy_to(CP,buf,ds,sizeof ds)) return RET_ERR(EFAULT); return 0; }
        return 0; }
    return RET_ERR(EINVAL);
}

static void fs_shm_release(void *o){ obj_unref((KObj*)o); }
