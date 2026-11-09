/* proc64.c — procesos y tareas de usuario (ring 3) del kernel de 64 bits.
 *
 * Que hay: GDT/TSS, CR3 propia por proceso (la imagen del kernel, bajo 4 MB, va en todas pero solo supervisor; el resto del
 * kernel solo se ve con la CR3 del kernel), cargador ELF64 (PIE y de direccion fija, con enlazador dinamico), pila inicial
 * con argv/envp/auxv, planificador por turnos con varias tareas, fork/clone/execve/wait4/exit, hilos (CLONE_VM|CLONE_THREAD),
 * futex, pipes, descriptores de archivo sobre el sistema de archivos de kng y llamadas al sistema con la ABI de Linux x86-64.
 *
 * Limites conocidos: las senales con manejador del usuario aun no se entregan (kill/terminar si funcionan); fork copia toda la
 * memoria (sin copy-on-write); un solo nucleo de CPU; sin sockets para programas de usuario todavia. */
#include <stdint.h>
#include <stddef.h>
#include "kng.h"
#include "fs.h"

typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
void kprintf(const char *f,...);
int  kgetc(void);
uint32_t kticks(void);
void k_write_console(const char *s,int n);
void isr_handler64(void *r);

struct regs { u64 r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rbx,rdx,rcx,rax,int_no,err,rip,cs,rflags,rsp,ss; };
extern char kstack_top[];
extern u64 kcr3, ucr3, cur_ktop;
extern uint64_t pd[2048];
void switch_to(u64 *prev_ksp,u64 next_ksp);
void task_start(void);
void syscall_entry(void);
extern char blob_hola_start[], blob_hola_end[];

#define USER_MIN   0x400000ull           /* debajo de 4 MB vive la imagen del kernel (solo supervisor) */
#define USER_BASE  0x555555554000ull     /* donde se cargan los ELF PIE */
#define INTERP_BASE 0x7e0000000000ull    /* enlazador dinamico (ld.so) */
#define MMAP_BASE  0x7f0000000000ull
#define STACK_TOP  0x7ffffff00000ull
#define STACK_SIZE (1024*1024)
#define USER_MAX   0x7ffffffff000ull
enum { ENOENT=2, ESRCH=3, EINTR=4, ENOEXEC=8, EBADF=9, ECHILD=10, EAGAIN=11, ENOMEM=12, EFAULT=14, EEXIST=17, ENOTDIR=20, EISDIR=21,
       EINVAL=22, EMFILE=24, ENOTTY=25, ERANGE=34, EPIPE=32, ENOSYS=38, ETIMEDOUT=110 };
#define RET_ERR(e) ((u64)-(long)(e))
#define EXEC_DONE  0xE4EC0001ull

/* ---------- GDT + TSS ---------- */
static u64 gdt[7];
static struct __attribute__((packed)) { u32 r0; u64 rsp0,rsp1,rsp2,r1,ist[7],r2; u16 r3,iomap; } tss;
struct __attribute__((packed)) gdtr { u16 lim; u64 base; };
static inline void wrmsr(u32 m,u64 v){ __asm__ volatile("wrmsr"::"c"(m),"a"((u32)v),"d"((u32)(v>>32)):"memory"); }
static inline u64 rdmsr(u32 m){ u32 a,d; __asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(m)); return ((u64)d<<32)|a; }
static int proc_ready;

/* ---------- estructuras ---------- */
#define SLAB_PAGES 64
#define SLAB_MAX   256
#define MAXFD 64
#define MAXT  64
#define KSTK  32768
struct Pipe { u8 buf[4096]; int r,w,count,readers,writers; };
typedef struct { u8 type; FsNode *node; u64 pos; int flags; struct Pipe *pipe; } FD;      /* 0 libre, 1 consola, 2 archivo, 3 pipe (lectura), 4 pipe (escritura) */
typedef struct Proc {
    u64 *pml4; void *slab_raw[SLAB_MAX]; u64 slab_base[SLAB_MAX]; int slab_n, slab_used;
    u64 brk, brk0, mmap_next; FD fd[MAXFD]; int nthreads, pid; u64 sighand[65]; char cwd[96];
} Proc;
enum { T_FREE, T_READY, T_SLEEP, T_WAIT, T_READ, T_FUTEX, T_PIPE, T_ZOMBIE };
typedef struct Task {
    int tid, pid, ppid, state, leader, killed, exit_code, wait_pid, bg, fg, timedout, pipe_fd; Proc *pr; u8 *kstk; u64 ksp; struct regs *frame;
    u64 wake_at, fut_key, clear_tid, fs_base; char name[24];
} Task;
static Task tasks[MAXT]; static Task task0; static Task *cur=&task0; static int next_tid=1;
static void (*idle_hook)(void);
void proc_set_idle(void (*f)(void)){ idle_hook=f; }

int proc_init(void){
    if(proc_ready) return 0;
    gdt[1]=0x00AF9A000000FFFFull; gdt[2]=0x00CF92000000FFFFull;       /* 0x08 codigo K, 0x10 datos K (igual que boot64.S) */
    gdt[3]=0x00CFF2000000FFFFull; gdt[4]=0x00AFFA000000FFFFull;       /* 0x18 datos U, 0x20 codigo U 64 bits */
    tss.rsp0=(u64)(uintptr_t)kstack_top; tss.iomap=sizeof tss;
    u64 b=(u64)(uintptr_t)&tss;
    gdt[5]=0x67ull|((b&0xFFFFFFull)<<16)|(0x89ull<<40)|(((b>>24)&0xFFull)<<56); gdt[6]=b>>32;
    struct gdtr g={sizeof gdt-1,(u64)(uintptr_t)gdt}; __asm__ volatile("lgdt %0"::"m"(g));
    __asm__ volatile("ltr %%ax"::"a"((u16)0x28));
    { u64 cr0,cr4; __asm__ volatile("mov %%cr0,%0":"=r"(cr0)); cr0=(cr0&~4ull)|2ull; __asm__ volatile("mov %0,%%cr0"::"r"(cr0));
      __asm__ volatile("mov %%cr4,%0":"=r"(cr4)); cr4|=(1ull<<9)|(1ull<<10); __asm__ volatile("mov %0,%%cr4"::"r"(cr4)); __asm__ volatile("fninit"); }
    wrmsr(0xC0000080,rdmsr(0xC0000080)|1);                           /* EFER.SCE */
    wrmsr(0xC0000081,((u64)0x10<<48)|((u64)0x08<<32));
    wrmsr(0xC0000082,(u64)(uintptr_t)syscall_entry);
    wrmsr(0xC0000084,0x4700);
    u64 cr3; __asm__ volatile("mov %%cr3,%0":"=r"(cr3)); kcr3=cr3;
    task0.state=T_READY; task0.leader=1; k_strcpy(task0.name,"kernel",sizeof task0.name);
    proc_ready=1; return 0;
}

