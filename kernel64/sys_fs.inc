/* sys_fs.inc — llamadas al sistema de archivos y carpetas (se incluye dentro de proc64.c, despues de sys_mem.inc).
 * open/openat, stat/fstat/lstat/newfstatat/statx, getdents64, lseek, pread/pwrite, truncate/ftruncate/fallocate, mkdir, rmdir, unlink, rename,
 * symlink, link, readlink, access, chdir/fchdir. Todas aceptan "carpeta de referencia" (dirfd) y siguen enlaces simbolicos. */

#define AT_FDCWD_L (-100L)
#define O_ACCMODE_ 3
#define O_CREAT_   0x40
#define O_EXCL_    0x80
#define O_TRUNC_   0x200
#define O_APPEND_  0x400
#define O_DIRECTORY_ 0x10000
#define O_NOFOLLOW_  0x20000
#define O_CLOEXEC_   0x80000
#define PATH_MAX_  420
enum { EACCES_=13, ENOTEMPTY_=39, ELOOP_=40, ESPIPE_=29, ENOSPC_=28, EXDEV_=18, ENAMETOOLONG_=36, ENXIO_=6, EBUSY_=16 };

static int upath(u64 va,char *out){ if(!va) return -EFAULT; if(ustr(CP,out,PATH_MAX_,va)) return -EFAULT; return 0; }
/* ruta absoluta a partir de una ruta del programa y su carpeta de referencia; devuelve 0 o -errno */
static int path_full(long dirfd,const char *path,char *full,int max){
    int n=0;
    if(path[0]!='/'){
        if(dirfd==AT_FDCWD_L) k_strcpy(full,CP->cwd,max);
        else { if(dirfd<0||dirfd>=MAXFD||!CP->fd[dirfd].type) return -EBADF; if(CP->fd[dirfd].type!=2||!CP->fd[dirfd].node->dir) return -ENOTDIR; fs_path(CP->fd[dirfd].node,full,max); }
        n=k_strlen(full); if(n&&full[n-1]!='/') full[n++]='/'; }
    int l=k_strlen(path); if(n+l>=max) return -ENAMETOOLONG_; memcpy(full+n,path,(size_t)l+1); return 0;
}
static FsNode *lookup_at(long dirfd,const char *path,int follow,int *err){
    char full[PATH_MAX_+100]; int r=path_full(dirfd,path,full,sizeof full); if(r){ *err=-r; return 0; }
    FsNode *n=follow?fs_resolve(fs_root(),full):fs_resolve_nf(fs_root(),full);
    if(!n) *err=ENOENT;
    return n;
}
static FsNode *parent_at(long dirfd,const char *path,char *leaf,int *err){      /* carpeta contenedora y nombre final */
    char full[PATH_MAX_+100]; int r=path_full(dirfd,path,full,sizeof full); if(r){ *err=-r; return 0; }
    int e=k_strlen(full); while(e>1&&full[e-1]=='/') full[--e]=0;
    int s2=e; while(s2>0&&full[s2-1]!='/') s2--;
    if(e-s2<=0){ *err=EEXIST; return 0; }
    if(e-s2>=FS_NAME){ *err=ENAMETOOLONG_; return 0; }
    memcpy(leaf,full+s2,(size_t)(e-s2)); leaf[e-s2]=0;
    if(!k_strcmp(leaf,".")||!k_strcmp(leaf,"..")){ *err=EEXIST; return 0; }
    if(s2<=1) full[1]=0; else full[s2-1]=0;
    FsNode *d=fs_resolve(fs_root(),full); if(!d){ *err=ENOENT; return 0; } if(!d->dir){ *err=ENOTDIR; return 0; }
    return d;
}
/* los archivos que crean los programas de Linux fuera de las carpetas propias de kng no se guardan en el disco de kng */
static void mark_sys(FsNode *n){
    char p[256]; fs_path(n,p,sizeof p);
    n->sys=!(k_startswith(p,"/Escritorio")||k_startswith(p,"/Documentos")||k_startswith(p,"/Imagenes")||k_startswith(p,"/Sistema"));
}
static FsNode *make_file(FsNode *d,const char *leaf){ FsNode *n=fs_create(d,leaf,"",0); if(n) mark_sys(n); return n; }

