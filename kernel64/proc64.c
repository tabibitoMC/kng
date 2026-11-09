/* proc64.c — procesos de usuario (ring 3) del kernel de 64 bits: GDT/TSS, tablas de paginas por proceso, cargador ELF64,
 * pila inicial con argv/envp/auxv y llamadas al sistema con la ABI de Linux x86-64 (instruccion SYSCALL).
 *
 * Alcance actual (etapa 1): UN proceso a la vez, ejecutado hasta que termina. Sin hilos, fork ni senales todavia.
 * El espacio de usuario vive en el slot 1+ de la PML4 (>= 512 GB); el kernel sigue en identidad bajo 4 GB (solo supervisor).
 * Limite conocido: un ELF no-PIE enlazado en direcciones bajas (p. ej. 0x400000) NO se puede cargar todavia (requiere mover
 * el kernel a la mitad alta); los ELF PIE / static-pie si, porque se cargan en USER_BASE. */
#include <stdint.h>
#include <stddef.h>
#include "kng.h"
#include "fs.h"

typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
void kprintf(const char *f,...);
int  kgetc(void);
uint32_t kticks(void);
void k_write_console(const char *s,int n);

extern u64 kstk_top;
u64  user_enter(u64 entry,u64 usp,u64 cr3);
void user_leave(u64 code) __attribute__((noreturn));
void syscall_entry(void);
extern char blob_hola_start[], blob_hola_end[], blob_glibc_start[], blob_glibc_end[];

#define USER_MIN   0x8000000000ull       /* 512 GB: inicio del espacio de usuario */
#define USER_BASE  0x8000400000ull       /* donde se cargan los ELF PIE */
#define MMAP_BASE  0x8100000000ull
#define STACK_TOP  0x8200000000ull
#define STACK_SIZE (1024*1024)
#define USER_MAX   0x8400000000ull
#define EPERM_ 1
#define ENOENT 2
#define EBADF  9
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define EINVAL 22
#define ENOTTY 25
#define ENOSYS 38

/* ---------- GDT + TSS ---------- */
static u64 gdt[7];
static struct __attribute__((packed)) { u32 r0; u64 rsp0,rsp1,rsp2,r1,ist[7],r2; u16 r3,iomap; } tss;
struct __attribute__((packed)) gdtr { u16 lim; u64 base; };
static inline void wrmsr(u32 m,u64 v){ __asm__ volatile("wrmsr"::"c"(m),"a"((u32)v),"d"((u32)(v>>32)):"memory"); }
static inline u64 rdmsr(u32 m){ u32 a,d; __asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(m)); return ((u64)d<<32)|a; }

static u64 *k_pml4; static int proc_ready;

int proc_init(void){
    if(proc_ready) return 0;
    gdt[1]=0x00AF9A000000FFFFull; gdt[2]=0x00CF92000000FFFFull;       /* 0x08 codigo K, 0x10 datos K (igual que boot64.S) */
    gdt[3]=0x00CFF2000000FFFFull; gdt[4]=0x00AFFA000000FFFFull;       /* 0x18 datos U, 0x20 codigo U 64 bits */
    u8 *ks=kmalloc(65536+16); if(!ks) return -1;
    kstk_top=((u64)(uintptr_t)ks+65536)&~15ull; tss.rsp0=kstk_top; tss.iomap=sizeof tss;
    u64 b=(u64)(uintptr_t)&tss;
    gdt[5]=0x67ull|((b&0xFFFFFFull)<<16)|(0x89ull<<40)|(((b>>24)&0xFFull)<<56); gdt[6]=b>>32;
    struct gdtr g={sizeof gdt-1,(u64)(uintptr_t)gdt}; __asm__ volatile("lgdt %0"::"m"(g));
    __asm__ volatile("ltr %%ax"::"a"((u16)0x28));
    /* SSE/FXSR para programas normales (glibc usa SSE2) */
    { u64 cr0,cr4; __asm__ volatile("mov %%cr0,%0":"=r"(cr0)); cr0=(cr0&~4ull)|2ull; __asm__ volatile("mov %0,%%cr0"::"r"(cr0));
      __asm__ volatile("mov %%cr4,%0":"=r"(cr4)); cr4|=(1ull<<9)|(1ull<<10); __asm__ volatile("mov %0,%%cr4"::"r"(cr4)); __asm__ volatile("fninit"); }
    wrmsr(0xC0000080,rdmsr(0xC0000080)|1);                           /* EFER.SCE */
    wrmsr(0xC0000081,((u64)0x10<<48)|((u64)0x08<<32));               /* STAR: sysret CS=0x20|3 SS=0x18|3 */
    wrmsr(0xC0000082,(u64)(uintptr_t)syscall_entry);
    wrmsr(0xC0000084,0x4700);                                        /* SFMASK: IF, DF, TF, NT fuera durante la llamada */
    u64 cr3; __asm__ volatile("mov %%cr3,%0":"=r"(cr3)); k_pml4=(u64*)(uintptr_t)(cr3&~0xFFFull);
    proc_ready=1; return 0;
}