/* ---------- paginas y tablas (por proceso) ---------- */
static u64 *pg_new(Proc *pr){
    if(!pr->slab_n || pr->slab_used>=SLAB_PAGES){
        if(pr->slab_n>=SLAB_MAX) return 0;
        void *raw=kmalloc((SLAB_PAGES+1)*4096ull); if(!raw) return 0;
        pr->slab_raw[pr->slab_n]=raw; pr->slab_base[pr->slab_n]=((u64)(uintptr_t)raw+4095)&~4095ull; pr->slab_n++; pr->slab_used=0; }
    u64 *p=(u64*)(uintptr_t)(pr->slab_base[pr->slab_n-1]+(u64)pr->slab_used*4096); pr->slab_used++;
    memset(p,0,4096); return p;
}
static void pg_free_all(Proc *pr){ for(int i=0;i<pr->slab_n;i++) kfree(pr->slab_raw[i]); pr->slab_n=0; pr->slab_used=0; pr->pml4=0; }
static int proc_space(Proc *pr){             /* PML4 nueva con los primeros 4 MB del kernel (solo supervisor) */
    pr->pml4=pg_new(pr); u64 *pdpt=pg_new(pr), *pd0=pg_new(pr);
    if(!pr->pml4||!pdpt||!pd0) return -1;
    pr->pml4[0]=(u64)(uintptr_t)pdpt|7; pdpt[0]=(u64)(uintptr_t)pd0|7; pd0[0]=pd[0]; pd0[1]=pd[1];
    return 0;
}
static int umap(Proc *pr,u64 va){
    static const int sh[3]={39,30,21}; u64 *t=pr->pml4;
    if(va<USER_MIN||va>=USER_MAX) return -1;
    for(int l=0;l<3;l++){ int i=(int)((va>>sh[l])&511); if(!(t[i]&1)){ u64 *n=pg_new(pr); if(!n) return -1; t[i]=(u64)(uintptr_t)n|7; } t=(u64*)(uintptr_t)(t[i]&~0xFFFull); }
    int i=(int)((va>>12)&511); if(!(t[i]&1)){ u64 *p=pg_new(pr); if(!p) return -1; t[i]=(u64)(uintptr_t)p|7; }
    return 0;
}
static u8 *uptr(Proc *pr,u64 va){
    static const int sh[3]={39,30,21}; u64 *t=pr->pml4;
    if(!t||va<USER_MIN||va>=USER_MAX) return 0;
    for(int l=0;l<3;l++){ u64 e=t[(va>>sh[l])&511]; if(!(e&1)) return 0; t=(u64*)(uintptr_t)(e&~0xFFFull); }
    u64 e=t[(va>>12)&511]; if(!(e&1)) return 0;
    return (u8*)(uintptr_t)((e&~0xFFFull)|(va&0xFFF));
}
static int ucopy_to(Proc *pr,u64 va,const void *src,u64 n){ const u8 *s=src; while(n){ u8 *d=uptr(pr,va); if(!d) return -1; u64 c=4096-(va&0xFFF); if(c>n) c=n; memcpy(d,s,c); va+=c; s+=c; n-=c; } return 0; }
static int ucopy_from(Proc *pr,void *dst,u64 va,u64 n){ u8 *o=dst; while(n){ u8 *s=uptr(pr,va); if(!s) return -1; u64 c=4096-(va&0xFFF); if(c>n) c=n; memcpy(o,s,c); va+=c; o+=c; n-=c; } return 0; }
static int umap_range(Proc *pr,u64 va,u64 len){ for(u64 p=va&~0xFFFull;p<va+len;p+=4096) if(umap(pr,p)) return -1; return 0; }
static int ustr(Proc *pr,char *out,int max,u64 va){ for(int i=0;i<max-1;i++){ u8 *c=uptr(pr,va+(u64)i); if(!c) return -1; out[i]=(char)*c; if(!*c) return 0; } out[max-1]=0; return 0; }

static int as_clone(Proc *dst,Proc *src){      /* copia completa del espacio de usuario (fork) */
    for(int a=0;a<512;a++){ u64 e4=src->pml4[a]; if(!(e4&1)) continue; u64 *pdpt=(u64*)(uintptr_t)(e4&~0xFFFull);
      for(int b=0;b<512;b++){ u64 e3=pdpt[b]; if(!(e3&1)) continue; u64 *pdd=(u64*)(uintptr_t)(e3&~0xFFFull);
        for(int c=0;c<512;c++){ u64 e2=pdd[c]; if(!(e2&1)||(e2&0x80)) continue; u64 *pt=(u64*)(uintptr_t)(e2&~0xFFFull);
          for(int d=0;d<512;d++){ u64 e1=pt[d]; if(!(e1&1)) continue;
            u64 va=((u64)a<<39)|((u64)b<<30)|((u64)c<<21)|((u64)d<<12);
            if(va<USER_MIN) continue;
            if(umap(dst,va)) return -1;
            memcpy(uptr(dst,va),(void*)(uintptr_t)(e1&~0xFFFull),4096); } } } }
    return 0;
}

/* ---------- tareas y planificador ---------- */
static Task *task_alloc(void){
    for(int i=0;i<MAXT;i++) if(tasks[i].state==T_FREE){
        Task *t=&tasks[i]; u8 *ks=t->kstk; memset(t,0,sizeof *t); t->kstk=ks;
        if(!t->kstk){ t->kstk=kmalloc(KSTK); if(!t->kstk) return 0; }
        t->tid=next_tid++; return t; }
    return 0;
}
static void task_prepare(Task *t,const struct regs *fr){      /* pila de kernel: [r15..rbp][task_start][marco] */
    u8 *top=t->kstk+KSTK; struct regs *f=(struct regs*)(top-sizeof(struct regs)); *f=*fr; t->frame=f;
    u64 *sp=(u64*)f; *--sp=(u64)(uintptr_t)task_start; for(int i=0;i<6;i++) *--sp=0; t->ksp=(u64)(uintptr_t)sp;
}
static void fd_close(FD *f){
    if(f->type==3||f->type==4){ struct Pipe *p=f->pipe; if(f->type==3) p->readers--; else p->writers--; if(p->readers<=0&&p->writers<=0) kfree(p); }
    memset(f,0,sizeof *f);
}
static void fd_dup_into(FD *d,const FD *s){ *d=*s; if(s->type==3) s->pipe->readers++; else if(s->type==4) s->pipe->writers++; }

static u8 inl[256]; static int inl_n, inl_r, inl_ready;
static void stdin_pump(void){
    int c;
    while((c=kgetc())>=0){
        if(c=='\b'){ if(inl_n>0){ inl_n--; k_write_console("\b",1); } }
        else if(c=='\n'){ if(inl_n<255) inl[inl_n++]='\n'; k_write_console("\n",1); inl_ready=1; }
        else if(c>=32&&inl_n<254){ inl[inl_n++]=(u8)c; char ch=(char)c; k_write_console(&ch,1); }
    }
}
static int pipe_ready(Task *t){
    FD *f=&t->pr->fd[t->pipe_fd]; if(f->type!=3&&f->type!=4) return 1; struct Pipe *p=f->pipe;
    return f->type==3 ? (p->count>0||p->writers<=0) : (p->count<4096||p->readers<=0);
}
static void poll_wakeups(void){
    u32 now=kticks(); int any_read=0;
    for(int i=0;i<MAXT;i++){ Task *t=&tasks[i];
        if(t->state==T_FREE||t->state==T_READY) continue;
        if(t->state==T_ZOMBIE){
            if(t!=cur&&!t->leader) t->state=T_FREE;                       /* hilo terminado: su hueco se reutiliza */
            else if(t!=cur&&t->leader&&t->bg){ kprintf("[proceso %d termino con codigo %d]\n",t->pid,t->exit_code); t->state=T_FREE; }
            else if(t!=cur&&t->leader&&!t->fg&&t->ppid==0) t->state=T_FREE;      /* huerfano ya terminado */
            continue; }
        if(t->state==T_READ) any_read=1;
        if(t->killed){ t->state=T_READY; continue; }
        if(t->state==T_SLEEP&&(int)(now-(u32)t->wake_at)>=0) t->state=T_READY;
        else if(t->state==T_FUTEX&&t->wake_at&&(int)(now-(u32)t->wake_at)>=0){ t->wake_at=0; t->fut_key=0; t->timedout=1; t->state=T_READY; }
        else if(t->state==T_PIPE&&pipe_ready(t)) t->state=T_READY;
        else if(t->state==T_WAIT){
            for(int j=0;j<MAXT;j++){ Task *c=&tasks[j]; if(c->state==T_ZOMBIE&&c->leader&&c->ppid==t->pid&&(t->wait_pid<=0||c->pid==t->wait_pid)){ t->state=T_READY; break; } }
        }
    }
    if(any_read){ stdin_pump(); if(inl_ready) for(int i=0;i<MAXT;i++) if(tasks[i].state==T_READ) tasks[i].state=T_READY; }
}
static Task *pick_next(void){                    /* turnos: tasks[0..MAXT-1] y luego el kernel (task0) */
    int me=cur==&task0?MAXT:(int)(cur-tasks);
    for(int k=1;k<=MAXT+1;k++){ int i=(me+k)%(MAXT+1); Task *t=i==MAXT?&task0:&tasks[i]; if(t->state==T_READY) return t; }
    return &task0;
}
static int any_ready_user(void){ for(int i=0;i<MAXT;i++) if(tasks[i].state==T_READY) return 1; return 0; }
static void run_task(Task *n){
    if(n==cur) return;
    Task *p=cur; if(p->pr) p->fs_base=rdmsr(0xC0000100);
    cur=n;
    if(n->kstk){ cur_ktop=(u64)(uintptr_t)(n->kstk+KSTK); }
    if(n->pr){ ucr3=(u64)(uintptr_t)n->pr->pml4; wrmsr(0xC0000100,n->fs_base); }
    switch_to(&p->ksp,n->ksp);
}
static void schedule(void){ poll_wakeups(); run_task(pick_next()); }
static void block(int state){ cur->state=state; schedule(); }
void proc_pump(void){ if(!proc_ready) return; poll_wakeups(); if(any_ready_user()) schedule(); }