static u64 do_open(long dirfd,u64 path_va,u64 flags,u64 mode){
    (void)mode;
    char path[PATH_MAX_]; int e=upath(path_va,path); if(e) return RET_ERR(-e);
    if(!path[0]) return RET_ERR(ENOENT);
    if(!k_strcmp(path,"/etc/resolv.conf")) refresh_resolv();
    int err=ENOENT; FsNode *n=lookup_at(dirfd,path,!(flags&O_NOFOLLOW_),&err);
    if(!n&&err==ENOENT){ u64 v=virt_open(path,flags); if(v!=RET_ERR(ENOENT)) return v; }
    if(n&&(flags&O_CREAT_)&&(flags&O_EXCL_)) return RET_ERR(EEXIST);
    if(!n&&(flags&O_CREAT_)){
        char leaf[FS_NAME]; int e2=ENOENT; FsNode *d=parent_at(dirfd,path,leaf,&e2); if(!d) return RET_ERR(e2);
        n=make_file(d,leaf); if(!n) return RET_ERR(ENOSPC_);
    }
    if(!n) return RET_ERR(err);
    if(n->link) return RET_ERR(ELOOP_);
    if(n->dev==8) return RET_ERR(ENXIO_);                                  /* archivo de socket AF_UNIX: se abre con connect(), no con open() */
    if((flags&O_DIRECTORY_)&&!n->dir) return RET_ERR(ENOTDIR);
    if(n->dir&&(flags&O_ACCMODE_)) return RET_ERR(EISDIR);
    int f=fd_alloc(CP,0); if(f<0) return RET_ERR(EMFILE);
    FD *fp=&CP->fd[f]; memset(fp,0,sizeof *fp); fp->cloexec=(flags&O_CLOEXEC_)?1:0; fp->flags=(int)flags;
    if(n->dev){ fp->type=10; fp->aux=n->dev; fp->node=n; fs_node_ref(n); return (u64)f; }
    if((flags&O_TRUNC_)&&!n->dir&&(flags&O_ACCMODE_)) fs_resize(n,0);
    fp->type=2; fp->node=n; fp->pos=0; fs_node_ref(n); return (u64)f;
}