/* ---------- paginas y tablas ---------- */
#define SLAB_PAGES 64
#define MAX_SLABS  512
static void *slab_raw[MAX_SLABS]; static u64 slab_base[MAX_SLABS]; static int slab_n, slab_used;
static u64 *pg_new(void){
    if(!slab_n || slab_used>=SLAB_PAGES){
        if(slab_n>=MAX_SLABS) return 0;
        void *raw=kmalloc((SLAB_PAGES+1)*4096ull); if(!raw) return 0;
        slab_raw[slab_n]=raw; slab_base[slab_n]=((u64)(uintptr_t)raw+4095)&~4095ull; slab_n++; slab_used=0; }
    u64 *p=(u64*)(uintptr_t)(slab_base[slab_n-1]+(u64)slab_used*4096); slab_used++;
    memset(p,0,4096); return p;
}
static void pg_free_all(void){ for(int i=0;i<slab_n;i++) kfree(slab_raw[i]); slab_n=0; slab_used=0; }

static int umap(u64 *pml4,u64 va){          /* asegura que la pagina de 4 KB de 'va' exista (cero) */
    static const int sh[3]={39,30,21}; u64 *t=pml4;
    if(va<USER_MIN||va>=USER_MAX) return -1;
    for(int l=0;l<3;l++){ int i=(int)((va>>sh[l])&511); if(!(t[i]&1)){ u64 *n=pg_new(); if(!n) return -1; t[i]=(u64)(uintptr_t)n|7; } t=(u64*)(uintptr_t)(t[i]&~0xFFFull); }
    int i=(int)((va>>12)&511); if(!(t[i]&1)){ u64 *p=pg_new(); if(!p) return -1; t[i]=(u64)(uintptr_t)p|7; }
    return 0;
}
static u8 *uptr(u64 *pml4,u64 va){          /* direccion del kernel (identidad) que corresponde a 'va'; 0 si no esta mapeada */
    static const int sh[3]={39,30,21}; u64 *t=pml4;
    if(va<USER_MIN||va>=USER_MAX) return 0;
    for(int l=0;l<3;l++){ u64 e=t[(va>>sh[l])&511]; if(!(e&1)) return 0; t=(u64*)(uintptr_t)(e&~0xFFFull); }
    u64 e=t[(va>>12)&511]; if(!(e&1)) return 0;
    return (u8*)(uintptr_t)((e&~0xFFFull)|(va&0xFFF));
}
static int ucopy_to(u64 *pml4,u64 va,const void *src,u64 n){ const u8 *s=src; while(n){ u8 *d=uptr(pml4,va); if(!d) return -1; u64 c=4096-(va&0xFFF); if(c>n) c=n; memcpy(d,s,c); va+=c; s+=c; n-=c; } return 0; }
static int ucopy_from(u64 *pml4,void *dst,u64 va,u64 n){ u8 *o=dst; while(n){ u8 *s=uptr(pml4,va); if(!s) return -1; u64 c=4096-(va&0xFFF); if(c>n) c=n; memcpy(o,s,c); va+=c; o+=c; n-=c; } return 0; }
static int umap_range(u64 *pml4,u64 va,u64 len){ for(u64 p=va&~0xFFFull;p<va+len;p+=4096) if(umap(pml4,p)) return -1; return 0; }