/* ---------- salida de procesos ---------- */
static void futex_wake(Proc *pr,u64 addr,int n,int *woken){
    u64 key=(u64)(uintptr_t)pr+addr; int w=0;
    for(int i=0;i<MAXT&&w<n;i++) if(tasks[i].state==T_FUTEX&&tasks[i].fut_key==key){ tasks[i].state=T_READY; tasks[i].fut_key=0; tasks[i].wake_at=0; w++; }
    if(woken) *woken=w;
}
static void proc_teardown(Proc *pr){
    for(int i=0;i<MAXFD;i++) if(pr->fd[i].type) fd_close(&pr->fd[i]);
    pg_free_all(pr);
    for(int i=0;i<MAXT;i++) if(tasks[i].pr==pr) tasks[i].pr=0;
    kfree(pr);
}
static void thread_exit_cleanup(Task *t){
    if(t->clear_tid&&t->pr){ u32 z=0; ucopy_to(t->pr,t->clear_tid,&z,4); futex_wake(t->pr,t->clear_tid,1,0); t->clear_tid=0; }
}
static void do_exit_group(int code) __attribute__((noreturn));
static void do_exit_group(int code){
    Task *me=cur; Proc *pr=me->pr; int pid=me->pid;
    thread_exit_cleanup(me);
    for(int i=0;i<MAXT;i++){ Task *t=&tasks[i]; if(t!=me&&t->pr==pr&&t->state!=T_FREE&&t->state!=T_ZOMBIE){ thread_exit_cleanup(t); if(t->leader) t->exit_code=code; t->state=T_ZOMBIE; } }
    for(int i=0;i<MAXT;i++) if(tasks[i].state!=T_FREE&&tasks[i].ppid==pid&&tasks[i].leader){ tasks[i].ppid=0; if(tasks[i].state==T_ZOMBIE&&!tasks[i].bg) tasks[i].state=T_FREE; }
    proc_teardown(pr);
    me->exit_code=code; me->state=T_ZOMBIE;
    schedule();
    for(;;) __asm__ volatile("hlt");            /* no se vuelve a planificar */
}

/* ---------- ELF64 ---------- */
typedef struct { u8 id[16]; u16 type,machine; u32 ver; u64 entry,phoff,shoff; u32 flags; u16 ehsize,phentsize,phnum,shentsize,shnum,shstrndx; } Ehdr;
typedef struct { u32 type,flags; u64 off,vaddr,paddr,filesz,memsz,align; } Phdr;
struct elfinfo { u64 entry, base, phdr, end; int phnum; char interp[96]; };

static int elf_map(Proc *pr,const u8 *img,u64 sz,u64 dyn_base,struct elfinfo *o,int allow_interp){
    if(sz<sizeof(Ehdr)) return -1;
    const Ehdr *e=(const Ehdr*)img;
    if(e->id[0]!=0x7F||e->id[1]!='E'||e->id[2]!='L'||e->id[3]!='F'){ kprintf("ejecutar: no es un archivo ELF\n"); return -1; }
    if(e->id[4]!=2||e->id[5]!=1||e->machine!=62){ kprintf("ejecutar: se necesita ELF de 64 bits para x86-64\n"); return -1; }
    if(e->type!=2&&e->type!=3){ kprintf("ejecutar: tipo de ELF no soportado (%u)\n",e->type); return -1; }
    if(e->phentsize!=sizeof(Phdr)||e->phoff+(u64)e->phnum*sizeof(Phdr)>sz){ kprintf("ejecutar: cabeceras de programa invalidas\n"); return -1; }
    u64 base=e->type==3?dyn_base:0, top=0, lo0=0; int have_phdr=0;
    memset(o,0,sizeof *o);
    for(int i=0;i<e->phnum;i++){
        const Phdr *p=(const Phdr*)(img+e->phoff+(u64)i*sizeof(Phdr));
        if(p->type==3){ if(!allow_interp||p->off+p->filesz>sz||p->filesz>=sizeof o->interp){ kprintf("ejecutar: PT_INTERP invalido\n"); return -1; } memcpy(o->interp,img+p->off,p->filesz); o->interp[p->filesz]=0; }
        if(p->type==6){ o->phdr=base+p->vaddr; have_phdr=1; }
        if(p->type!=1) continue;
        u64 va=base+p->vaddr;
        if(va<USER_MIN){ kprintf("ejecutar: el programa esta enlazado por debajo de 4 MB (0x%lx): ese rango es del kernel\n",va); return -1; }
        if(p->off+p->filesz>sz||p->filesz>p->memsz||va+p->memsz>=MMAP_BASE){ kprintf("ejecutar: segmento invalido\n"); return -1; }
        if(umap_range(pr,va,p->memsz?p->memsz:1)){ kprintf("ejecutar: sin memoria para cargar el programa\n"); return -1; }
        if(ucopy_to(pr,va,img+p->off,p->filesz)) return -1;
        if(va+p->memsz>top) top=va+p->memsz;
        if(!have_phdr && p->off==0) lo0=va;
    }
    if(!have_phdr && lo0) o->phdr=lo0+e->phoff;
    o->entry=base+e->entry; o->base=base; o->phnum=e->phnum; o->end=top; return 0;
}

#define MAXARG 16
static int build_stack(Proc *pr,u64 *sp_out,int argc,char **argv,int envc,char **envp,const char *execfn,u64 entry,u64 phdr,int phnum,u64 at_base){
    if(argc>MAXARG){ argc=MAXARG; }
    if(envc>MAXARG){ envc=MAXARG; }
    if(umap_range(pr,STACK_TOP-STACK_SIZE,STACK_SIZE)) return -1;
    u64 sp=STACK_TOP-64, argv_va[MAXARG], env_va[MAXARG], l;
    l=(u64)k_strlen(execfn)+1; sp-=l; if(ucopy_to(pr,sp,execfn,l)) return -1; u64 execfn_va=sp;
    for(int i=0;i<argc;i++){ l=(u64)k_strlen(argv[i])+1; sp-=l; if(ucopy_to(pr,sp,argv[i],l)) return -1; argv_va[i]=sp; }
    for(int i=0;i<envc;i++){ l=(u64)k_strlen(envp[i])+1; sp-=l; if(ucopy_to(pr,sp,envp[i],l)) return -1; env_va[i]=sp; }
    sp-=8; if(ucopy_to(pr,sp,"x86_64",7)) return -1; u64 plat_va=sp;
    static u64 rng_s=88172645463325252ull;
    u8 rnd[16]; for(int i=0;i<16;i++){ rng_s^=rng_s<<13; rng_s^=rng_s>>7; rng_s^=rng_s<<17; rnd[i]=(u8)rng_s; }
    sp-=16; u64 rnd_va=sp; if(ucopy_to(pr,sp,rnd,16)) return -1;
    u32 ca,cb,cc,cd; __asm__ volatile("cpuid":"=a"(ca),"=b"(cb),"=c"(cc),"=d"(cd):"a"(1));
    u64 aux[][2]={{3,phdr},{4,sizeof(Phdr)},{5,(u64)phnum},{6,4096},{7,at_base},{8,0},{9,entry},{11,0},{12,0},{13,0},{14,0},{15,plat_va},{16,cd},{17,100},{23,0},{25,rnd_va},{31,execfn_va},{0,0}};
    int na=(int)(sizeof aux/sizeof aux[0]);
    u64 words=1+(u64)argc+1+(u64)envc+1+(u64)na*2;
    sp=(sp-words*8)&~15ull; u64 q=sp; u64 v;
    v=(u64)argc; ucopy_to(pr,q,&v,8); q+=8;
    for(int i=0;i<argc;i++){ ucopy_to(pr,q,&argv_va[i],8); q+=8; } v=0; ucopy_to(pr,q,&v,8); q+=8;
    for(int i=0;i<envc;i++){ ucopy_to(pr,q,&env_va[i],8); q+=8; } v=0; ucopy_to(pr,q,&v,8); q+=8;
    for(int i=0;i<na;i++){ ucopy_to(pr,q,&aux[i][0],8); q+=8; ucopy_to(pr,q,&aux[i][1],8); q+=8; }
    *sp_out=sp; return 0;
}