/* ---------- stat ---------- */
struct KStat { u32 mode; u64 size, ino, rdev; u32 nlink; u64 mtime; };
static void node_kstat(FsNode *n,struct KStat *k){
    k->mode=n->link?0120777:n->dir?040755:0100755; k->size=(u64)(n->link?k_strlen(n->link):n->size); k->ino=(u64)(uintptr_t)n>>4; k->rdev=0; k->nlink=n->dir?2:1; k->mtime=n->mtime;
    if(n->dev==8) k->mode=0140777; else if(n->dev){ k->mode=020666; k->rdev=(u64)n->dev; }
}
static int fd_kstat(long fd,struct KStat *k){
    if(fd<0||fd>=MAXFD||!CP->fd[fd].type) return -EBADF;
    FD *f=&CP->fd[fd]; memset(k,0,sizeof *k); k->nlink=1; k->ino=0x10000+(u64)fd;
    switch(f->type){
    case 1: k->mode=020620; k->rdev=0x8800; break;
    case 2: case 10: node_kstat(f->node,k); break;
    case 3: case 4: k->mode=010600; break;
    case 5: k->mode=0140777; break;
    case 11: k->mode=0100444; k->size=(u64)f->obj->size; k->ino=0x7000+(u64)(uintptr_t)f->obj; break;
    case 12: k->mode=0100600; k->size=f->obj->ssize; k->ino=(u64)(uintptr_t)f->obj>>4; break;
    default: k->mode=0100600; break; }
    return 0;
}
static int path_kstat(long dirfd,u64 path_va,int follow,struct KStat *k){
    char path[PATH_MAX_]; int e=upath(path_va,path); if(e) return e;
    if(!path[0]) return -ENOENT;
    int err=ENOENT; FsNode *n=lookup_at(dirfd,path,follow,&err);
    if(n){ memset(k,0,sizeof *k); node_kstat(n,k); return 0; }
    if(err==ENOENT){ char full[PATH_MAX_+100]; static char tmpb[8192];
        if(path_full(dirfd,path,full,sizeof full)==0&&is_virtual_path(full)&&gen_proc(full,tmpb,sizeof tmpb)>=0){ memset(k,0,sizeof *k); k->mode=0100444; k->ino=0x7000+(u64)k_strlen(full); k->nlink=1; return 0; } }
    return -err;
}
static void put_stat(u8 *st,const struct KStat *k){    /* struct stat de x86-64 (144 bytes) */
    u64 t=k->mtime?k->mtime:1700000000ull;
    memset(st,0,144);
    *(u64*)(st+0)=1; *(u64*)(st+8)=k->ino; *(u64*)(st+16)=k->nlink?k->nlink:1; *(u32*)(st+24)=k->mode; *(u64*)(st+40)=k->rdev;
    *(u64*)(st+48)=k->size; *(u64*)(st+56)=4096; *(u64*)(st+64)=(k->size+511)/512; *(u64*)(st+72)=t; *(u64*)(st+88)=t; *(u64*)(st+104)=t;
}
static u64 sys_stat_at(long dirfd,u64 path_va,u64 buf,u64 flags,int statx_mode,u64 mask){
    struct KStat k; int r;
    (void)mask;
    char c0=0; if(path_va) ucopy_from(CP,&c0,path_va,1);
    if((flags&0x1000)&&!c0) r=fd_kstat(dirfd,&k);              /* AT_EMPTY_PATH: se pregunta por el descriptor mismo */
    else r=path_kstat(dirfd,path_va,!(flags&0x100),&k);
    if(r) return RET_ERR(-r);
    if(!statx_mode){ u8 st[144]; put_stat(st,&k); return ucopy_to(CP,buf,st,sizeof st)?RET_ERR(EFAULT):0; }
    u8 sx[256]; memset(sx,0,sizeof sx); u64 t=k.mtime?k.mtime:1700000000ull;
    *(u32*)(sx+0)=0x7ff; *(u32*)(sx+4)=4096; *(u32*)(sx+16)=k.nlink?k.nlink:1; *(u16*)(sx+28)=(u16)k.mode; *(u64*)(sx+32)=k.ino; *(u64*)(sx+40)=k.size; *(u64*)(sx+48)=(k.size+511)/512;
    *(u64*)(sx+64)=t; *(u64*)(sx+80)=t; *(u64*)(sx+96)=t; *(u64*)(sx+112)=t; *(u32*)(sx+128)=(u32)(k.rdev>>8); *(u32*)(sx+132)=(u32)(k.rdev&255); *(u32*)(sx+136)=0; *(u32*)(sx+140)=1;
    return ucopy_to(CP,buf,sx,sizeof sx)?RET_ERR(EFAULT):0;
}
static u64 sys_fstat(u64 fd,u64 buf){
    struct KStat k; int r=fd_kstat((long)fd,&k); if(r) return RET_ERR(-r);
    u8 st[144]; put_stat(st,&k); return ucopy_to(CP,buf,st,sizeof st)?RET_ERR(EFAULT):0;
}