/* ---------- estado del proceso ---------- */
#define MAXFD 16
static struct { u64 *pml4; u64 brk, brk0, mmap_next; FsNode *fd[MAXFD]; u64 pos[MAXFD]; int run; } P;
static u64 rng_s=88172645463325252ull;

/* ---------- ELF64 ---------- */
typedef struct { u8 id[16]; u16 type,machine; u32 ver; u64 entry,phoff,shoff; u32 flags; u16 ehsize,phentsize,phnum,shentsize,shnum,shstrndx; } Ehdr;
typedef struct { u32 type,flags; u64 off,vaddr,paddr,filesz,memsz,align; } Phdr;

static int elf_load(const u8 *img,u64 sz,u64 *entry,u64 *phdr_va,int *phnum,u64 *end){
    if(sz<sizeof(Ehdr)) return -1;
    const Ehdr *e=(const Ehdr*)img;
    if(e->id[0]!=0x7F||e->id[1]!='E'||e->id[2]!='L'||e->id[3]!='F'){ kprintf("ejecutar: no es un archivo ELF\n"); return -1; }
    if(e->id[4]!=2||e->id[5]!=1||e->machine!=62){ kprintf("ejecutar: se necesita ELF de 64 bits para x86-64\n"); return -1; }
    if(e->type!=2&&e->type!=3){ kprintf("ejecutar: tipo de ELF no soportado (%u)\n",e->type); return -1; }
    if(e->phentsize!=sizeof(Phdr)||e->phoff+(u64)e->phnum*sizeof(Phdr)>sz){ kprintf("ejecutar: cabeceras de programa invalidas\n"); return -1; }
    u64 base=e->type==3?USER_BASE:0, top=0, lo_off0=0; int have_phdr=0; *phdr_va=0;
    for(int i=0;i<e->phnum;i++){
        const Phdr *p=(const Phdr*)(img+e->phoff+(u64)i*sizeof(Phdr));
        if(p->type==3){ kprintf("ejecutar: el programa pide un enlazador dinamico (PT_INTERP): aun no soportado, usa un binario estatico\n"); return -1; }
        if(p->type==6){ *phdr_va=base+p->vaddr; have_phdr=1; }
        if(p->type!=1) continue;
        u64 va=base+p->vaddr;
        if(va<USER_MIN){ kprintf("ejecutar: el programa esta enlazado en una direccion baja (0x%lx). Usa un ejecutable PIE (-static-pie): aun no se puede cargar uno de direccion fija baja\n",va); return -1; }
        if(p->off+p->filesz>sz||p->filesz>p->memsz||va+p->memsz>=MMAP_BASE){ kprintf("ejecutar: segmento invalido\n"); return -1; }
        if(umap_range(P.pml4,va,p->memsz?p->memsz:1)){ kprintf("ejecutar: sin memoria para cargar el programa\n"); return -1; }
        if(ucopy_to(P.pml4,va,img+p->off,p->filesz)) return -1;
        if(va+p->memsz>top) top=va+p->memsz;
        if(!have_phdr && p->off==0) lo_off0=va;
    }
    if(!have_phdr && lo_off0) *phdr_va=lo_off0+e->phoff;
    *entry=base+e->entry; *phnum=e->phnum; *end=top; return 0;
}