struct builtin { const char *name; char *s, *e; };
static struct builtin builtins[]={{"hola",blob_hola_start,blob_hola_end}};
void proc_list(void){ kprintf("Programas incluidos en el kernel (ejecutar /bin/<nombre>):\n"); for(unsigned i=0;i<sizeof builtins/sizeof *builtins;i++) kprintf("  /bin/%s  (%lu bytes)\n",builtins[i].name,(u64)(builtins[i].e-builtins[i].s)); }

static const char *DEFENV[]={"PATH=/bin:/usr/bin","HOME=/","TERM=linux","LANG=C"};
static int find_image(const char *path,const u8 **img,u64 *sz){
    if(k_startswith(path,"/bin/")) for(unsigned i=0;i<sizeof builtins/sizeof *builtins;i++) if(!k_strcmp(path+5,builtins[i].name)){ *img=(u8*)builtins[i].s; *sz=(u64)(builtins[i].e-builtins[i].s); return 0; }
    if(!fs_root()) fs_init();
    FsNode *f=fs_resolve(fs_root(),path); if(!f||f->dir||!f->data) return -1;
    *img=(u8*)f->data; *sz=(u64)f->size; return 0;
}
/* arma un espacio de direcciones nuevo con el programa cargado; 0 ok */
static int load_image(Proc *pr,const char *path,int argc,char **argv,int envc,char **envp,u64 *entry,u64 *sp){
    const u8 *img; u64 sz;
    if(find_image(path,&img,&sz)){ kprintf("ejecutar: no existe %s\n",path); return -ENOENT; }
    if(proc_space(pr)){ kprintf("ejecutar: sin memoria\n"); return -ENOMEM; }
    struct elfinfo m,in;
    if(elf_map(pr,img,sz,USER_BASE,&m,1)) return -ENOEXEC;
    *entry=m.entry; u64 at_base=0;
    if(m.interp[0]){
        FsNode *f=fs_resolve(fs_root(),m.interp);
        if(!f||f->dir||!f->data){ kprintf("ejecutar: falta el enlazador dinamico %s (un programa dinamico necesita el sistema de archivos de Linux: rootfs)\n",m.interp); return -ENOENT; }
        if(elf_map(pr,(u8*)f->data,(u64)f->size,INTERP_BASE,&in,0)) return -ENOEXEC;
        *entry=in.entry; at_base=in.base;
    }
    if(!envp){ envp=(char**)DEFENV; envc=4; }
    if(build_stack(pr,sp,argc,argv,envc,envp,path,m.entry,m.phdr,m.phnum,at_base)) return -ENOMEM;
    pr->brk0=pr->brk=(m.end+4095+65536)&~4095ull; pr->mmap_next=MMAP_BASE;
    return 0;
}
static Proc *proc_new(void){ Proc *p=kmalloc(sizeof *p); if(p){ memset(p,0,sizeof *p); k_strcpy(p->cwd,"/",sizeof p->cwd); } return p; }

/* API para el shell del kernel: crea el proceso y (si no es en segundo plano) espera a que termine */
int proc_exec(char *path,int argc,char **argv,int bg){
    if(proc_init()){ kprintf("ejecutar: no hay memoria\n"); return -1; }
    Proc *pr=proc_new(); if(!pr){ kprintf("ejecutar: sin memoria\n"); return -1; }
    u64 entry,sp;
    if(load_image(pr,path,argc,argv,0,0,&entry,&sp)){ if(pr->slab_n) pg_free_all(pr); kfree(pr); return -1; }
    for(int i=0;i<3;i++) pr->fd[i].type=1;
    Task *t=task_alloc(); if(!t){ kprintf("ejecutar: demasiadas tareas\n"); pg_free_all(pr); kfree(pr); return -1; }
    t->pr=pr; t->pid=t->tid; pr->pid=t->pid; pr->nthreads=1; t->ppid=0; t->leader=1; t->bg=bg; t->fg=!bg;
    k_strcpy(t->name,path+(k_startswith(path,"/bin/")?5:0),sizeof t->name);
    struct regs fr; memset(&fr,0,sizeof fr); fr.rip=entry; fr.cs=0x23; fr.ss=0x1B; fr.rflags=0x202; fr.rsp=sp;
    inl_n=inl_r=inl_ready=0;
    task_prepare(t,&fr); t->state=T_READY;
    if(bg){ kprintf("[proceso %d en segundo plano]\n",t->pid); return t->pid; }
    for(;;){
        poll_wakeups();
        if(t->state==T_ZOMBIE) break;
        if(any_ready_user()) schedule(); else __asm__ volatile("sti;hlt;cli");
        if(idle_hook) idle_hook();
    }
    int code=t->exit_code; t->state=T_FREE;
    wrmsr(0xC0000100,0); __asm__ volatile("sti");
    return code;
}
void proc_ps(void){
    kprintf("  PID  PPID  ESTADO     NOMBRE\n");
    static const char *st[]={"libre","listo","dormido","espera","entrada","futex","pipe","zombie"};
    for(int i=0;i<MAXT;i++) if(tasks[i].state!=T_FREE) kprintf("  %-4d %-5d %-10s %s%s\n",tasks[i].tid,tasks[i].ppid,st[tasks[i].state],tasks[i].name,tasks[i].leader?"":" (hilo)");
}
int proc_kill(int pid,int sig){
    int n=0; for(int i=0;i<MAXT;i++) if(tasks[i].state!=T_FREE&&tasks[i].state!=T_ZOMBIE&&tasks[i].pid==pid){ tasks[i].killed=sig?sig:9; n++; }
    return n;
}