/* ---------- carpetas ---------- */
static u64 sys_getdents64(u64 fdn,u64 buf,u64 cap){
    if(fdn>=MAXFD||CP->fd[fdn].type!=2) return RET_ERR(EBADF);
    FD *f=&CP->fd[fdn]; FsNode *dn=f->node; if(!dn->dir) return RET_ERR(ENOTDIR);
    static u8 out[4096]; u64 off=0; if(cap>sizeof out) cap=sizeof out;
    FsNode *c=dn->child; u64 skip=f->pos>2?f->pos-2:0; while(c&&skip){ c=c->next; skip--; }
    for(;;){
        const char *nm; u8 ty; u64 ino; u64 np;
        if(f->pos==0){ nm="."; ty=4; ino=(u64)(uintptr_t)dn>>4; np=1; }
        else if(f->pos==1){ nm=".."; ty=4; ino=(u64)(uintptr_t)(dn->parent?dn->parent:dn)>>4; np=2; }
        else { if(!c) break; nm=c->name; ty=c->link?10:c->dir?4:c->dev==8?12:c->dev?2:8; ino=(u64)(uintptr_t)c>>4; np=f->pos+1; }
        int nl=k_strlen(nm); u64 rl=(u64)((19+nl+1+7)&~7); if(off+rl>cap){ if(!off) return RET_ERR(EINVAL); break; }
        u8 *e=out+off; memset(e,0,rl); *(u64*)e=ino; *(u64*)(e+8)=np; *(u16*)(e+16)=(u16)rl; e[18]=ty; memcpy(e+19,nm,(size_t)nl);
        off+=rl; if(f->pos>=2) c=c->next; f->pos=np;
    }
    if(off&&ucopy_to(CP,buf,out,off)) return RET_ERR(EFAULT);
    return off;
}
static u64 sys_lseek(u64 fdn,u64 off,u64 whence){
    if(fdn>=MAXFD||!CP->fd[fdn].type) return RET_ERR(EBADF);
    FD *f=&CP->fd[fdn]; u64 sz;
    switch(f->type){ case 2: sz=(u64)f->node->size; break; case 12: sz=f->obj->ssize; break; case 11: sz=(u64)f->obj->size; break; case 10: return 0; default: return RET_ERR(ESPIPE_); }
    long o=(long)off; long np=whence==0?o:whence==1?(long)f->pos+o:whence==2?(long)sz+o:-1;
    if(np<0) return RET_ERR(EINVAL);
    f->pos=(u64)np; return (u64)np;
}
/* lectura/escritura en una posicion sin mover el cursor (pread64/pwrite64) */
static u64 sys_pio(u64 fdn,u64 buf,u64 n,u64 off,int write){
    if(fdn>=MAXFD||!CP->fd[fdn].type) return RET_ERR(EBADF);
    FD *f=&CP->fd[fdn]; if(f->type!=2&&f->type!=12&&f->type!=11) return RET_ERR(ESPIPE_);
    u64 save=f->pos; f->pos=off; u64 r=write?fd_write(fdn,buf,n):fd_read(fdn,buf,n); f->pos=save; return r;
}
static u64 sys_ftruncate_fd(u64 fdn,u64 size){
    if(fdn>=MAXFD||!CP->fd[fdn].type) return RET_ERR(EBADF);
    FD *f=&CP->fd[fdn];
    if(f->type==12) return shm_resize(f->obj,size)?RET_ERR(ENOMEM):0;
    if(f->type!=2) return RET_ERR(EINVAL);
    if(f->node->dir) return RET_ERR(EISDIR);
    if(size>0x7fff0000ull) return RET_ERR(EINVAL);
    return fs_resize(f->node,(int)size)?RET_ERR(ENOSPC_):0;
}
static u64 sys_truncate_path(u64 path_va,u64 size){
    char path[PATH_MAX_]; int e=upath(path_va,path); if(e) return RET_ERR(-e);
    int err=ENOENT; FsNode *n=lookup_at(AT_FDCWD_L,path,1,&err); if(!n) return RET_ERR(err); if(n->dir) return RET_ERR(EISDIR);
    return fs_resize(n,(int)size)?RET_ERR(ENOSPC_):0;
}
static u64 sys_mkdirat(long dirfd,u64 path_va){
    char path[PATH_MAX_]; int e=upath(path_va,path); if(e) return RET_ERR(-e);
    char leaf[FS_NAME]; int err=ENOENT; FsNode *d=parent_at(dirfd,path,leaf,&err); if(!d) return RET_ERR(err);
    if(fs_find(d,leaf)) return RET_ERR(EEXIST);
    FsNode *n=fs_mkdir(d,leaf); if(!n) return RET_ERR(ENOSPC_); mark_sys(n); return 0;
}
static u64 sys_unlinkat(long dirfd,u64 path_va,u64 flags){
    char path[PATH_MAX_]; int e=upath(path_va,path); if(e) return RET_ERR(-e);
    int err=ENOENT; FsNode *n=lookup_at(dirfd,path,0,&err); if(!n) return RET_ERR(err); if(!n->parent) return RET_ERR(EBUSY_);
    if(n->dir){ if(!(flags&0x200)) return RET_ERR(EISDIR); if(n->child) return RET_ERR(ENOTEMPTY_); }
    else if(flags&0x200) return RET_ERR(ENOTDIR);
    return fs_remove(n)?RET_ERR(EBUSY_):0;
}
static u64 sys_renameat(long odir,u64 opath,long ndir,u64 npath){
    char o[PATH_MAX_],nw[PATH_MAX_],leaf[FS_NAME]; int e=upath(opath,o); if(e) return RET_ERR(-e); e=upath(npath,nw); if(e) return RET_ERR(-e);
    int err=ENOENT; FsNode *n=lookup_at(odir,o,0,&err); if(!n) return RET_ERR(err); if(!n->parent) return RET_ERR(EBUSY_);
    FsNode *d=parent_at(ndir,nw,leaf,&err); if(!d) return RET_ERR(err);
    FsNode *ex=fs_find(d,leaf);
    if(ex==n) return 0;
    if(ex){ if(ex->dir&&!n->dir) return RET_ERR(EISDIR); if(!ex->dir&&n->dir) return RET_ERR(ENOTDIR); if(ex->dir&&ex->child) return RET_ERR(ENOTEMPTY_); fs_remove(ex); }
    int r=(d==n->parent)?fs_rename(n,leaf):fs_move(n,d,leaf); if(r) return RET_ERR(EINVAL);
    mark_sys(n); return 0;
}
static u64 sys_symlinkat(u64 target_va,long dirfd,u64 path_va){
    char tg[PATH_MAX_],path[PATH_MAX_],leaf[FS_NAME]; int e=upath(target_va,tg); if(e) return RET_ERR(-e); e=upath(path_va,path); if(e) return RET_ERR(-e);
    if(!tg[0]) return RET_ERR(ENOENT);
    int err=ENOENT; FsNode *d=parent_at(dirfd,path,leaf,&err); if(!d) return RET_ERR(err);
    if(fs_find(d,leaf)) return RET_ERR(EEXIST);
    FsNode *n=fs_symlink(d,leaf,tg); if(!n) return RET_ERR(ENOSPC_); mark_sys(n); return 0;
}
static u64 sys_linkat(long odir,u64 opath,long ndir,u64 npath){      /* "enlace duro": kng no los tiene, se hace una copia */
    char o[PATH_MAX_],nw[PATH_MAX_],leaf[FS_NAME]; int e=upath(opath,o); if(e) return RET_ERR(-e); e=upath(npath,nw); if(e) return RET_ERR(-e);
    int err=ENOENT; FsNode *n=lookup_at(odir,o,1,&err); if(!n) return RET_ERR(err); if(n->dir) return RET_ERR(1);
    FsNode *d=parent_at(ndir,nw,leaf,&err); if(!d) return RET_ERR(err);
    if(fs_find(d,leaf)) return RET_ERR(EEXIST);
    FsNode *c=fs_copy(n,d,leaf); if(!c) return RET_ERR(ENOSPC_); mark_sys(c); return 0;
}
static u64 sys_readlinkat(long dirfd,u64 path_va,u64 buf,u64 size){
    char path[PATH_MAX_]; int e=upath(path_va,path); if(e) return RET_ERR(-e);
    char full[PATH_MAX_+100]; e=path_full(dirfd,path,full,sizeof full); if(e) return RET_ERR(-e);
    char tgt[300]; tgt[0]=0;
    if(!k_strcmp(full,"/proc/self/exe")) k_strcpy(tgt,CP->exe[0]?CP->exe:"/bin/kng-app",sizeof tgt);
    else if(!k_strcmp(full,"/proc/self/cwd")) k_strcpy(tgt,CP->cwd,sizeof tgt);
    else if(k_startswith(full,"/proc/self/fd/")){ int fd=0; const char *q=full+14; while(*q>='0'&&*q<='9') fd=fd*10+(*q++-'0');
        if(fd>=MAXFD||!CP->fd[fd].type) return RET_ERR(ENOENT);
        FD *f=&CP->fd[fd]; if(f->type==2||f->type==10) fs_path(f->node,tgt,sizeof tgt); else if(f->type==1) k_strcpy(tgt,"/dev/console",sizeof tgt); else if(f->type==5) k_strcpy(tgt,"socket:[1]",sizeof tgt);
        else if(f->type==3||f->type==4) k_strcpy(tgt,"pipe:[1]",sizeof tgt); else if(f->type==12) k_strcpy(tgt,"/memfd:kng (deleted)",sizeof tgt); else k_strcpy(tgt,"anon_inode:[kng]",sizeof tgt); }
    else { int err=ENOENT; FsNode *n=fs_resolve_nf(fs_root(),full); (void)err; if(!n) return RET_ERR(ENOENT); if(!n->link) return RET_ERR(EINVAL); k_strcpy(tgt,n->link,sizeof tgt); }
    u64 l=(u64)k_strlen(tgt); if(l>size) l=size; return ucopy_to(CP,buf,tgt,l)?RET_ERR(EFAULT):l;
}
static u64 sys_chdir_path(u64 path_va){
    char path[PATH_MAX_]; int e=upath(path_va,path); if(e) return RET_ERR(-e);
    int err=ENOENT; FsNode *n=lookup_at(AT_FDCWD_L,path,1,&err); if(!n) return RET_ERR(err); if(!n->dir) return RET_ERR(ENOTDIR);
    fs_path(n,CP->cwd,sizeof CP->cwd); return 0;
}
static u64 sys_fchdir(u64 fd){
    if(fd>=MAXFD||CP->fd[fd].type!=2) return RET_ERR(EBADF);
    if(!CP->fd[fd].node->dir) return RET_ERR(ENOTDIR);
    fs_path(CP->fd[fd].node,CP->cwd,sizeof CP->cwd); return 0;
}
static u64 sys_access_at(long dirfd,u64 path_va){
    char path[PATH_MAX_]; int e=upath(path_va,path); if(e) return RET_ERR(-e);
    if(!path[0]) return RET_ERR(ENOENT);
    int err=ENOENT; if(lookup_at(dirfd,path,1,&err)) return 0;
    char full[PATH_MAX_+100]; static char tmpb[8192];
    if(err==ENOENT&&path_full(dirfd,path,full,sizeof full)==0&&is_virtual_path(full)&&gen_proc(full,tmpb,sizeof tmpb)>=0) return 0;
    return RET_ERR(err);
}
static u64 sys_exists_nf(long dirfd,u64 path_va,int follow);
static u64 sys_exists_noop(long dirfd,u64 path_va){ return sys_exists_nf(dirfd,path_va,1); }
static u64 sys_exists_nf(long dirfd,u64 path_va,int follow){        /* chmod/chown/utimes...: kng no guarda permisos ni fechas; solo se comprueba que exista */
    char path[PATH_MAX_]; int e=upath(path_va,path); if(e) return RET_ERR(-e);
    int err=ENOENT; return lookup_at(dirfd,path,follow,&err)?0:RET_ERR(err);
}