static int build_stack(u64 *sp_out,int argc,char **argv,u64 entry,u64 phdr,int phnum){
    u64 *pm=P.pml4;
    if(umap_range(pm,STACK_TOP-STACK_SIZE,STACK_SIZE)) return -1;
    static const char *envs[]={"PATH=/bin","HOME=/","TERM=linux"}; int envc=3;
    u64 sp=STACK_TOP-64, argv_va[16], env_va[4];
    for(int i=0;i<argc;i++){ u64 l=(u64)k_strlen(argv[i])+1; sp-=l; if(ucopy_to(pm,sp,argv[i],l)) return -1; argv_va[i]=sp; }
    for(int i=0;i<envc;i++){ u64 l=(u64)k_strlen(envs[i])+1; sp-=l; if(ucopy_to(pm,sp,envs[i],l)) return -1; env_va[i]=sp; }
    u8 rnd[16]; for(int i=0;i<16;i++){ rng_s^=rng_s<<13; rng_s^=rng_s>>7; rng_s^=rng_s<<17; rnd[i]=(u8)rng_s; }
    sp-=16; u64 rnd_va=sp; if(ucopy_to(pm,sp,rnd,16)) return -1;
    u64 aux[][2]={{3,phdr},{4,sizeof(Phdr)},{5,(u64)phnum},{6,4096},{9,entry},{11,0},{12,0},{13,0},{14,0},{17,100},{23,0},{25,rnd_va},{0,0}};
    int na=(int)(sizeof aux/sizeof aux[0]);
    u64 words=1+(u64)argc+1+(u64)envc+1+(u64)na*2;
    sp=(sp-words*8)&~15ull; u64 q=sp; u64 v;
    v=(u64)argc; ucopy_to(pm,q,&v,8); q+=8;
    for(int i=0;i<argc;i++){ ucopy_to(pm,q,&argv_va[i],8); q+=8; } v=0; ucopy_to(pm,q,&v,8); q+=8;
    for(int i=0;i<envc;i++){ ucopy_to(pm,q,&env_va[i],8); q+=8; } v=0; ucopy_to(pm,q,&v,8); q+=8;
    for(int i=0;i<na;i++){ ucopy_to(pm,q,&aux[i][0],8); q+=8; ucopy_to(pm,q,&aux[i][1],8); q+=8; }
    *sp_out=sp; return 0;
}

/* ---------- llamadas al sistema (numeros y estructuras = ABI de Linux x86-64) ---------- */
struct sc { u64 nr,a6,a5,a4,a3,a2,a1,r11,rcx,usp; };
static void sys_exit_now(u64 code){ P.run=0; user_leave(code&0xFF); }
#define RET_ERR(e) ((u64)-(long)(e))