/* ---------- llamadas al sistema (numeros y estructuras = ABI de Linux x86-64) ---------- */
#define CP (cur->pr)
static int fd_alloc(Proc *pr,int from){ for(int f=from;f<MAXFD;f++) if(!pr->fd[f].type) return f; return -1; }
static int abs_path(const char *path,char *full,int max){
    int n=0; if(path[0]!='/'){ k_strcpy(full,CP->cwd,max); n=k_strlen(full); if(n&&full[n-1]!='/') full[n++]='/'; }
    int l=k_strlen(path); if(n+l>=max) return -1; memcpy(full+n,path,(size_t)l+1); return 0;
}
static FsNode *resolve_path(const char *path){ char full[200]; if(abs_path(path,full,sizeof full)) return 0; return fs_resolve(fs_root(),full); }
static FsNode *resolve_parent(const char *path,char *leaf){      /* carpeta contenedora y nombre final */
    char full[200]; if(abs_path(path,full,sizeof full)) return 0;
    int e=k_strlen(full); while(e>1&&full[e-1]=='/') full[--e]=0;
    int s2=e; while(s2>0&&full[s2-1]!='/') s2--;
    if(e-s2<=0||e-s2>=32) return 0;
    memcpy(leaf,full+s2,(size_t)(e-s2)); leaf[e-s2]=0;
    if(s2<=1) full[1]=0; else full[s2-1]=0;
    return fs_resolve(fs_root(),full);
}
static u64 sys_open(u64 path_va,u64 flags){
    char path[160]; if(ustr(CP,path,sizeof path,path_va)) return RET_ERR(EFAULT);
    FsNode *n=resolve_path(path);
    if(!n&&(flags&0x40)){ char leaf[32]; FsNode *d=resolve_parent(path,leaf); if(d&&d->dir) n=fs_create(d,leaf,"",0); }
    if(!n) return RET_ERR(ENOENT);
    if((flags&0x200)&&!n->dir) fs_write(n,"",0);
    int f=fd_alloc(CP,3); if(f<0) return RET_ERR(24);
    CP->fd[f].type=2; CP->fd[f].node=n; CP->fd[f].pos=0; CP->fd[f].flags=(int)flags; return (u64)f;
}
static u64 fd_read(u64 fdn,u64 buf,u64 n){
    if(fdn>=MAXFD||!CP->fd[fdn].type) return RET_ERR(EBADF);
    FD *f=&CP->fd[fdn];
    if(f->type==1){
        if(fdn!=0) return RET_ERR(EBADF);
        if(!n) return 0;
        for(;;){
            if(inl_ready&&inl_r<inl_n){ u64 k=(u64)(inl_n-inl_r); if(k>n) k=n; if(ucopy_to(CP,buf,inl+inl_r,k)) return RET_ERR(EFAULT); inl_r+=(int)k; if(inl_r>=inl_n){ inl_ready=0; inl_n=inl_r=0; } return k; }
            if(cur->killed) return RET_ERR(EINTR);
            block(T_READ);
        }
    }
    if(f->type==2){
        FsNode *nd=f->node; if(nd->dir) return RET_ERR(EISDIR);
        u64 left=f->pos<(u64)nd->size?(u64)nd->size-f->pos:0; if(n>left) n=left;
        if(n&&ucopy_to(CP,buf,nd->data+f->pos,n)) return RET_ERR(EFAULT);
        f->pos+=n; return n;
    }
    if(f->type==3){
        struct Pipe *p=f->pipe;
        for(;;){
            if(p->count>0){ u64 k=0; u8 tmp[512]; while(k<n&&p->count>0){ u64 c=n-k; if(c>sizeof tmp) c=sizeof tmp; u64 j=0; while(j<c&&p->count>0){ tmp[j++]=p->buf[p->r]; p->r=(p->r+1)%4096; p->count--; }
                    if(ucopy_to(CP,buf+k,tmp,j)){ return RET_ERR(EFAULT); }
                    k+=j; } return k; }
            if(p->writers<=0) return 0;
            if(cur->killed) return RET_ERR(EINTR);
            cur->pipe_fd=(int)fdn; block(T_PIPE);
        }
    }
    return RET_ERR(EBADF);
}
static u64 fd_write(u64 fdn,u64 buf,u64 n){
    if(fdn>=MAXFD||!CP->fd[fdn].type) return RET_ERR(EBADF);
    FD *f=&CP->fd[fdn]; u8 tmp[1024]; u64 done=0;
    if(f->type==1){
        if(fdn==0) return RET_ERR(EBADF);
        while(done<n){ u64 c=n-done; if(c>sizeof tmp) c=sizeof tmp; if(ucopy_from(CP,tmp,buf+done,c)) return done?done:RET_ERR(EFAULT); k_write_console((char*)tmp,(int)c); done+=c; }
        return n;
    }
    if(f->type==2){
        FsNode *nd=f->node; if(nd->dir) return RET_ERR(EISDIR);
        if(f->flags&0x400) f->pos=(u64)nd->size;
        u64 end=f->pos+n; u64 nsz=end>(u64)nd->size?end:(u64)nd->size; if(nsz>(64ull<<20)) return RET_ERR(ENOMEM);
        char *nb=kmalloc(nsz?nsz:1); if(!nb) return RET_ERR(ENOMEM);
        memset(nb,0,nsz); if(nd->size) memcpy(nb,nd->data,(size_t)nd->size);
        if(n&&ucopy_from(CP,nb+f->pos,buf,n)){ kfree(nb); return RET_ERR(EFAULT); }
        fs_write(nd,nb,(int)nsz); kfree(nb); f->pos=end; return n;
    }
    if(f->type==4){
        struct Pipe *p=f->pipe;
        while(done<n){
            if(p->readers<=0) return done?done:RET_ERR(EPIPE);
            if(p->count>=4096){ if(cur->killed) return done?done:RET_ERR(EINTR); cur->pipe_fd=(int)fdn; block(T_PIPE); continue; }
            u64 c=n-done; if(c>sizeof tmp) c=sizeof tmp; if(c>(u64)(4096-p->count)) c=(u64)(4096-p->count);
            if(ucopy_from(CP,tmp,buf+done,c)) return done?done:RET_ERR(EFAULT);
            for(u64 j=0;j<c;j++){ p->buf[p->w]=tmp[j]; p->w=(p->w+1)%4096; p->count++; } done+=c;
        }
        return n;
    }
    return RET_ERR(EBADF);
}
static u64 sys_mmap(u64 addr,u64 len,u64 prot,u64 flags,u64 fdn,u64 off){
    if(!len) return RET_ERR(EINVAL);
    len=(len+4095)&~4095ull;
    u64 va;
    if((flags&0x10) && addr>=USER_MIN && addr+len<=USER_MAX && !(addr&0xFFF)) va=addr;
    else { va=CP->mmap_next; if(va+len>=STACK_TOP-STACK_SIZE-(1ull<<20)) return RET_ERR(ENOMEM); CP->mmap_next+=len+4096; }
    if(prot!=0){                                  /* PROT_NONE solo reserva direcciones (se mapea en mprotect) */
        if(len>(256ull<<20)) return RET_ERR(ENOMEM);
        if(umap_range(CP,va,len)) return RET_ERR(ENOMEM);
        if(flags&0x10) for(u64 q=va;q<va+len;q+=4096){ u8 *pg=uptr(CP,q); if(pg) memset(pg,0,4096); }   /* MAP_FIXED encima de algo: empieza en cero */
        if(!(flags&0x20) && (long)fdn>=0){        /* mapeo de archivo: copia privada */
            if(fdn>=MAXFD||CP->fd[fdn].type!=2) return RET_ERR(EBADF);
            FsNode *f=CP->fd[fdn].node; if(off<(u64)f->size){ u64 n=(u64)f->size-off; if(n>len) n=len; ucopy_to(CP,va,f->data+off,n); }
        }
    }
    return va;
}
static u64 sys_brk(u64 want){
    if(want<CP->brk0||want>=CP->brk0+(256ull<<20)) return CP->brk;
    if(want>CP->brk){ if(umap_range(CP,CP->brk,want-CP->brk)) return CP->brk; }
    CP->brk=want; return CP->brk;
}
static const char *UN[6]={"Linux","kng","6.1.0-kng","kng-compat 1","x86_64","(none)"};