/* fechas de archivos: mode 0 = utimensat (times = 2 timespec), 1 = utimes/futimesat (2 timeval), 2 = utime (actime, modtime) */
static u64 sys_utime(long dirfd,u64 path_va,u64 times,int mode,int follow){
    FsNode *n=0; int err=ENOENT;
    if(path_va){ char path[PATH_MAX_]; int e=upath(path_va,path); if(e) return RET_ERR(-e); n=lookup_at(dirfd,path,follow,&err); }
    else if(dirfd>=0&&dirfd<MAXFD&&(CP->fd[dirfd].type==2||CP->fd[dirfd].type==10)){ n=CP->fd[dirfd].node; }
    if(!n) return RET_ERR(err);
    u64 now=wall_sec(), mt=now;
    if(times){
        if(mode==0){ u64 t[4]; if(ucopy_from(CP,t,times,32)) return RET_ERR(EFAULT);
            if(t[3]==((1u<<30)-2)) return 0;                       /* UTIME_OMIT */
            mt=(t[3]==((1u<<30)-1))?now:t[2]; }
        else if(mode==1){ u64 t[4]; if(ucopy_from(CP,t,times+16,16)) return RET_ERR(EFAULT); mt=t[0]; }
        else { u64 t[2]; if(ucopy_from(CP,t,times,16)) return RET_ERR(EFAULT); mt=t[1]; }
    }
    n->mtime=(uint32_t)mt; return 0;
}