static u64 sys_open(u64 path_va){
    char path[160]; int i=0; for(;i<159;i++){ u8 *c=uptr(P.pml4,path_va+(u64)i); if(!c) return RET_ERR(EFAULT); path[i]=(char)*c; if(!*c) break; } path[159]=0;
    FsNode *n=fs_resolve(fs_root(),path); if(!n) return RET_ERR(ENOENT);
    for(int f=3;f<MAXFD;f++) if(!P.fd[f]){ P.fd[f]=n; P.pos[f]=0; return (u64)f; }
    return RET_ERR(24);
}
static char inl[256]; static int inl_n, inl_r, inl_ready;
static u64 sys_read(u64 fd,u64 buf,u64 n){
    if(fd==0){
        if(!n) return 0;
        while(!(inl_ready && inl_r<inl_n)){
            if(inl_ready && inl_r>=inl_n){ inl_ready=0; inl_n=inl_r=0; }
            int c=kgetc(); if(c<0){ __asm__ volatile("sti;hlt;cli"); continue; }
            if(c=='\b'){ if(inl_n>0){ inl_n--; k_write_console("\b",1); } }
            else if(c=='\n'){ if(inl_n<255) inl[inl_n++]='\n'; k_write_console("\n",1); inl_ready=1; }
            else if(c>=32&&c<256&&inl_n<254){ inl[inl_n++]=(char)c; char ch=(char)c; k_write_console(&ch,1); }
        }
        u64 k=(u64)(inl_n-inl_r); if(k>n) k=n; if(ucopy_to(P.pml4,buf,inl+inl_r,k)) return RET_ERR(EFAULT); inl_r+=(int)k; return k;
    }
    if(fd>=MAXFD||!P.fd[fd]) return RET_ERR(EBADF);
    FsNode *f=P.fd[fd]; if(f->dir) return RET_ERR(21);
    u64 left=P.pos[fd]<(u64)f->size?(u64)f->size-P.pos[fd]:0; if(n>left) n=left;
    if(n&&ucopy_to(P.pml4,buf,f->data+P.pos[fd],n)) return RET_ERR(EFAULT);
    P.pos[fd]+=n; return n;
}
static u64 sys_write(u64 fd,u64 buf,u64 n){
    if(fd!=1&&fd!=2) return RET_ERR(EBADF);
    u64 done=0; char tmp[256];
    while(done<n){ u64 c=n-done; if(c>sizeof tmp) c=sizeof tmp; if(ucopy_from(P.pml4,tmp,buf+done,c)) return done?done:RET_ERR(EFAULT); k_write_console(tmp,(int)c); done+=c; }
    return n;
}
static u64 sys_mmap(u64 addr,u64 len,u64 prot,u64 flags,u64 fd,u64 off){
    if(!len) return RET_ERR(EINVAL);
    len=(len+4095)&~4095ull;
    u64 va;
    if((flags&0x10) && addr>=USER_MIN && addr+len<=USER_MAX && !(addr&0xFFF)) va=addr;
    else { va=P.mmap_next; if(va+len>=STACK_TOP-STACK_SIZE-(1ull<<20)) return RET_ERR(ENOMEM); P.mmap_next+=len+4096; }
    if(prot!=0){                                  /* PROT_NONE solo reserva direcciones (se mapea en mprotect) */
        if(len>(256ull<<20)) return RET_ERR(ENOMEM);
        if(umap_range(P.pml4,va,len)) return RET_ERR(ENOMEM);
        if(!(flags&0x20) && (long)fd>=0){         /* mapeo de archivo: copia privada */
            if(fd>=MAXFD||!P.fd[fd]) return RET_ERR(EBADF);
            FsNode *f=P.fd[fd]; if(off<(u64)f->size){ u64 n=(u64)f->size-off; if(n>len) n=len; ucopy_to(P.pml4,va,f->data+off,n); }
        }
    }
    return va;
}
static u64 sys_brk(u64 want){
    if(want<P.brk0||want>=P.brk0+(256ull<<20)) return P.brk;
    if(want>P.brk){ if(umap_range(P.pml4,P.brk,want-P.brk)) return P.brk; }
    P.brk=want; return P.brk;
}
static const char *UN[6]={"Linux","kng","6.1.0-kng","kng-compat 1","x86_64","(none)"};
static u64 sys_dispatch(struct sc *s){
    u64 a1=s->a1,a2=s->a2,a3=s->a3,a4=s->a4,a5=s->a5,a6=s->a6;
    switch(s->nr){
    case 0:  return sys_read(a1,a2,a3);
    case 1:  return sys_write(a1,a2,a3);
    case 2:  return sys_open(a1);
    case 257:return sys_open(a2);
    case 3:  if(a1>=3&&a1<MAXFD&&P.fd[a1]){ P.fd[a1]=0; return 0; } return a1<3?0:RET_ERR(EBADF);
    case 8:  { if(a1>=MAXFD||!P.fd[a1]) return RET_ERR(EBADF); long o=(long)a2; u64 sz=(u64)P.fd[a1]->size; u64 np=a3==0?(u64)o:a3==1?P.pos[a1]+(u64)o:sz+(u64)o; P.pos[a1]=np; return np; }
    case 4: case 5: case 6: case 262: {                       /* stat / fstat / lstat / newfstatat */
        u64 sbuf=s->nr==262?a3:a2; u8 st[144]; memset(st,0,sizeof st); u32 mode=0x2190; u64 size=0;
        if(s->nr==5){ if(a1>=3){ if(a1>=MAXFD||!P.fd[a1]) return RET_ERR(EBADF); mode=P.fd[a1]->dir?040755:0100644; size=(u64)P.fd[a1]->size; } }
        else { u64 pv=s->nr==262?a2:a1; char path[160]; int i=0; for(;i<159;i++){ u8 *c=uptr(P.pml4,pv+(u64)i); if(!c) return RET_ERR(EFAULT); path[i]=(char)*c; if(!*c) break; } path[159]=0;
               FsNode *n=fs_resolve(fs_root(),path); if(!n) return RET_ERR(ENOENT); mode=n->dir?040755:0100644; size=(u64)n->size; }
        *(u32*)(st+24)=mode; *(u64*)(st+48)=size; *(u64*)(st+56)=4096;
        return ucopy_to(P.pml4,sbuf,st,sizeof st)?RET_ERR(EFAULT):0; }
    case 9:  return sys_mmap(a1,a2,a3,a4,a5,a6);
    case 10: if(a3&7){ if(umap_range(P.pml4,a1,a2)) return RET_ERR(ENOMEM); } return 0;     /* mprotect: pasa a mapear lo reservado */
    case 11: case 28: return 0;                                 /* munmap/madvise: de momento no devuelven memoria */
    case 12: return sys_brk(a1);
    case 13: case 14: case 131: return 0;                       /* senales: aun sin implementar, se aceptan en silencio */
    case 16: return RET_ERR(ENOTTY);
    case 17: { if(a1>=MAXFD||!P.fd[a1]) return RET_ERR(EBADF); FsNode *f=P.fd[a1]; u64 left=a4<(u64)f->size?(u64)f->size-a4:0; if(a3>left) a3=left; if(a3&&ucopy_to(P.pml4,a2,f->data+a4,a3)) return RET_ERR(EFAULT); return a3; }
    case 20: { u64 tot=0; for(u64 i=0;i<a3&&i<64;i++){ u64 iov[2]; if(ucopy_from(P.pml4,iov,a2+i*16,16)) return RET_ERR(EFAULT); long r=(long)sys_write(a1,iov[0],iov[1]); if(r<0) return tot?tot:(u64)r; tot+=(u64)r; } return tot; }
    case 21: case 89: return RET_ERR(ENOENT);
    case 24: return 0;
    case 35: case 230: { u64 ts=s->nr==35?a1:a3; u64 t[2]; if(ucopy_from(P.pml4,t,ts,16)) return RET_ERR(EFAULT); u32 until=kticks()+(u32)(t[0]*100+t[1]/10000000ull); while((int)(kticks()-until)<0) __asm__ volatile("sti;hlt;cli"); return 0; }
    case 39: return 1;
    case 60: case 231: sys_exit_now(a1); return 0;
    case 63: { u8 b[390]; memset(b,0,sizeof b); for(int i=0;i<6;i++) memcpy(b+i*65,UN[i],(size_t)k_strlen(UN[i])); return ucopy_to(P.pml4,a1,b,sizeof b)?RET_ERR(EFAULT):0; }
    case 79: { if(a2<2) return RET_ERR(34); return ucopy_to(P.pml4,a1,"/",2)?RET_ERR(EFAULT):2; }
    case 96: { u64 t[2]; u32 k=kticks(); t[0]=k/100; t[1]=(u64)(k%100)*10000; return a1&&ucopy_to(P.pml4,a1,t,16)?RET_ERR(EFAULT):0; }
    case 102: case 104: case 107: case 108: case 110: return 0;
    case 158: if(a1==0x1002){ wrmsr(0xC0000100,a2); return 0; } if(a1==0x1003){ u64 v=rdmsr(0xC0000100); return ucopy_to(P.pml4,a2,&v,8)?RET_ERR(EFAULT):0; } return RET_ERR(EINVAL);
    case 186: case 218: return 1;
    case 202: return (a2&0x7F)==0?RET_ERR(EAGAIN):0;            /* futex: un solo hilo (WAIT no puede bloquear; WAKE nadie) */
    case 228: { u64 t[2]; u32 k=kticks(); t[0]=k/100; t[1]=(u64)(k%100)*10000000ull; return ucopy_to(P.pml4,a2,t,16)?RET_ERR(EFAULT):0; }
    case 273: return 0;
    case 302: { if(a4){ u64 r[2]={8ull<<20,~0ull}; if(ucopy_to(P.pml4,a4,r,16)) return RET_ERR(EFAULT); } return 0; }
    case 318: { u8 b[256]; u64 n=a2>256?256:a2; for(u64 i=0;i<n;i++){ rng_s^=rng_s<<13; rng_s^=rng_s>>7; rng_s^=rng_s<<17; b[i]=(u8)rng_s; } return ucopy_to(P.pml4,a1,b,n)?RET_ERR(EFAULT):n; }
    case 334: return RET_ERR(ENOSYS);
    default:
        kprintf("[syscall] numero %lu aun no implementado -> ENOSYS\n",s->nr); return RET_ERR(ENOSYS);
    }
}
u64 sys_dispatch64(struct sc *s){ return sys_dispatch(s); }