static u64 sys_clone(struct regs *r,u64 flags,u64 stack,u64 ptid,u64 ctid,u64 tls){
    int thread=(flags&0x10000)!=0;
    Task *t=task_alloc(); if(!t) return RET_ERR(EAGAIN);
    Proc *pr=CP;
    if(!thread){
        Proc *np=proc_new(); if(!np){ return RET_ERR(ENOMEM); }
        if(proc_space(np)||as_clone(np,pr)){ pg_free_all(np); kfree(np); return RET_ERR(ENOMEM); }
        np->brk=pr->brk; np->brk0=pr->brk0; np->mmap_next=pr->mmap_next; memcpy(np->sighand,pr->sighand,sizeof np->sighand); memcpy(np->cwd,pr->cwd,sizeof np->cwd);
        for(int i=0;i<MAXFD;i++) if(pr->fd[i].type) fd_dup_into(&np->fd[i],&pr->fd[i]);
        np->pid=t->tid; np->nthreads=1; t->pr=np; t->pid=t->tid; t->ppid=cur->pid; t->leader=1;
    } else { pr->nthreads++; t->pr=pr; t->pid=cur->pid; t->ppid=cur->ppid; t->leader=0; }
    t->fs_base=(flags&0x80000)?tls:cur->fs_base; memcpy(t->name,cur->name,sizeof t->name);
    struct regs fr=*r; fr.rax=0; if(stack) fr.rsp=stack; fr.rflags|=0x200;
    task_prepare(t,&fr);
    u32 tid=(u32)t->tid;
    if(flags&0x100000) ucopy_to(pr,ptid,&tid,4);
    if(flags&0x1000000) ucopy_to(t->pr,ctid,&tid,4);
    if(flags&0x200000) t->clear_tid=ctid;
    t->state=T_READY; return (u64)t->tid;
}
static u64 sys_execve(struct regs *r,u64 path_va,u64 argv_va,u64 envp_va){
    static char path[160], kargv[MAXARG][160], kenv[MAXARG][160]; static char *pa[MAXARG], *pe[MAXARG]; int argc=0, envc=0;
    if(ustr(CP,path,sizeof path,path_va)) return RET_ERR(EFAULT);
    for(; argv_va && argc<MAXARG; argc++){ u64 p=0; if(ucopy_from(CP,&p,argv_va+(u64)argc*8,8)) return RET_ERR(EFAULT); if(!p) break; if(ustr(CP,kargv[argc],160,p)) return RET_ERR(EFAULT); pa[argc]=kargv[argc]; }
    for(; envp_va && envc<MAXARG; envc++){ u64 p=0; if(ucopy_from(CP,&p,envp_va+(u64)envc*8,8)) return RET_ERR(EFAULT); if(!p) break; if(ustr(CP,kenv[envc],160,p)) return RET_ERR(EFAULT); pe[envc]=kenv[envc]; }
    if(!argc){ pa[0]=path; argc=1; }
    Proc *np=proc_new(); if(!np) return RET_ERR(ENOMEM); u64 entry,sp;
    int rc=load_image(np,path,argc,pa,envc,envc?pe:0,&entry,&sp);
    if(rc){ if(np->slab_n) pg_free_all(np); kfree(np); return RET_ERR(-rc); }
    Proc *op=CP; memcpy(np->fd,op->fd,sizeof np->fd); memcpy(np->cwd,op->cwd,sizeof np->cwd); np->pid=op->pid; np->nthreads=op->nthreads;
    memset(op->fd,0,sizeof op->fd); pg_free_all(op);
    *op=*np; kfree(np);
    ucr3=(u64)(uintptr_t)op->pml4; cur->fs_base=0; wrmsr(0xC0000100,0); cur->clear_tid=0;
    memset(r,0,22*8); r->rip=entry; r->cs=0x23; r->ss=0x1B; r->rflags=0x202; r->rsp=sp; r->int_no=0x80;
    k_strcpy(cur->name,path+(k_startswith(path,"/bin/")?5:0),sizeof cur->name);
    return EXEC_DONE;
}
static u64 sys_wait4(u64 pid,u64 status_va,u64 options){
    for(;;){
        int have=0;
        for(int i=0;i<MAXT;i++){ Task *c=&tasks[i]; if(c->state==T_FREE||!c->leader||c->ppid!=cur->pid) continue;
            if((long)pid>0&&c->pid!=(int)pid) continue;
            have=1;
            if(c->state==T_ZOMBIE){
                int code=c->exit_code, rv=c->pid; u32 st=code<0?(u32)(-code):(((u32)code&0xFF)<<8);
                if(status_va) ucopy_to(CP,status_va,&st,4);
                c->state=T_FREE; return (u64)rv; } }
        if(!have) return RET_ERR(ECHILD);
        if(options&1) return 0;
        if(cur->killed) return RET_ERR(EINTR);
        cur->wait_pid=(int)(long)pid; block(T_WAIT);
    }
}
static u64 sys_futex(u64 uaddr,u64 op,u64 val,u64 timeout_va){
    int cmd=(int)(op&0x7F);
    if(cmd==0||cmd==9){                                  /* WAIT / WAIT_BITSET */
        u32 cv; if(ucopy_from(CP,&cv,uaddr,4)) return RET_ERR(EFAULT);
        if(cv!=(u32)val) return RET_ERR(EAGAIN);
        cur->fut_key=(u64)(uintptr_t)CP+uaddr; cur->wake_at=0; cur->timedout=0;
        if(timeout_va){ u64 t[2]; if(ucopy_from(CP,t,timeout_va,16)) return RET_ERR(EFAULT); u32 tk=(u32)(t[0]*100+t[1]/10000000ull); if(!tk) tk=1; cur->wake_at=(u64)(kticks()+tk); }
        if(cur->killed) return RET_ERR(EINTR);
        block(T_FUTEX);
        if(cur->timedout){ cur->timedout=0; return RET_ERR(ETIMEDOUT); }
        return 0;
    }
    if(cmd==1||cmd==10){ int w=0; futex_wake(CP,uaddr,(int)(val>0x7fffffff?0x7fffffff:val),&w); return (u64)w; }
    if(cmd==3||cmd==4){ int w=0; futex_wake(CP,uaddr,0x7fffffff,&w); return (u64)w; }      /* REQUEUE: se despierta a todos (aproximacion) */
    return RET_ERR(ENOSYS);
}
static int sig_default_ignored(int s){ return s==17||s==23||s==28||s==18||s==0; }
static u64 sys_kill(int pid,int sig,int tid_only,int tid){
    if(sig<0||sig>64) return RET_ERR(EINVAL);
    int found=0;
    for(int i=0;i<MAXT;i++){ Task *t=&tasks[i]; if(t->state==T_FREE||t->state==T_ZOMBIE) continue;
        if(tid_only?(t->tid!=tid):(t->pid!=pid)) continue;
        found=1;
        if(!sig) continue;
        u64 h=t->pr?t->pr->sighand[sig]:0;
        if(sig_default_ignored(sig)||h==1) continue;           /* ignorada */
        if(h>1&&sig!=9&&sig!=19) continue;                     /* manejador del usuario: aun no se entrega */
        for(int j=0;j<MAXT;j++) if(tasks[j].pr==t->pr&&tasks[j].state!=T_FREE) tasks[j].killed=sig;
    }
    return found?0:RET_ERR(ESRCH);
}