void proc_fault(u64 no,u64 rip,u64 err,u64 cr2){
    kprintf("\n[proceso] excepcion %lu en el programa: rip=0x%lx err=0x%lx cr2=0x%lx -> proceso terminado\n",no,rip,err,cr2);
    P.run=0; user_leave(139);
}

/* ---------- API ---------- */
struct builtin { const char *name; char *s, *e; };
static struct builtin builtins[]={{"hola",blob_hola_start,blob_hola_end},{"hola-glibc",blob_glibc_start,blob_glibc_end}};
void proc_list(void){ kprintf("Programas incluidos en el kernel (ejecutar /bin/<nombre>):\n"); for(unsigned i=0;i<sizeof builtins/sizeof *builtins;i++) kprintf("  /bin/%s  (%lu bytes)\n",builtins[i].name,(u64)(builtins[i].e-builtins[i].s)); }

int proc_exec(char *path,int argc,char **argv){
    if(proc_init()){ kprintf("ejecutar: no hay memoria\n"); return -1; }
    if(P.run){ kprintf("ejecutar: ya hay un proceso en marcha\n"); return -1; }
    const u8 *img=0; u64 sz=0;
    if(k_startswith(path,"/bin/")) for(unsigned i=0;i<sizeof builtins/sizeof *builtins;i++) if(!k_strcmp(path+5,builtins[i].name)){ img=(u8*)builtins[i].s; sz=(u64)(builtins[i].e-builtins[i].s); }
    if(!img){ if(!fs_root()) fs_init(); FsNode *f=fs_resolve(fs_root(),path); if(!f||f->dir||!f->data){ kprintf("ejecutar: no existe %s\n",path); return -1; } img=(u8*)f->data; sz=(u64)f->size; }
    memset(&P,0,sizeof P); inl_n=inl_r=inl_ready=0;
    P.pml4=pg_new(); if(!P.pml4){ kprintf("ejecutar: sin memoria\n"); return -1; }
    P.pml4[0]=k_pml4[0];
    u64 entry,phdr,end,sp; int phnum;
    if(elf_load(img,sz,&entry,&phdr,&phnum,&end)||build_stack(&sp,argc,argv,entry,phdr,phnum)){ pg_free_all(); return -1; }
    P.brk0=P.brk=(end+4095+65536)&~4095ull; P.mmap_next=MMAP_BASE; P.run=1;
    u64 code=user_enter(entry,sp,(u64)(uintptr_t)P.pml4);
    wrmsr(0xC0000100,0); __asm__ volatile("sti");
    pg_free_all(); P.run=0;
    return (int)code;
}