static u64 sys_dispatch(struct regs *r){
    u64 nr=r->rax,a1=r->rdi,a2=r->rsi,a3=r->rdx,a4=r->r10,a5=r->r8,a6=r->r9;
    switch(nr){
    case 0:  return fd_read(a1,a2,a3);
    case 1:  return fd_write(a1,a2,a3);
    case 2:  return sys_open(a1,a2);
    case 257:return sys_open(a2,a3);
    case 3:  if(a1<MAXFD&&CP->fd[a1].type){ fd_close(&CP->fd[a1]); return 0; } return RET_ERR(EBADF);
    case 8:  { if(a1>=MAXFD||CP->fd[a1].type!=2) return RET_ERR(EBADF); FD *f=&CP->fd[a1]; u64 sz=(u64)f->node->size; long o=(long)a2; u64 np=a3==0?(u64)o:a3==1?f->pos+(u64)o:sz+(u64)o; f->pos=np; return np; }
    case 4: case 5: case 6: case 262: {                       /* stat / fstat / lstat / newfstatat */
        u64 sbuf=nr==262?a3:a2; u8 st[144]; memset(st,0,sizeof st); u32 mode=0x2190; u64 size=0, ino=1+a1; FsNode *sn=0;
        if(nr==5){ if(a1>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF); if(CP->fd[a1].type==2) sn=CP->fd[a1].node; else if(CP->fd[a1].type>=3) mode=010600; }
        else { u64 pv=nr==262?a2:a1; char path[160]; if(ustr(CP,path,sizeof path,pv)) return RET_ERR(EFAULT);
               if(nr==262 && !path[0] && (a4&0x1000)){ if(a1>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF); if(CP->fd[a1].type==2) sn=CP->fd[a1].node; }
               else { FsNode *n=resolve_path(path); if(!n) return RET_ERR(ENOENT); sn=n; } }
        if(sn){ mode=sn->dir?040755:0100755; size=(u64)sn->size; ino=(u64)(uintptr_t)sn>>4; }       /* st_dev/st_ino distintos por archivo: ld.so los usa para saber si una libreria ya esta cargada */
        *(u64*)(st+0)=1; *(u64*)(st+8)=ino; *(u64*)(st+16)=1; *(u32*)(st+24)=mode; *(u64*)(st+48)=size; *(u64*)(st+56)=4096;
        return ucopy_to(CP,sbuf,st,sizeof st)?RET_ERR(EFAULT):0; }
    case 9:  return sys_mmap(a1,a2,a3,a4,a5,a6);
    case 10: if(a3&7){ if(umap_range(CP,a1,a2)) return RET_ERR(ENOMEM); } return 0;
    case 11: case 28: return 0;                                 /* munmap/madvise: de momento no devuelven memoria */
    case 12: return sys_brk(a1);
    case 13: { if(a1>64||a1==9||a1==19) return RET_ERR(EINVAL); u64 old=CP->sighand[a1]; if(a3&&ucopy_to(CP,a3,&old,8)) return RET_ERR(EFAULT);
               if(a2){ u64 h; if(ucopy_from(CP,&h,a2,8)) return RET_ERR(EFAULT); CP->sighand[a1]=h; } return 0; }
    case 14: case 131: { if(nr==14&&a3){ u64 z=0; ucopy_to(CP,a3,&z,8); } return 0; }
    case 16: if((a2==0x5413)&&CP->fd[a1<MAXFD?a1:0].type==1){ u16 ws[4]={25,80,0,0}; return ucopy_to(CP,a3,ws,8)?RET_ERR(EFAULT):0; } return RET_ERR(ENOTTY);
    case 17: { if(a1>=MAXFD||CP->fd[a1].type!=2) return RET_ERR(EBADF); FsNode *f=CP->fd[a1].node; u64 left=a4<(u64)f->size?(u64)f->size-a4:0; if(a3>left) a3=left; if(a3&&ucopy_to(CP,a2,f->data+a4,a3)) return RET_ERR(EFAULT); return a3; }
    case 20: { u64 tot=0; for(u64 i=0;i<a3&&i<64;i++){ u64 iov[2]; if(ucopy_from(CP,iov,a2+i*16,16)) return RET_ERR(EFAULT); long rv=(long)fd_write(a1,iov[0],iov[1]); if(rv<0) return tot?tot:(u64)rv; tot+=(u64)rv; } return tot; }
    case 19: { u64 tot=0; for(u64 i=0;i<a3&&i<64;i++){ u64 iov[2]; if(ucopy_from(CP,iov,a2+i*16,16)) return RET_ERR(EFAULT); long rv=(long)fd_read(a1,iov[0],iov[1]); if(rv<0) return tot?tot:(u64)rv; tot+=(u64)rv; if((u64)rv<iov[1]) break; } return tot; }
    case 21: case 269: { char path[160]; if(ustr(CP,path,sizeof path,nr==21?a1:a2)) return RET_ERR(EFAULT); return resolve_path(path)?0:RET_ERR(ENOENT); }
    case 89: case 267: return RET_ERR(ENOENT);
    case 22: case 293: {                                      /* pipe / pipe2 */
        int f0=fd_alloc(CP,3); if(f0<0) return RET_ERR(EMFILE); CP->fd[f0].type=9; int f1=fd_alloc(CP,3); CP->fd[f0].type=0; if(f1<0) return RET_ERR(EMFILE);
        struct Pipe *p=kmalloc(sizeof *p); if(!p) return RET_ERR(ENOMEM); memset(p,0,sizeof *p); p->readers=1; p->writers=1;
        CP->fd[f0].type=3; CP->fd[f0].pipe=p; CP->fd[f1].type=4; CP->fd[f1].pipe=p;
        int fds[2]={f0,f1}; return ucopy_to(CP,a1,fds,8)?RET_ERR(EFAULT):0; }
    case 32: { if(a1>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF); int n=fd_alloc(CP,0); if(n<0) return RET_ERR(EMFILE); fd_dup_into(&CP->fd[n],&CP->fd[a1]); return (u64)n; }
    case 33: case 292: { if(a1>=MAXFD||a2>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF); if(a1==a2) return a2; if(CP->fd[a2].type) fd_close(&CP->fd[a2]); fd_dup_into(&CP->fd[a2],&CP->fd[a1]); return a2; }
    case 72: { if(a1>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF);
        if(a2==0||a2==1030){ int n=fd_alloc(CP,(int)a3); if(n<0) return RET_ERR(EMFILE); fd_dup_into(&CP->fd[n],&CP->fd[a1]); return (u64)n; }
        if(a2==3){ return (u64)(CP->fd[a1].flags&~3)|2; }
        return 0; }
    case 24: if(any_ready_user()){ cur->state=T_READY; schedule(); } return 0;
    case 35: case 230: { u64 ts=nr==35?a1:a3; u64 t[2]; if(ucopy_from(CP,t,ts,16)) return RET_ERR(EFAULT); u32 tk=(u32)(t[0]*100+t[1]/10000000ull); cur->wake_at=(u64)(kticks()+(tk?tk:1)); if(cur->killed) return RET_ERR(EINTR); block(T_SLEEP); return 0; }
    case 39: return (u64)cur->pid;
    case 56: return sys_clone(r,a1,a2,a3,a4,a5);
    case 57: return sys_clone(r,0x11,0,0,0,0);                 /* fork */
    case 58: return sys_clone(r,0x4111,0,0,0,0);               /* vfork: se trata como fork (copia) */
    case 59: return sys_execve(r,a1,a2,a3);
    case 60: if(cur->leader||CP->nthreads<=1) do_exit_group((int)(a1&0xFF));
             { Task *me=cur; thread_exit_cleanup(me); CP->nthreads--; me->state=T_ZOMBIE; schedule(); for(;;) __asm__ volatile("hlt"); }
    case 231: do_exit_group((int)(a1&0xFF));
    case 61: return sys_wait4(a1,a2,a3);
    case 62: return sys_kill((int)a1,(int)a2,0,0);
    case 200: return sys_kill(0,(int)a2,1,(int)a1);
    case 234: return sys_kill(0,(int)a3,1,(int)a2);
    case 63: { u8 b[390]; memset(b,0,sizeof b); for(int i=0;i<6;i++) memcpy(b+i*65,UN[i],(size_t)k_strlen(UN[i])); return ucopy_to(CP,a1,b,sizeof b)?RET_ERR(EFAULT):0; }
    case 79: { int l=k_strlen(CP->cwd)+1; if((u64)l>a2) return RET_ERR(ERANGE); return ucopy_to(CP,a1,CP->cwd,(u64)l)?RET_ERR(EFAULT):(u64)l; }
    case 80: { char path[160]; if(ustr(CP,path,sizeof path,a1)) return RET_ERR(EFAULT); FsNode *n=resolve_path(path); if(!n) return RET_ERR(ENOENT); if(!n->dir) return RET_ERR(ENOTDIR); fs_path(n,CP->cwd,sizeof CP->cwd); return 0; }
    case 82: { char o[160],nw[160],leaf[32]; if(ustr(CP,o,sizeof o,a1)||ustr(CP,nw,sizeof nw,a2)) return RET_ERR(EFAULT);
        FsNode *n=resolve_path(o); if(!n||!n->parent) return RET_ERR(ENOENT); FsNode *d=resolve_parent(nw,leaf); if(!d) return RET_ERR(ENOENT);
        if(fs_find(d,leaf)) fs_remove(fs_find(d,leaf));
        return (d==n->parent?fs_rename(n,leaf):fs_move(n,d,leaf))?RET_ERR(EINVAL):0; }
    case 83: { char p[160],leaf[32]; if(ustr(CP,p,sizeof p,a1)) return RET_ERR(EFAULT); FsNode *d=resolve_parent(p,leaf); if(!d) return RET_ERR(ENOENT); if(fs_find(d,leaf)) return RET_ERR(EEXIST); return fs_mkdir(d,leaf)?0:RET_ERR(ENOMEM); }
    case 84: case 87: { char p[160]; if(ustr(CP,p,sizeof p,a1)) return RET_ERR(EFAULT); FsNode *n=resolve_path(p); if(!n||!n->parent) return RET_ERR(ENOENT); return fs_remove(n)?RET_ERR(EINVAL):0; }
    case 217: { if(a1>=MAXFD||CP->fd[a1].type!=2||!CP->fd[a1].node->dir) return RET_ERR(EBADF);
        FD *f=&CP->fd[a1]; FsNode *l[96]; int cnt=fs_list(f->node,l,96); u64 off=0; u8 buf[4096]; if(a3>sizeof buf) a3=sizeof buf;
        while((int)f->pos<cnt){ FsNode *c=l[f->pos]; int nl=k_strlen(c->name); u64 rl=(u64)((19+nl+1+7)&~7); if(off+rl>a3) break;
            u8 *e=buf+off; memset(e,0,rl); *(u64*)e=(u64)(uintptr_t)c>>4; *(u64*)(e+8)=f->pos+1; *(u16*)(e+16)=(u16)rl; e[18]=c->dir?4:8; memcpy(e+19,c->name,(size_t)nl); off+=rl; f->pos++; }
        if(off&&ucopy_to(CP,a2,buf,off)){ return RET_ERR(EFAULT); }
        return off; }
    case 96: { u64 t[2]; u32 k=kticks(); t[0]=k/100; t[1]=(u64)(k%100)*10000; return a1&&ucopy_to(CP,a1,t,16)?RET_ERR(EFAULT):0; }
    case 99: { u8 si[112]; memset(si,0,sizeof si); *(u64*)si=kticks()/100; *(u64*)(si+32)=256ull<<20; *(u64*)(si+40)=128ull<<20; *(u32*)(si+104)=1; return ucopy_to(CP,a1,si,sizeof si)?RET_ERR(EFAULT):0; }
    case 102: case 104: case 107: case 108: case 112: case 105: case 106: return 0;
    case 110: return (u64)cur->ppid;
    case 109: case 121: case 124: return (u64)cur->pid;
    case 158: if(a1==0x1002){ cur->fs_base=a2; wrmsr(0xC0000100,a2); return 0; } if(a1==0x1003){ u64 v=rdmsr(0xC0000100); return ucopy_to(CP,a2,&v,8)?RET_ERR(EFAULT):0; } return RET_ERR(EINVAL);
    case 186: return (u64)cur->tid;
    case 204: { u64 m=1; return ucopy_to(CP,a3,&m,8)?RET_ERR(EFAULT):8; }
    case 203: return 0;
    case 218: cur->clear_tid=a1; return (u64)cur->tid;
    case 202: return sys_futex(a1,a2,a3,a4);
    case 228: { u64 t[2]; u32 k=kticks(); t[0]=k/100; t[1]=(u64)(k%100)*10000000ull; return ucopy_to(CP,a2,t,16)?RET_ERR(EFAULT):0; }
    case 273: return 0;
    case 302: { if(a4){ u64 rl[2]={8ull<<20,~0ull}; if(ucopy_to(CP,a4,rl,16)) return RET_ERR(EFAULT); } return 0; }
    case 318: { static u64 rs=0x9E3779B97F4A7C15ull; u8 b[256]; u64 n=a2>256?256:a2; rs^=kticks(); for(u64 i=0;i<n;i++){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; b[i]=(u8)rs; } return ucopy_to(CP,a1,b,n)?RET_ERR(EFAULT):n; }
    case 334: return RET_ERR(ENOSYS);
    default:
        kprintf("[syscall] numero %lu aun no implementado -> ENOSYS\n",nr); return RET_ERR(ENOSYS);
    }
}

/* ---------- punto de entrada desde ring 3 (lo llama proc64.S con la CR3 del kernel) ---------- */
void kernel_entry(struct regs *r){
    cur->frame=r;
    if(r->int_no==0x80){
        u64 rv=sys_dispatch(r);
        if(rv!=EXEC_DONE) r->rax=rv;
    } else if(r->int_no<32){
        u64 cr2=0; if(r->int_no==14) __asm__ volatile("mov %%cr2,%0":"=r"(cr2));
        kprintf("\n[proceso %d] excepcion %lu en el programa: rip=0x%lx err=0x%lx cr2=0x%lx -> terminado\n",cur->pid,r->int_no,r->rip,r->err,cr2);
        do_exit_group(r->int_no==14||r->int_no==13?-11:r->int_no==6?-4:r->int_no==0?-8:-6);
    } else {
        isr_handler64(r);
        if(r->int_no==32){ static int q; if(++q>=2){ q=0; Task *n=pick_next(); if(n!=cur){ poll_wakeups(); run_task(pick_next()); } } }
    }
    r->rflags|=0x200;
    if(cur->killed) do_exit_group(-cur->killed);
}

/* ---------- rootfs (archivo cpio "newc" entregado como modulo Multiboot: -initrd en QEMU, "module" en GRUB) ---------- */
static u32 hex8(const u8 *p){ u32 v=0; for(int i=0;i<8;i++){ u8 c=p[i]; v=v*16+(u32)(c<='9'?c-'0':(c|32)-'a'+10); } return v; }
static FsNode *mkpath(const char *path,int want_parent_only,const char **leaf){
    FsNode *cur2=fs_root(); char part[32]; const char *p=path;
    for(;;){
        while(*p=='/') p++;
        int n=0; while(p[n]&&p[n]!='/') n++;
        if(!n) return cur2;
        if(n>=32) return 0;
        memcpy(part,p,(size_t)n); part[n]=0;
        int last=p[n]==0;
        if(last&&want_parent_only){ *leaf=p; return cur2; }
        FsNode *c=fs_find(cur2,part);
        if(!c){ c=fs_mkdir(cur2,part); if(!c) return 0; c->sys=1; }
        if(!c->dir) return 0;
        cur2=c; p+=n;
    }
}
void rootfs_load(u32 mods_n,u32 mods_addr){
    struct mod { u32 s,e,c,pad; } *m=(struct mod*)(uintptr_t)mods_addr; int files=0; u64 bytes=0;
    for(u32 i=0;i<mods_n;i++){
        const u8 *p=(const u8*)(uintptr_t)m[i].s, *end=(const u8*)(uintptr_t)m[i].e;
        while(p+110<=end && p[0]=='0'&&p[1]=='7'&&p[2]=='0'&&p[3]=='7'&&p[4]=='0'&&p[5]=='1'){
            u32 mode=hex8(p+14), fsz=hex8(p+54), nsz=hex8(p+94);
            const char *name=(const char*)p+110; const u8 *data=(const u8*)(((uintptr_t)p+110+nsz+3)&~3ull); p=(const u8*)(((uintptr_t)data+fsz+3)&~3ull);
            if(!k_strcmp(name,"TRAILER!!!")) break;
            while(name[0]=='.'&&name[1]=='/'){ name+=2; }
            if(!k_strcmp(name,".")||!name[0]) continue;
            if((mode&0170000)==0040000){ mkpath(name,0,0); continue; }
            if((mode&0170000)!=0100000) continue;                       /* enlaces simbolicos y especiales: omitidos (el rootfs se arma con copias reales) */
            const char *leaf=0; FsNode *d=mkpath(name,1,&leaf); if(!d||!leaf||k_strlen(leaf)>=32) continue;
            FsNode *f=fs_create(d,leaf,(const char*)data,(int)fsz); if(f){ f->sys=1; files++; bytes+=fsz; }
        }
    }
    kprintf("[ok] rootfs: %d archivos (%lu KB) desde el modulo de arranque\n",files,bytes/1024);
}
