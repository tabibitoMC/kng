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
static uint32_t fs_now_fn(void);
static uint64_t wall_sec(void);
#include "sock.h"
#include "net.h"

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
#define STACK_SIZE (8*1024*1024)
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
static void dev_create(void);
static void fs_shm_release(void *o);

/* ---------- estructuras ---------- */
#define SLAB_MAX   2048                  /* bloques de paginas por proceso: los 4 primeros de 64 paginas, el resto de 256 (hasta ~2 GB) */
static inline int slab_cap(int idx){ return idx<4?64:256; }
#define MAXFD 512
#define MAXT  512
#define KSTK  32768
struct Pipe { u8 buf[4096]; int r,w,count,readers,writers; };
/* Objetos del kernel que se ven como descriptores (eventfd, epoll, timerfd, archivos generados como /proc) */
#define EP_MAX 256
typedef struct KObj {
    int refs, kind, flags; u64 cnt; u64 t_next, t_int;
    char *data; int size;
    u8 **tab, **traw; int ntab; u8 *contig, *craw; u64 ssize;           /* memoria compartida (kind 12): paginas por bloques o un solo bloque alineado */
    int nw; struct { int fd; u32 ev; u64 data; } w[EP_MAX];
} KObj;
typedef struct { u8 type, cloexec; FsNode *node; u64 pos; int flags; struct Pipe *pipe; int aux; KObj *obj; u32 tmo_r, tmo_s; } FD;
/* tipos: 0 libre, 1 consola, 2 archivo, 3 pipe (lectura), 4 pipe (escritura), 5 socket (aux = indice), 6 eventfd, 7 epoll, 8 timerfd,
 *        10 dispositivo (aux = numero), 11 archivo generado (obj->data), 12 memoria compartida (memfd) */
struct SigAct { u64 h, flags, restorer, mask; };
struct Vma { u64 s, e; u32 prot, kind; KObj *obj; FsNode *node; u64 off; };      /* region de memoria virtual de un proceso */
typedef struct Proc {
    struct Vma *vma; int nvma, cvma; u64 *freepg;
    u64 *pml4; void *slab_raw[SLAB_MAX]; u64 slab_base[SLAB_MAX]; int slab_n, slab_used;
    u64 brk, brk0, mmap_next; FD fd[MAXFD]; int nthreads, pid; struct SigAct sa[65]; char cwd[96]; char exe[96];
    u64 alarm_at, alarm_int; u32 umask; int pgid;
} Proc;
enum { T_FREE, T_READY, T_SLEEP, T_WAIT, T_READ, T_FUTEX, T_PIPE, T_ZOMBIE, T_FN };
typedef struct Task {
    int tid, pid, ppid, state, leader, killed, exit_code, wait_pid, bg, fg, timedout, pipe_fd; Proc *pr; u8 *kstk; u64 ksp; struct regs *frame;
    u64 wake_at, fut_key, clear_tid, fs_base; char name[24];
    u64 sigpend, sigmask, sigsaved; int has_saved, sysnr, in_sig; u64 alt_sp, alt_size;
    int (*wait_fn)(struct Task*); int wa_fd, wa_mask, wa_n; u64 wa_p1, wa_p2, wa_p3;
    u8 fx[512] __attribute__((aligned(16)));
} Task;
static Task tasks[MAXT]; static Task task0; static Task *cur=&task0; static int next_tid=1;
#define CP (cur->pr)
#define INTR() (cur->killed || (cur->sigpend&~cur->sigmask))
static void sig_queue_proc(Proc *pr,int sig);
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
    sock_init(); dev_create(); fs_shm_hook=fs_shm_release; fs_now=fs_now_fn;
    proc_ready=1; return 0;
}

/* ---------- paginas y tablas (por proceso) ---------- */
static void vma_release(Proc *pr);
static int vma_fault(Proc *pr,u64 va,int write);
static u64 *pg_new(Proc *pr){
    if(pr->freepg){ u64 *p=pr->freepg; pr->freepg=(u64*)(uintptr_t)p[0]; memset(p,0,4096); return p; }
    if(!pr->slab_n || pr->slab_used>=slab_cap(pr->slab_n-1)){
        if(pr->slab_n>=SLAB_MAX) return 0;
        void *raw=kmalloc((u64)(slab_cap(pr->slab_n)+1)*4096ull); if(!raw) return 0;
        pr->slab_raw[pr->slab_n]=raw; pr->slab_base[pr->slab_n]=((u64)(uintptr_t)raw+4095)&~4095ull; pr->slab_n++; pr->slab_used=0; }
    u64 *p=(u64*)(uintptr_t)(pr->slab_base[pr->slab_n-1]+(u64)pr->slab_used*4096); pr->slab_used++;
    memset(p,0,4096); return p;
}
static void pg_free_all(Proc *pr){ vma_release(pr); for(int i=0;i<pr->slab_n;i++) kfree(pr->slab_raw[i]); pr->slab_n=0; pr->slab_used=0; pr->pml4=0; }
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
    static const int sh[3]={39,30,21};
    if(!pr->pml4||va<USER_MIN||va>=USER_MAX) return 0;
    for(int tries=0;tries<2;tries++){
        u64 *t=pr->pml4; int ok=1;
        for(int l=0;l<3;l++){ u64 e=t[(va>>sh[l])&511]; if(!(e&1)){ ok=0; break; } t=(u64*)(uintptr_t)(e&~0xFFFull); }
        if(ok){ u64 e=t[(va>>12)&511]; if(e&1) return (u8*)(uintptr_t)((e&~0xFFFull)|(va&0xFFF)); }
        if(tries||vma_fault(pr,va,0)) return 0;          /* pagina aun sin crear: se crea ahora si cae en una region valida */
    }
    return 0;
}
/* memoria intermedia del nucleo con una direccion 'virtual' propia: permite a sendfile/copy_file_range reutilizar fd_read/fd_write sin pasar por el usuario */
#define KB_VA 0xFFFF900000000000ull
static u8 kbounce[65536];
static int ucopy_to(Proc *pr,u64 va,const void *src,u64 n){ const u8 *s=src; if(va>=KB_VA&&va-KB_VA<=sizeof kbounce&&n<=sizeof kbounce-(va-KB_VA)){ memcpy(kbounce+(va-KB_VA),src,n); return 0; } while(n){ u8 *d=uptr(pr,va); if(!d) return -1; u64 c=4096-(va&0xFFF); if(c>n) c=n; memcpy(d,s,c); va+=c; s+=c; n-=c; } return 0; }
static int ucopy_from(Proc *pr,void *dst,u64 va,u64 n){ u8 *o=dst; if(va>=KB_VA&&va-KB_VA<=sizeof kbounce&&n<=sizeof kbounce-(va-KB_VA)){ memcpy(dst,kbounce+(va-KB_VA),n); return 0; } while(n){ u8 *s=uptr(pr,va); if(!s) return -1; u64 c=4096-(va&0xFFF); if(c>n) c=n; memcpy(o,s,c); va+=c; o+=c; n-=c; } return 0; }
static int umap_range(Proc *pr,u64 va,u64 len){ for(u64 p=va&~0xFFFull;p<va+len;p+=4096) if(umap(pr,p)) return -1; return 0; }
static int ustr(Proc *pr,char *out,int max,u64 va){ for(int i=0;i<max-1;i++){ u8 *c=uptr(pr,va+(u64)i); if(!c) return -1; out[i]=(char)*c; if(!*c) return 0; } out[max-1]=0; return 0; }

static int umap_set(Proc *pr,u64 va,u64 phys,u64 fl);
static int as_clone(Proc *dst,Proc *src){      /* copia completa del espacio de usuario (fork) */
    for(int a=0;a<512;a++){ u64 e4=src->pml4[a]; if(!(e4&1)) continue; u64 *pdpt=(u64*)(uintptr_t)(e4&~0xFFFull);
      for(int b=0;b<512;b++){ u64 e3=pdpt[b]; if(!(e3&1)) continue; u64 *pdd=(u64*)(uintptr_t)(e3&~0xFFFull);
        for(int c=0;c<512;c++){ u64 e2=pdd[c]; if(!(e2&1)||(e2&0x80)) continue; u64 *pt=(u64*)(uintptr_t)(e2&~0xFFFull);
          for(int d=0;d<512;d++){ u64 e1=pt[d]; if(!(e1&1)) continue;
            u64 va=((u64)a<<39)|((u64)b<<30)|((u64)c<<21)|((u64)d<<12);
            if(va<USER_MIN) continue;
            if(e1&0x200){ if(umap_set(dst,va,e1&~0xFFFull,e1&0xFFF)) return -1; continue; }      /* pagina compartida: se enlaza, no se copia */
            if(umap(dst,va)) return -1;
            memcpy(uptr(dst,va),(void*)(uintptr_t)(e1&~0xFFFull),4096); } } } }
    if(src->nvma){                                              /* mismas regiones (con una referencia mas a sus objetos) */
        dst->vma=kmalloc((size_t)src->cvma*sizeof(struct Vma)); if(!dst->vma) return -1;
        memcpy(dst->vma,src->vma,(size_t)src->nvma*sizeof(struct Vma)); dst->nvma=src->nvma; dst->cvma=src->cvma;
        for(int i=0;i<dst->nvma;i++) if(dst->vma[i].obj) dst->vma[i].obj->refs++; }
    return 0;
}

/* ---------- tareas y planificador ---------- */
static Task *task_alloc(void){
    for(int i=0;i<MAXT;i++) if(tasks[i].state==T_FREE){
        Task *t=&tasks[i]; u8 *ks=t->kstk; memset(t,0,sizeof *t); t->kstk=ks;
        if(!t->kstk){ t->kstk=kmalloc(KSTK); if(!t->kstk) return 0; }
        t->fx[0]=0x7F; t->fx[1]=0x03; t->fx[24]=0x80; t->fx[25]=0x1F;          /* FCW=0x37F, MXCSR=0x1F80 */
        t->tid=next_tid++; return t; }
    return 0;
}
static void task_prepare(Task *t,const struct regs *fr){      /* pila de kernel: [r15..rbp][task_start][marco] */
    u8 *top=t->kstk+KSTK; struct regs *f=(struct regs*)(top-sizeof(struct regs)); *f=*fr; t->frame=f;
    u64 *sp=(u64*)f; *--sp=(u64)(uintptr_t)task_start; for(int i=0;i<6;i++) *--sp=0; t->ksp=(u64)(uintptr_t)sp;
}
static void shm_free(KObj *o);
static void obj_unref(KObj *o){ if(o&&--o->refs<=0){ if(o->kind==12) shm_free(o); if(o->data) kfree(o->data); kfree(o); } }
static void fd_close(FD *f){
    if(f->type==3||f->type==4){ struct Pipe *p=f->pipe; if(f->type==3) p->readers--; else p->writers--; if(p->readers<=0&&p->writers<=0) kfree(p); }
    else if(f->type==5) sock_unref(f->aux);
    else if(f->type==2||f->type==10) fs_node_unref(f->node);
    else if(f->type==6||f->type==7||f->type==8||f->type==11||f->type==12||(f->type==10&&f->obj)) obj_unref(f->obj);
    memset(f,0,sizeof *f);
}
static void fd_dup_into(FD *d,const FD *s){ *d=*s; d->cloexec=0; if(s->type==3) s->pipe->readers++; else if(s->type==4) s->pipe->writers++; else if(s->type==5) sock_ref(s->aux); else if((s->type==2||s->type==10)&&s->node) fs_node_ref(s->node); else if(s->obj&&(s->type==6||s->type==7||s->type==8||s->type==11||s->type==12||s->type==10)) s->obj->refs++; }

#include "sys_tty.inc"
static int pipe_ready(Task *t){
    FD *f=&t->pr->fd[t->pipe_fd]; if(f->type!=3&&f->type!=4) return 1; struct Pipe *p=f->pipe;
    return f->type==3 ? (p->count>0||p->writers<=0) : (p->count<4096||p->readers<=0);
}
static void poll_wakeups(void){
    u32 now=kticks(); int any_read=0, any_fg=0;
    for(int i=0;i<MAXT;i++){ Task *t=&tasks[i];
        if(t->state==T_FREE) continue;
        if(t->leader&&t->pr&&t->pr->alarm_at&&t->state!=T_ZOMBIE&&(int)(now-(u32)t->pr->alarm_at)>=0){
            t->pr->alarm_at=t->pr->alarm_int?(u64)(u32)(now+(u32)t->pr->alarm_int):0; sig_queue_proc(t->pr,14); }
        if(t->state==T_READY) continue;
        if(t->state==T_ZOMBIE){
            if(t!=cur&&!t->leader) t->state=T_FREE;                       /* hilo terminado: su hueco se reutiliza */
            else if(t!=cur&&t->leader&&t->bg==2){ int pid=t->pid, code=t->exit_code; t->state=T_FREE; if(kproc_done) kproc_done(pid,code); }      /* programa pegado a la terminal: la ventana vuelve a mostrar el prompt */
            else if(t!=cur&&t->leader&&t->bg){ kprintf("[proceso %d termino con codigo %d]\n",t->pid,t->exit_code); t->state=T_FREE; }
            else if(t!=cur&&t->leader&&!t->fg&&t->ppid==0) t->state=T_FREE;      /* huerfano ya terminado */
            continue; }
        if(t->state==T_READ) any_read=1;
        if(t->leader&&(t->fg||t->bg==2)) any_fg=1;
        if(t->killed||(t->sigpend&~t->sigmask)){ t->state=T_READY; continue; }
        if(t->state==T_SLEEP&&(int)(now-(u32)t->wake_at)>=0) t->state=T_READY;
        else if(t->state==T_FUTEX&&t->wake_at&&(int)(now-(u32)t->wake_at)>=0){ t->wake_at=0; t->fut_key=0; t->timedout=1; t->state=T_READY; }
        else if(t->state==T_PIPE&&pipe_ready(t)) t->state=T_READY;
        else if(t->state==T_FN){ if(t->wait_fn(t)) t->state=T_READY; else if(t->wake_at&&(int)(now-(u32)t->wake_at)>=0){ t->timedout=1; t->state=T_READY; } }
        else if(t->state==T_WAIT){
            for(int j=0;j<MAXT;j++){ Task *c=&tasks[j]; if(c->state==T_ZOMBIE&&c->leader&&c->ppid==t->pid&&(t->wait_pid<=0||c->pid==t->wait_pid)){ t->state=T_READY; break; } }
        }
    }
    if(any_read||any_fg){ stdin_pump(); if(any_read&&tty_input_ready()) for(int i=0;i<MAXT;i++) if(tasks[i].state==T_READ) tasks[i].state=T_READY; }     /* con un programa pegado a la terminal las teclas (Ctrl+C...) se atienden aunque no lea */
}
static Task *pick_next(void){                    /* turnos: tasks[0..MAXT-1] y luego el kernel (task0) */
    int me=cur==&task0?MAXT:(int)(cur-tasks);
    for(int k=1;k<=MAXT+1;k++){ int i=(me+k)%(MAXT+1); Task *t=i==MAXT?&task0:&tasks[i]; if(t->state==T_READY) return t; }
    return &task0;
}
static int any_ready_user(void){ for(int i=0;i<MAXT;i++) if(tasks[i].state==T_READY) return 1; return 0; }
static void run_task(Task *n){
    if(n==cur) return;
    Task *p=cur; if(p->pr){ p->fs_base=rdmsr(0xC0000100); __asm__ volatile("fxsave %0":"=m"(*(u8(*)[512])p->fx)); }
    cur=n;
    if(n->kstk){ cur_ktop=(u64)(uintptr_t)(n->kstk+KSTK); }
    if(n->pr){ ucr3=(u64)(uintptr_t)n->pr->pml4; wrmsr(0xC0000100,n->fs_base); __asm__ volatile("fxrstor %0"::"m"(*(u8(*)[512])n->fx)); }
    switch_to(&p->ksp,n->ksp);
}
static void schedule(void){ poll_wakeups(); run_task(pick_next()); }
static int block(int state){ cur->state=state; schedule(); return INTR()!=0; }
static int w_never(Task *t){ (void)t; return 0; }
/* espera hasta que se cumpla fn(tarea) o pasen 'tmo' ticks (0 = sin limite). 0 listo, 1 interrumpido por senal, 2 tiempo agotado */
static int block_fn(int (*fn)(Task*),u32 tmo){
    cur->wait_fn=fn; cur->timedout=0; cur->wake_at=tmo?(u64)(kticks()+tmo):0; if(tmo&&!cur->wake_at) cur->wake_at=1;
    block(T_FN);
    cur->wake_at=0;
    if(cur->timedout){ cur->timedout=0; return 2; }
    return INTR()?1:0;
}
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
    if(me->leader&&me->ppid) for(int i=0;i<MAXT;i++){ Task *pt=&tasks[i]; if(pt->state!=T_FREE&&pt->state!=T_ZOMBIE&&pt->leader&&pt->pid==me->ppid&&pt->pr&&pt->pr->sa[17].h>1){ sig_queue_proc(pt->pr,17); break; } }
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

#define MAXARG 64
static int vma_add(Proc *pr,u64 s,u64 e,u32 prot,u32 kind,KObj *obj,FsNode *node,u64 off);
static int build_stack(Proc *pr,u64 *sp_out,int argc,char **argv,int envc,char **envp,const char *execfn,u64 entry,u64 phdr,int phnum,u64 at_base){
    if(argc>MAXARG){ argc=MAXARG; }
    if(envc>MAXARG){ envc=MAXARG; }
    if(vma_add(pr,STACK_TOP-STACK_SIZE,STACK_TOP,3,1,0,0,0)) return -1;            /* pila: 8 MB, las paginas se crean al usarlas */
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

static const char *DEFENV[]={"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin","HOME=/root","TERM=linux","LANG=C.UTF-8","USER=root","LOGNAME=root","SHELL=/bin/sh","TMPDIR=/tmp","XDG_RUNTIME_DIR=/run","DISPLAY=:0"};
#define DEFENV_N 10
static int exec_quiet;                                   /* execve de un programa de usuario: el error se devuelve sin escribir en pantalla */
static int find_image(const char *path,const u8 **img,u64 *sz){
    if(k_startswith(path,"/bin/")) for(unsigned i=0;i<sizeof builtins/sizeof *builtins;i++) if(!k_strcmp(path+5,builtins[i].name)){ *img=(u8*)builtins[i].s; *sz=(u64)(builtins[i].e-builtins[i].s); return 0; }
    if(!fs_root()) fs_init();
    FsNode *f=fs_resolve(fs_root(),path); if(!f||f->dir||!f->data) return -1;
    *img=(u8*)f->data; *sz=(u64)f->size; return 0;
}
/* arma un espacio de direcciones nuevo con el programa cargado; 0 ok */
static int load_image2(Proc *pr,const char *path,int argc,char **argv,int envc,char **envp,u64 *entry,u64 *sp,int depth){
    const u8 *img; u64 sz;
    if(find_image(path,&img,&sz)){ if(!depth&&!exec_quiet) kprintf("ejecutar: no existe %s\n",path); return -ENOENT; }
    if(sz>2&&img[0]=='#'&&img[1]=='!'){                              /* script: se ejecuta con el interprete indicado en la primera linea */
        if(depth>=4) return -ENOEXEC;
        static char interp[4][200], iarg[4][200]; static char *nav[4][MAXARG+2];
        char *ip=interp[depth], *ia=iarg[depth]; char **na=nav[depth]; ip[0]=ia[0]=0;
        u64 i=2; while(i<sz&&(img[i]==' '||img[i]=='\t')) i++;
        int k=0; while(i<sz&&img[i]!='\n'&&img[i]!=' '&&img[i]!='\t'&&img[i]!='\r'&&k<199) ip[k++]=(char)img[i++]; ip[k]=0;
        while(i<sz&&(img[i]==' '||img[i]=='\t')) i++;
        k=0; while(i<sz&&img[i]!='\n'&&img[i]!='\r'&&k<199) ia[k++]=(char)img[i++]; while(k>0&&(ia[k-1]==' '||ia[k-1]=='\t')) k--; ia[k]=0;
        if(!ip[0]) return -ENOEXEC;
        int n=0; na[n++]=ip; if(ia[0]) na[n++]=ia; na[n++]=(char*)path; for(int a=1;a<argc&&n<MAXARG;a++) na[n++]=argv[a];
        return load_image2(pr,ip,n,na,envc,envp,entry,sp,depth+1);
    }
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
    k_strcpy(pr->exe,path,sizeof pr->exe);
    if(!envp){ envp=(char**)DEFENV; envc=DEFENV_N; }
    if(build_stack(pr,sp,argc,argv,envc,envp,path,m.entry,m.phdr,m.phnum,at_base)) return -ENOMEM;
    pr->brk0=pr->brk=(m.end+4095+65536)&~4095ull; pr->mmap_next=MMAP_BASE;
    return 0;
}
static int load_image(Proc *pr,const char *path,int argc,char **argv,int envc,char **envp,u64 *entry,u64 *sp){ return load_image2(pr,path,argc,argv,envc,envp,entry,sp,0); }
static Proc *proc_new(void){ Proc *p=kmalloc(sizeof *p); if(p){ memset(p,0,sizeof *p); k_strcpy(p->cwd,"/",sizeof p->cwd); } return p; }

/* API para el shell del kernel: crea el proceso y (si no es en segundo plano) espera a que termine */
int kattach_pid; char kshell_cwd[96]; void (*kproc_done)(int pid,int code);
int proc_exec(char *path,int argc,char **argv,int bg){
    if(proc_init()){ kprintf("ejecutar: no hay memoria\n"); return -1; }
    Proc *pr=proc_new(); if(!pr){ kprintf("ejecutar: sin memoria\n"); return -1; }
    u64 entry,sp;
    if(load_image(pr,path,argc,argv,0,0,&entry,&sp)){ if(pr->slab_n) pg_free_all(pr); kfree(pr); return -1; }
    for(int i=0;i<3;i++) pr->fd[i].type=1;
    Task *t=task_alloc(); if(!t){ kprintf("ejecutar: demasiadas tareas\n"); pg_free_all(pr); kfree(pr); return -1; }
    t->pr=pr; t->pid=t->tid; pr->pid=t->pid; pr->nthreads=1; t->ppid=0; t->leader=1; t->bg=bg; t->fg=!bg;
    if(kshell_cwd[0]) k_strcpy(pr->cwd,kshell_cwd,sizeof pr->cwd);
    k_strcpy(t->name,path+(k_startswith(path,"/bin/")?5:0),sizeof t->name);
    struct regs fr; memset(&fr,0,sizeof fr); fr.rip=entry; fr.cs=0x23; fr.ss=0x1B; fr.rflags=0x202; fr.rsp=sp;
    tty_reset_input();
    task_prepare(t,&fr); t->state=T_READY;
    if(bg==2){ kattach_pid=t->pid; return t->pid; }                      /* pegado a la terminal grafica: la ventana sigue viva y el prompt vuelve al terminar */
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
    kprintf("  TID PID PPID ESTADO SYS NOMBRE / ESPERA\n");
    static const char *st[]={"libre","listo","dormido","espera","entrada","futex","pipe","zombie","e/s"};
    for(int i=0;i<MAXT;i++){ Task *t=&tasks[i]; if(t->state==T_FREE) continue;
        kprintf("  %d %d %d %s sys=%d %s%s%s",t->tid,t->pid,t->ppid,st[t->state],t->sysnr,t->name,t->leader?"":" (hilo)",t->fg?" [fg]":"");
        if(t->state==T_WAIT) kprintf("  espera hijo %d",t->wait_pid);
        else if(t->state==T_PIPE) kprintf("  pipe fd %d",t->pipe_fd);
        else if(t->state==T_FUTEX) kprintf("  futex %lx",t->fut_key&0xFFFFFFFFull);
        else if(t->state==T_FN){ u64 v[3]={0,0,0}; if(t->wa_p1) ucopy_from(t->pr,&v[0],t->wa_p1,8); if(t->wa_p2) ucopy_from(t->pr,&v[1],t->wa_p2,8); if(t->wa_p3) ucopy_from(t->pr,&v[2],t->wa_p3,8);
            kprintf("  poll/select n=%d p1=%lx p2=%lx p3=%lx r=%lx w=%lx e=%lx listo=%d",t->wa_n,t->wa_p1,t->wa_p2,t->wa_p3,v[0],v[1],v[2],t->wait_fn?t->wait_fn(t):-1); }
        else if(t->state==T_SLEEP) kprintf("  %d ticks",(int)((u32)t->wake_at-kticks()));
        kprintf("\n");
        if(t->leader&&t->pr&&t->state!=T_ZOMBIE){                      /* descriptores abiertos (los 24 primeros) */
            kprintf("      fds:");
            for(int d=0;d<24;d++){ FD *f=&t->pr->fd[d]; if(!f->type) continue;
                kprintf(" %d:t%d",d,f->type);
                if(f->type==3||f->type==4){ struct Pipe *pp=f->pipe; kprintf("(%lx c=%d r=%d w=%d)",(u64)(uintptr_t)pp&0xFFFFFFull,pp->count,pp->readers,pp->writers); } }
            kprintf("\n"); }
    }
}
int proc_kill(int pid,int sig){
    int n=0; for(int i=0;i<MAXT;i++) if(tasks[i].state!=T_FREE&&tasks[i].state!=T_ZOMBIE&&tasks[i].pid==pid){ tasks[i].killed=sig?sig:9; n++; }
    return n;
}

/* ---------- llamadas al sistema (numeros y estructuras = ABI de Linux x86-64) ---------- */
static int fd_alloc(Proc *pr,int from){ for(int f=from;f<MAXFD;f++) if(!pr->fd[f].type) return f; return -1; }
static int abs_path(const char *path,char *full,int max){
    int n=0; if(path[0]!='/'){ k_strcpy(full,CP->cwd,max); n=k_strlen(full); if(n&&full[n-1]!='/') full[n++]='/'; }
    int l=k_strlen(path); if(n+l>=max) return -1; memcpy(full+n,path,(size_t)l+1); return 0;
}
static FsNode *resolve_parent(const char *path,char *leaf){      /* carpeta contenedora y nombre final */
    char full[200]; if(abs_path(path,full,sizeof full)) return 0;
    int e=k_strlen(full); while(e>1&&full[e-1]=='/') full[--e]=0;
    int s2=e; while(s2>0&&full[s2-1]!='/') s2--;
    if(e-s2<=0||e-s2>=FS_NAME) return 0;
    memcpy(leaf,full+s2,(size_t)(e-s2)); leaf[e-s2]=0;
    if(s2<=1) full[1]=0; else full[s2-1]=0;
    return fs_resolve(fs_root(),full);
}
#include "sys_sig.inc"
#include "sys_net.inc"
#include "sys_proc.inc"
static void etc_defaults(void){
    FsNode *root=fs_root(), *etc=root?fs_find(root,"etc"):0; if(!etc) return;
    static const struct { const char *n,*d; } L[]={{"hosts","127.0.0.1 localhost\n10.0.2.15 kng\n"},{"hostname","kng\n"},{"passwd","root:x:0:0:root:/root:/bin/sh\nnobody:x:65534:65534:nobody:/:/bin/false\n"},
        {"group","root:x:0:\nnogroup:x:65534:\n"},{"nsswitch.conf","passwd: files\ngroup: files\nhosts: files dns\n"},{"os-release","NAME=\"Koenigreich OS\"\nID=kng\nPRETTY_NAME=\"Koenigreich OS (kng)\"\n"},{"machine-id","6b6e6700000040008000000000000001\n"}};
    for(unsigned i=0;i<sizeof L/sizeof *L;i++) if(!fs_find(etc,L[i].n)){ FsNode *n=fs_create(etc,L[i].n,L[i].d,k_strlen(L[i].d)); if(n) n->sys=1; }
}
static void refresh_resolv(void){
    FsNode *etc=fs_find(fs_root(),"etc"); if(!etc) return;
    u32 d=net_dns_addr(); if(!d) d=(10u<<24)|(0<<16)|(2<<8)|3;
    char b[64]; int n=0; const char *h="nameserver "; while(*h) b[n++]=*h++;
    for(int i=0;i<4;i++){ u32 v=(d>>(24-8*i))&255; char t[4]; int k=0; if(!v) t[k++]='0'; while(v){ t[k++]=(char)('0'+v%10); v/=10; } while(k) b[n++]=t[--k]; if(i<3) b[n++]='.'; }
    b[n++]='\n'; b[n]=0;
    FsNode *f=fs_create(etc,"resolv.conf",b,n); if(f) f->sys=1;
}
static u64 console_read(u64 buf,u64 n,int nb){
    if(!n) return 0;
    u32 t0=kticks(); int waited=0;
    for(;;){
        stdin_pump();
        if(tty.lflag&TL_ICANON){
            if(inl_ready&&inl_eof){ inl_eof=0; inl_ready=0; inl_n=inl_r=0; return 0; }                      /* Ctrl+D en linea vacia: fin de archivo */
            if(inl_ready&&inl_r<inl_n){ u64 k=(u64)(inl_n-inl_r); if(k>n) k=n; if(ucopy_to(CP,buf,inl+inl_r,k)) return RET_ERR(EFAULT); inl_r+=(int)k; if(inl_r>=inl_n){ inl_ready=0; inl_n=inl_r=0; } return k; }
        } else if(raw_avail()>0){
            u8 tmp[64]; u64 k=0; while(k<n&&k<sizeof tmp&&raw_avail()>0){ tmp[k++]=rawq[raw_t]; raw_t=(raw_t+1)%(int)sizeof rawq; }
            if(ucopy_to(CP,buf,tmp,k)) return RET_ERR(EFAULT);
            return k;
        } else if(tty.cc[6]==0){                                                                              /* VMIN=0: no espera, o espera VTIME decimas */
            if(!tty.cc[5]) return 0;
            if(waited&&(int)(kticks()-t0)>=tty.cc[5]*10) return 0;
            waited=1; cur->wake_at=(u64)(kticks()+1); if(INTR()) return RET_ERR(EINTR); block(T_SLEEP); continue;
        }
        if(nb) return RET_ERR(EAGAIN);
        if(INTR()) return RET_ERR(EINTR);
        block(T_READ);
    }
}
static u64 shm_rw(KObj *o,u64 off,u8 *buf,u64 n,int write);
static int shm_resize(KObj *o,u64 size);
static u64 fd_read(u64 fdn,u64 buf,u64 n);
static u64 fd_write(u64 fdn,u64 buf,u64 n);
static u64 fd_read(u64 fdn,u64 buf,u64 n){
    if(fdn>=MAXFD||!CP->fd[fdn].type) return RET_ERR(EBADF);
    FD *f=&CP->fd[fdn];
    switch(f->type){
    case 1: return console_read(buf,n,(f->flags&0x800)!=0);
    case 2: {
        FsNode *nd=f->node; if(nd->dir) return RET_ERR(EISDIR);
        u64 left=f->pos<(u64)nd->size?(u64)nd->size-f->pos:0; if(n>left) n=left;
        if(n&&ucopy_to(CP,buf,nd->data+f->pos,n)) return RET_ERR(EFAULT);
        f->pos+=n; return n; }
    case 3: {
        struct Pipe *p=f->pipe;
        for(;;){
            if(p->count>0){ u64 k=0; u8 tmp[512]; while(k<n&&p->count>0){ u64 c=n-k; if(c>sizeof tmp) c=sizeof tmp; u64 j=0; while(j<c&&p->count>0){ tmp[j++]=p->buf[p->r]; p->r=(p->r+1)%4096; p->count--; }
                    if(ucopy_to(CP,buf+k,tmp,j)){ return RET_ERR(EFAULT); }
                    k+=j; } return k; }
            if(p->writers<=0) return 0;
            if(f->flags&0x800) return RET_ERR(EAGAIN);
            if(INTR()) return RET_ERR(EINTR);
            cur->pipe_fd=(int)fdn; block(T_PIPE);
        } }
    case 5: return sock_recv_fd(fdn,buf,n,0,0,0);
    case 6: {
        if(n<8) return RET_ERR(EINVAL);
        KObj *o=f->obj;
        for(;;){
            if(o->cnt>0){ u64 v=(o->flags&1)?1:o->cnt; o->cnt-=v; return ucopy_to(CP,buf,&v,8)?RET_ERR(EFAULT):8; }
            if(f->flags&0x800) return RET_ERR(EAGAIN);
            if(INTR()) return RET_ERR(EINTR);
            if(wait_fd((int)fdn,PIN,0)==1) return RET_ERR(EINTR);
        } }
    case 8: {
        if(n<8) return RET_ERR(EINVAL);
        KObj *o=f->obj;
        for(;;){
            tfd_update(o);
            if(o->cnt>0){ u64 v=o->cnt; o->cnt=0; return ucopy_to(CP,buf,&v,8)?RET_ERR(EFAULT):8; }
            if(f->flags&0x800) return RET_ERR(EAGAIN);
            if(INTR()) return RET_ERR(EINTR);
            if(wait_fd((int)fdn,PIN,0)==1) return RET_ERR(EINTR);
        } }
    case 10: return dev_read(f,buf,n);
    case 11: { KObj *o=f->obj; u64 left=f->pos<(u64)o->size?(u64)o->size-f->pos:0; if(n>left) n=left; if(n&&ucopy_to(CP,buf,o->data+f->pos,n)) return RET_ERR(EFAULT); f->pos+=n; return n; }
    case 12: { KObj *o=f->obj; u8 tmp[1024]; u64 done=0; while(done<n&&f->pos<o->ssize){ u64 c=n-done; if(c>sizeof tmp) c=sizeof tmp; u64 got=shm_rw(o,f->pos,tmp,c,0); if(!got) break;
                   if(ucopy_to(CP,buf+done,tmp,got)) return done?done:RET_ERR(EFAULT);
                   done+=got; f->pos+=got; } return done; }
    }
    return RET_ERR(EBADF);
}
static u64 fd_write(u64 fdn,u64 buf,u64 n){
    if(fdn>=MAXFD||!CP->fd[fdn].type) return RET_ERR(EBADF);
    FD *f=&CP->fd[fdn]; u8 tmp[1024]; u64 done=0;
    switch(f->type){
    case 1:
        while(done<n){ u64 c=n-done; if(c>sizeof tmp) c=sizeof tmp; if(ucopy_from(CP,tmp,buf+done,c)) return done?done:RET_ERR(EFAULT); k_write_console((char*)tmp,(int)c); done+=c; }
        return n;
    case 2: {
        FsNode *nd=f->node; if(nd->dir) return RET_ERR(EISDIR);
        if(f->flags&0x400) f->pos=(u64)nd->size;
        if(f->pos+n>0x7fff0000ull) return RET_ERR(28);
        while(done<n){ u64 c=n-done; if(c>sizeof tmp) c=sizeof tmp; if(ucopy_from(CP,tmp,buf+done,c)) return done?done:RET_ERR(EFAULT);
            if(fs_write_at(nd,(int)f->pos,(char*)tmp,(int)c)) return done?done:RET_ERR(28);
            f->pos+=c; done+=c; }
        return n; }
    case 12: {
        KObj *o=f->obj; if(f->flags&0x400) f->pos=o->ssize;
        if(f->pos+n>o->ssize&&shm_resize(o,f->pos+n)) return RET_ERR(ENOMEM);
        while(done<n){ u64 c=n-done; if(c>sizeof tmp) c=sizeof tmp; if(ucopy_from(CP,tmp,buf+done,c)) return done?done:RET_ERR(EFAULT);
            u64 w=shm_rw(o,f->pos,tmp,c,1); if(!w) break; f->pos+=w; done+=w; }
        return done; }
    case 4: {
        struct Pipe *p=f->pipe;
        while(done<n){
            if(p->readers<=0){ sig_self(13); return done?done:RET_ERR(EPIPE); }
            if(p->count>=4096){ if(f->flags&0x800) return done?done:RET_ERR(EAGAIN); if(INTR()) return done?done:RET_ERR(EINTR); cur->pipe_fd=(int)fdn; block(T_PIPE); continue; }
            u64 c=n-done; if(c>sizeof tmp) c=sizeof tmp; if(c>(u64)(4096-p->count)) c=(u64)(4096-p->count);
            if(ucopy_from(CP,tmp,buf+done,c)) return done?done:RET_ERR(EFAULT);
            for(u64 j=0;j<c;j++){ p->buf[p->w]=tmp[j]; p->w=(p->w+1)%4096; p->count++; } done+=c;
        }
        return n; }
    case 5: return sock_send_fd(fdn,buf,n,0,0,0,0,0,0);
    case 6: { if(n<8) return RET_ERR(EINVAL); u64 v; if(ucopy_from(CP,&v,buf,8)) return RET_ERR(EFAULT); if(v==~0ull) return RET_ERR(EINVAL); f->obj->cnt+=v; return 8; }
    case 10: return dev_write(f,buf,n);
    }
    return RET_ERR(EBADF);
}
#include "sys_mem.inc"
#include "sys_fs.inc"
static u64 sys_brk(u64 want){
    if(want<CP->brk0||want>=CP->brk0+(256ull<<20)) return CP->brk;
    if(want>CP->brk){ if(umap_range(CP,CP->brk,want-CP->brk)) return CP->brk; }
    CP->brk=want; return CP->brk;
}
static const char *UN[6]={"Linux","kng","6.1.0-kng","kng-compat 1","x86_64","(none)"};

static u64 sys_clone(struct regs *r,u64 flags,u64 stack,u64 ptid,u64 ctid,u64 tls){
    int thread=(flags&0x10000)!=0;
    if(flags&0x7E020000ull) return RET_ERR(EPERM);                 /* espacios de nombres (NEWNS, NEWUTS, NEWIPC, NEWUSER, NEWPID, NEWNET, NEWCGROUP): no hay */
    Task *t=task_alloc(); if(!t) return RET_ERR(EAGAIN);
    Proc *pr=CP;
    if(!thread){
        Proc *np=proc_new(); if(!np){ return RET_ERR(ENOMEM); }
        if(proc_space(np)||as_clone(np,pr)){ pg_free_all(np); kfree(np); return RET_ERR(ENOMEM); }
        np->brk=pr->brk; np->brk0=pr->brk0; np->mmap_next=pr->mmap_next; memcpy(np->sa,pr->sa,sizeof np->sa); memcpy(np->cwd,pr->cwd,sizeof np->cwd); memcpy(np->exe,pr->exe,sizeof np->exe); np->umask=pr->umask;
        for(int i=0;i<MAXFD;i++) if(pr->fd[i].type){ fd_dup_into(&np->fd[i],&pr->fd[i]); np->fd[i].cloexec=pr->fd[i].cloexec; }
        np->pid=t->tid; np->nthreads=1; t->pr=np; t->pid=t->tid; t->ppid=cur->pid; t->leader=1;
    } else { pr->nthreads++; t->pr=pr; t->pid=cur->pid; t->ppid=cur->ppid; t->leader=0; }
    t->fs_base=(flags&0x80000)?tls:cur->fs_base; memcpy(t->name,cur->name,sizeof t->name); t->sigmask=cur->sigmask;
    __asm__ volatile("fxsave %0":"=m"(*(u8(*)[512])cur->fx)); memcpy(t->fx,cur->fx,512);
    struct regs fr=*r; fr.rax=0; if(stack) fr.rsp=stack; fr.rflags|=0x200;
    task_prepare(t,&fr);
    u32 tid=(u32)t->tid;
    if(flags&0x100000) ucopy_to(pr,ptid,&tid,4);
    if(flags&0x1000000) ucopy_to(t->pr,ctid,&tid,4);
    if(flags&0x200000) t->clear_tid=ctid;
    t->state=T_READY; return (u64)t->tid;
}
static u64 sys_execve(struct regs *r,u64 path_va,u64 argv_va,u64 envp_va){
    static char path[420], kargv[MAXARG][512], kenv[MAXARG][512]; static char *pa[MAXARG], *pe[MAXARG]; int argc=0, envc=0;
    if(ustr(CP,path,sizeof path,path_va)) return RET_ERR(EFAULT);
    for(; argv_va && argc<MAXARG; argc++){ u64 p=0; if(ucopy_from(CP,&p,argv_va+(u64)argc*8,8)) return RET_ERR(EFAULT); if(!p) break; if(ustr(CP,kargv[argc],512,p)) return RET_ERR(EFAULT); pa[argc]=kargv[argc]; }
    for(; envp_va && envc<MAXARG; envc++){ u64 p=0; if(ucopy_from(CP,&p,envp_va+(u64)envc*8,8)) return RET_ERR(EFAULT); if(!p) break; if(ustr(CP,kenv[envc],512,p)) return RET_ERR(EFAULT); pe[envc]=kenv[envc]; }
    if(!argc){ pa[0]=path; argc=1; }
    Proc *np=proc_new(); if(!np) return RET_ERR(ENOMEM); u64 entry,sp;
    int rc=load_image(np,path,argc,pa,envc,envc?pe:0,&entry,&sp);
    if(rc){ if(np->slab_n) pg_free_all(np); kfree(np); return RET_ERR(-rc); }
    Proc *op=CP; memcpy(np->fd,op->fd,sizeof np->fd); memcpy(np->cwd,op->cwd,sizeof np->cwd); np->pid=op->pid; np->nthreads=op->nthreads;
    for(int i=1;i<65;i++) if(op->sa[i].h==1) np->sa[i]=op->sa[i];                /* lo ignorado sigue ignorado; los manejadores vuelven a "por omision" */
    np->umask=op->umask; np->alarm_at=op->alarm_at; np->alarm_int=op->alarm_int;
    memset(op->fd,0,sizeof op->fd); pg_free_all(op);
    *op=*np; kfree(np);
    for(int i=0;i<MAXFD;i++) if(op->fd[i].type&&op->fd[i].cloexec) fd_close(&op->fd[i]);
    cur->alt_sp=cur->alt_size=0;
    memset(cur->fx,0,512); cur->fx[0]=0x7F; cur->fx[1]=0x03; cur->fx[24]=0x80; cur->fx[25]=0x1F; __asm__ volatile("fxrstor %0"::"m"(*(u8(*)[512])cur->fx));
    ucr3=(u64)(uintptr_t)op->pml4; cur->fs_base=0; wrmsr(0xC0000100,0); cur->clear_tid=0;
    memset(r,0,22*8); r->rip=entry; r->cs=0x23; r->ss=0x1B; r->rflags=0x202; r->rsp=sp; r->int_no=0x80;
    k_strcpy(cur->name,path+(k_startswith(path,"/bin/")?5:0),sizeof cur->name);
    return EXEC_DONE;
}
static long wait_core(long pid,u64 options,u32 *stp,int keep){
    for(;;){
        int have=0;
        for(int i=0;i<MAXT;i++){ Task *c=&tasks[i]; if(c->state==T_FREE||!c->leader||c->ppid!=cur->pid) continue;
            if(pid>0&&c->pid!=(int)pid) continue;
            have=1;
            if(c->state==T_ZOMBIE){
                int code=c->exit_code, rv=c->pid; *stp=code<0?(u32)(-code):(((u32)code&0xFF)<<8);
                if(!keep) c->state=T_FREE;
                return rv; } }
        if(!have) return -ECHILD;
        if(options&1) return 0;
        if(INTR()) return -EINTR;
        cur->wait_pid=(int)pid; block(T_WAIT);
    }
}
static u64 sys_wait4(u64 pid,u64 status_va,u64 options){
    u32 st=0; long r=wait_core((long)(int)pid,options,&st,0);          /* pid es un int de 32 bits: -1 llega como 0xffffffff */
    if(r>0&&status_va) ucopy_to(CP,status_va,&st,4);
    return r<0?RET_ERR(-r):(u64)r;
}
static u64 sys_futex(u64 uaddr,u64 op,u64 val,u64 timeout_va){
    int cmd=(int)(op&0x7F);
    if(cmd==0||cmd==9){                                  /* WAIT / WAIT_BITSET */
        u32 cv; if(ucopy_from(CP,&cv,uaddr,4)) return RET_ERR(EFAULT);
        if(cv!=(u32)val) return RET_ERR(EAGAIN);
        cur->fut_key=(u64)(uintptr_t)CP+uaddr; cur->wake_at=0; cur->timedout=0;
        if(timeout_va){ u64 t[2]; if(ucopy_from(CP,t,timeout_va,16)) return RET_ERR(EFAULT); u32 tk=(u32)(t[0]*100+t[1]/10000000ull); if(!tk) tk=1; cur->wake_at=(u64)(kticks()+tk); }
        if(INTR()) return RET_ERR(EINTR);
        block(T_FUTEX);
        if(cur->timedout){ cur->timedout=0; return RET_ERR(ETIMEDOUT); }
        if(cur->fut_key){ cur->fut_key=0; if(INTR()) return RET_ERR(EINTR); }
        return 0;
    }
    if(cmd==1||cmd==10){ int w=0; futex_wake(CP,uaddr,(int)(val>0x7fffffff?0x7fffffff:val),&w); return (u64)w; }
    if(cmd==3||cmd==4){ int w=0; futex_wake(CP,uaddr,0x7fffffff,&w); return (u64)w; }      /* REQUEUE: se despierta a todos (aproximacion) */
    return RET_ERR(ENOSYS);
}
#include "sys_extra.inc"
/* Argumentos de tipo 'int' (descriptores, banderas): Linux solo mira los 32 bits bajos y hay programas (Chromium, en mmap) que dejan basura en los altos.
 * Devuelve una mascara: el bit i dice que el argumento i+1 es un int que se debe extender con signo desde 32 bits. */
static unsigned sys_intmask(u64 nr){
    switch(nr){
    case 0: case 1: case 3: case 5: case 8: case 16: case 17: case 18: case 19: case 20: case 72: case 73: case 74: case 75: case 77: case 78: case 81: case 91: case 217:
    case 32: case 42: case 43: case 49: case 51: case 52: case 267: case 268: return 1;
    case 9: return 16;
    case 33: case 40: case 48: case 50: case 285: case 62: case 200: return 3;
    case 41: case 53: case 54: case 55: case 292: case 233: case 234: return 7;
    case 44: case 45: case 288: case 280: return 1|8;
    case 46: case 47: case 263: case 258: case 269: case 264: return 1|4;
    case 232: case 281: return 1|4|8;
    case 257: return 1|4|8;
    case 262: return 1|8;
    case 265: case 316: return 1|4|16;
    case 332: return 1|4|8;
    case 266: case 293: case 319: return 2;
    case 291: return 1;
    }
    return 0;
}
static u64 sys_dispatch(struct regs *r){
    u64 nr=r->rax,a1=r->rdi,a2=r->rsi,a3=r->rdx,a4=r->r10,a5=r->r8,a6=r->r9;
    { unsigned im=sys_intmask(nr); if(im){ if(im&1) a1=(u64)(long)(int)a1; if(im&2) a2=(u64)(long)(int)a2; if(im&4) a3=(u64)(long)(int)a3; if(im&8) a4=(u64)(long)(int)a4; if(im&16) a5=(u64)(long)(int)a5; if(im&32) a6=(u64)(long)(int)a6; } }
    switch(nr){
    case 0:  return fd_read(a1,a2,a3);
    case 1:  return fd_write(a1,a2,a3);
    case 2:  return do_open(AT_FDCWD_L,a1,a2,a3);
    case 257:return do_open((long)(int)a1,a2,a3,a4);
    case 3:  if(a1<MAXFD&&CP->fd[a1].type){ fd_close(&CP->fd[a1]); return 0; } return RET_ERR(EBADF);
    case 8:  return sys_lseek(a1,a2,a3);
    case 4:  return sys_stat_at(AT_FDCWD_L,a1,a2,0,0,0);
    case 6:  return sys_stat_at(AT_FDCWD_L,a1,a2,0x100,0,0);
    case 5:  return sys_fstat(a1,a2);
    case 262:return sys_stat_at((long)(int)a1,a2,a3,a4,0,0);
    case 332:return sys_stat_at((long)(int)a1,a2,a5,a3,1,a4);
    case 9:  return sys_mmap(a1,a2,a3,a4,a5,a6);
    case 10: return sys_mprotect(a1,a2,a3);
    case 11: return sys_munmap(a1,a2);
    case 28: return sys_madvise(a1,a2,a3);
    case 25: return sys_mremap(a1,a2,a3,a4);
    case 319: return sys_memfd_create(a1,a2);
    case 29: return sys_shmget(a1,a2,a3);
    case 30: return sys_shmat(a1,a2,a3);
    case 31: return sys_shmctl(a1,a2,a3);
    case 67: return sys_shmdt(a1);
    case 12: return sys_brk(a1);
    case 13: return sys_rt_sigaction(a1,a2,a3,a4);
    case 14: return sys_sigprocmask(a1,a2,a3,a4);
    case 15: return sys_sigreturn(r);
    case 127: { u64 p=cur->sigpend&cur->sigmask; return ucopy_to(CP,a1,&p,8)?RET_ERR(EFAULT):0; }
    case 130: return sys_sigsuspend(a1,a2);
    case 34: return sys_pause();
    case 131: return sys_sigaltstack(a1,a2);
    case 36: { u64 v[4]; u32 now=kticks(); u64 left=CP->alarm_at?(u64)(int)((u32)CP->alarm_at-now):0; if((long)left<0) left=0; v[0]=CP->alarm_int/100; v[1]=(CP->alarm_int%100)*10000; v[2]=left/100; v[3]=(left%100)*10000; return ucopy_to(CP,a2,v,32)?RET_ERR(EFAULT):0; }
    case 37: return sys_alarm(a1);
    case 38: return sys_alarm_set(a1,a2,a3);
    case 16: { if(a1>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF);
        FD *tf=&CP->fd[a1]; int is_tty=tf->type==1||(tf->type==10&&tf->aux==DEV_TTY);
        if(a2==0x5421){ int v=0; if(a3&&ucopy_from(CP,&v,a3,4)) return RET_ERR(EFAULT); if(v) tf->flags|=0x800; else tf->flags&=~0x800; return 0; }      /* FIONBIO */
        if(is_tty) return tty_ioctl(a2,a3);
        if(a2==0x541B){ int v=0; if(tf->type==3) v=tf->pipe->count; else if(tf->type==5) v=sock_avail(tf->aux); return ucopy_to(CP,a3,&v,4)?RET_ERR(EFAULT):0; }   /* FIONREAD */
        return RET_ERR(ENOTTY); }
    case 17: return sys_pio(a1,a2,a3,a4,0);
    case 18: return sys_pio(a1,a2,a3,a4,1);
    case 20: { u64 tot=0; for(u64 i=0;i<a3&&i<64;i++){ u64 iov[2]; if(ucopy_from(CP,iov,a2+i*16,16)) return RET_ERR(EFAULT); long rv=(long)fd_write(a1,iov[0],iov[1]); if(rv<0) return tot?tot:(u64)rv; tot+=(u64)rv; } return tot; }
    case 19: { u64 tot=0; for(u64 i=0;i<a3&&i<64;i++){ u64 iov[2]; if(ucopy_from(CP,iov,a2+i*16,16)) return RET_ERR(EFAULT); long rv=(long)fd_read(a1,iov[0],iov[1]); if(rv<0) return tot?tot:(u64)rv; tot+=(u64)rv; if((u64)rv<iov[1]) break; } return tot; }
    case 21: return sys_access_at(AT_FDCWD_L,a1);
    case 269: case 439: return sys_access_at((long)(int)a1,a2);
    case 89: return sys_readlinkat(AT_FDCWD_L,a1,a2,a3);
    case 267: return sys_readlinkat((long)(int)a1,a2,a3,a4);
    case 22: case 293: {                                      /* pipe / pipe2 */
        int f0=fd_alloc(CP,3); if(f0<0) return RET_ERR(EMFILE); CP->fd[f0].type=9; int f1=fd_alloc(CP,3); CP->fd[f0].type=0; if(f1<0) return RET_ERR(EMFILE);
        struct Pipe *p=kmalloc(sizeof *p); if(!p) return RET_ERR(ENOMEM); memset(p,0,sizeof *p); p->readers=1; p->writers=1;
        u64 fl=nr==293?a2:0; memset(&CP->fd[f0],0,sizeof(FD)); memset(&CP->fd[f1],0,sizeof(FD));
        CP->fd[f0].type=3; CP->fd[f0].pipe=p; CP->fd[f1].type=4; CP->fd[f1].pipe=p;
        CP->fd[f0].flags=CP->fd[f1].flags=(fl&0x800)?0x800:0; CP->fd[f0].cloexec=CP->fd[f1].cloexec=(fl&0x80000)?1:0;
        int fds[2]={f0,f1}; return ucopy_to(CP,a1,fds,8)?RET_ERR(EFAULT):0; }
    case 32: { if(a1>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF); int n=fd_alloc(CP,0); if(n<0) return RET_ERR(EMFILE); fd_dup_into(&CP->fd[n],&CP->fd[a1]); return (u64)n; }
    case 33: case 292: { if(a1>=MAXFD||a2>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF); if(a1==a2) return nr==292?RET_ERR(EINVAL):a2; if(CP->fd[a2].type) fd_close(&CP->fd[a2]); fd_dup_into(&CP->fd[a2],&CP->fd[a1]); if(nr==292&&(a3&0x80000)) CP->fd[a2].cloexec=1; return a2; }
    case 72: { if(a1>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF);
        if(a2==0||a2==1030){ int n=fd_alloc(CP,(int)a3); if(n<0) return RET_ERR(EMFILE); fd_dup_into(&CP->fd[n],&CP->fd[a1]); if(a2==1030) CP->fd[n].cloexec=1; return (u64)n; }
        if(a2==1) return CP->fd[a1].cloexec;
        if(a2==2){ CP->fd[a1].cloexec=(u8)(a3&1); return 0; }
        if(a2==3){ return (u64)(CP->fd[a1].flags&~3)|2; }
        if(a2==4){ CP->fd[a1].flags=(CP->fd[a1].flags&~0x800)|(int)(a3&0x800); return 0; }
        if(a2==5||a2==36){ short lt=2; if(a3&&ucopy_to(CP,a3,&lt,2)) return RET_ERR(EFAULT); return 0; }      /* F_GETLK: nadie tiene el cerrojo */
        return 0; }
    /* ---- red ---- */
    case 41: return sys_socket(a1,a2,a3);
    case 42: return sys_connect(a1,a2,a3);
    case 43: return sys_accept(a1,a2,a3,0);
    case 288: return sys_accept(a1,a2,a3,a4);
    case 44: { int have=0; u32 ip=0; u16 port=0; char up[108]; int pl=0, fam=0; if(a5&&a6){ int rr=get_sockaddr(a5,a6,&fam,&ip,&port,up,&pl); if(rr<0) return RET_ERR(-rr); have=1; }
               if(a1>=MAXFD||CP->fd[a1].type!=5) return RET_ERR(a1<MAXFD&&CP->fd[a1].type?ENOTSOCK:EBADF);
               return sock_send_fd(a1,a2,a3,a4,have,ip,port,up,pl); }
    case 45: if(a1>=MAXFD||CP->fd[a1].type!=5) return RET_ERR(a1<MAXFD&&CP->fd[a1].type?ENOTSOCK:EBADF); return sock_recv_fd(a1,a2,a3,a4,a5,a6);
    case 46: return sys_sendmsg(a1,a2,a3);
    case 47: return sys_recvmsg(a1,a2,a3);
    case 48: return sys_shutdown(a1,a2);
    case 49: return sys_bind(a1,a2,a3);
    case 50: return sys_listen(a1,a2);
    case 51: return sys_getsockname(a1,a2,a3,0);
    case 52: return sys_getsockname(a1,a2,a3,1);
    case 53: return sys_socketpair(a1,a2,a3,a4);
    case 54: return sys_setsockopt(a1,a2,a3,a4,a5);
    case 55: return sys_getsockopt(a1,a2,a3,a4,a5);
    case 7:   return do_poll(a1,a2,(long)(int)a3,0,0);
    case 271: { long ms=-1; if(a3){ u64 t[2]; if(ucopy_from(CP,t,a3,16)) return RET_ERR(EFAULT); ms=(long)(t[0]*1000+t[1]/1000000); } return do_poll(a1,a2,ms,a4,a5); }
    case 23:  { long tk=-1; if(a5){ u64 t[2]; if(ucopy_from(CP,t,a5,16)) return RET_ERR(EFAULT); tk=(long)(t[0]*100+t[1]/10000); if(!tk&&(t[0]||t[1])) tk=1; } return do_select(a1,a2,a3,a4,tk,0); }
    case 270: { long tk=-1; if(a5){ u64 t[2]; if(ucopy_from(CP,t,a5,16)) return RET_ERR(EFAULT); tk=(long)(t[0]*100+t[1]/10000000ull); if(!tk&&(t[0]||t[1])) tk=1; } return do_select(a1,a2,a3,a4,tk,a6); }
    case 213: case 291: return sys_epoll_create(nr==291?a1:0);
    case 233: return sys_epoll_ctl(a1,a2,a3,a4);
    case 232: return sys_epoll_wait(a1,a2,a3,a4,0,0);
    case 281: return sys_epoll_wait(a1,a2,a3,a4,a5,a6);
    case 284: return sys_eventfd(a1,0);
    case 290: return sys_eventfd(a1,a2);
    case 283: return sys_timerfd_create(a1,a2);
    case 286: return sys_timerfd_settime(a1,a2,a3,a4);
    case 287: return sys_timerfd_gettime(a1,a2);
    /* ---- varias que piden los programas ---- */
    case 95: { u32 o=CP->umask; CP->umask=(u32)a1&0777; return o; }
    case 98: case 100: { u8 z[144]; memset(z,0,sizeof z); if(a2&&nr==98) return ucopy_to(CP,a2,z,144)?RET_ERR(EFAULT):0; if(a1&&nr==100) ucopy_to(CP,a1,z,32); return kticks(); }
    case 74: case 75: case 26: case 27: case 149: case 150: case 221: case 144: case 142: case 141: case 145: case 146: case 73: case 162: case 306: return 0;
    case 90: case 92: return sys_exists_noop(AT_FDCWD_L,a1);
    case 94: return sys_exists_nf(AT_FDCWD_L,a1,0);
    case 132: return sys_utime(AT_FDCWD_L,a1,a2,2,1);
    case 235: return sys_utime(AT_FDCWD_L,a1,a2?a2-16:0,1,1);
    case 268: return sys_exists_noop((long)(int)a1,a2);
    case 260: return sys_exists_nf((long)(int)a1,a2,!(a5&0x100));
    case 261: return sys_utime((long)(int)a1,a2,a3?a3-16:0,1,1);
    case 280: return sys_utime((long)(int)a1,a2,a3,0,!(a4&0x100));
    case 91: case 93: if(a1>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF); return 0;
    case 76: return sys_truncate_path(a1,a2);
    case 77: return sys_ftruncate_fd(a1,a2);
    case 285: if(a2) return 0; { u64 end=a3+a4; if(a1>=MAXFD||!CP->fd[a1].type) return RET_ERR(EBADF); if(CP->fd[a1].type==12){ if(end>CP->fd[a1].obj->ssize&&shm_resize(CP->fd[a1].obj,end)) return RET_ERR(ENOMEM); return 0; } if(CP->fd[a1].type==2){ FsNode *nd=CP->fd[a1].node; if(end>(u64)nd->size&&fs_resize(nd,(int)end)) return RET_ERR(28); return 0; } return RET_ERR(EINVAL); }
    case 137: case 138: { u8 sf[120]; memset(sf,0,sizeof sf); *(u64*)(sf+0)=0xEF53; *(u64*)(sf+8)=4096; *(u64*)(sf+16)=262144; *(u64*)(sf+24)=131072; *(u64*)(sf+32)=131072; *(u64*)(sf+40)=100000; *(u64*)(sf+48)=50000; *(u64*)(sf+72)=255; return ucopy_to(CP,a2,sf,sizeof sf)?RET_ERR(EFAULT):0; }
    case 157: { if(a1==15){ char nm[16]; if(ustr(CP,nm,sizeof nm,a2)) return RET_ERR(EFAULT); k_strcpy(cur->name,nm,sizeof cur->name); return 0; } if(a1==16){ return ucopy_to(CP,a2,cur->name,16)?RET_ERR(EFAULT):0; } return 0; }
    case 111: return (u64)cur->pid;
    case 24: if(any_ready_user()){ cur->state=T_READY; schedule(); } return 0;
    case 35: case 230: { u64 ts=nr==35?a1:a3; u64 t[2]; if(ucopy_from(CP,t,ts,16)) return RET_ERR(EFAULT); u32 tk=(u32)(t[0]*100+t[1]/10000000ull); cur->wake_at=(u64)(kticks()+(tk?tk:1)); if(INTR()) return RET_ERR(EINTR); block(T_SLEEP); if(INTR()) return RET_ERR(EINTR); return 0; }
    case 39: return (u64)cur->pid;
    case 56: return sys_clone(r,a1,a2,a3,a4,a5);
    case 57: return sys_clone(r,0x11,0,0,0,0);                 /* fork */
    case 58: return sys_clone(r,0x4111,0,0,0,0);               /* vfork: se trata como fork (copia) */
    case 59: { exec_quiet=1; u64 xr=sys_execve(r,a1,a2,a3); exec_quiet=0; return xr; }
    case 60: if(cur->leader||CP->nthreads<=1) do_exit_group((int)(a1&0xFF));
             { Task *me=cur; thread_exit_cleanup(me); CP->nthreads--; me->state=T_ZOMBIE; schedule(); for(;;) __asm__ volatile("hlt"); }
    case 231: do_exit_group((int)(a1&0xFF));
    case 61: return sys_wait4(a1,a2,a3);
    case 62: return sys_kill((int)a1,(int)a2,0,0);
    case 200: return sys_kill(0,(int)a2,1,(int)a1);
    case 234: return sys_kill(0,(int)a3,1,(int)a2);
    case 63: { u8 b[390]; memset(b,0,sizeof b); for(int i=0;i<6;i++) memcpy(b+i*65,UN[i],(size_t)k_strlen(UN[i])); return ucopy_to(CP,a1,b,sizeof b)?RET_ERR(EFAULT):0; }
    case 79: { int l=k_strlen(CP->cwd)+1; if((u64)l>a2) return RET_ERR(ERANGE); return ucopy_to(CP,a1,CP->cwd,(u64)l)?RET_ERR(EFAULT):(u64)l; }
    case 80: return sys_chdir_path(a1);
    case 81: return sys_fchdir(a1);
    case 82: return sys_renameat(AT_FDCWD_L,a1,AT_FDCWD_L,a2);
    case 264: case 316: return sys_renameat((long)(int)a1,a2,(long)(int)a3,a4);
    case 86: return sys_linkat(AT_FDCWD_L,a1,AT_FDCWD_L,a2);
    case 265: return sys_linkat((long)(int)a1,a2,(long)(int)a3,a4);
    case 88: return sys_symlinkat(a1,AT_FDCWD_L,a2);
    case 266: return sys_symlinkat(a1,(long)(int)a2,a3);
    case 83: return sys_mkdirat(AT_FDCWD_L,a1);
    case 258: return sys_mkdirat((long)(int)a1,a2);
    case 263: return sys_unlinkat((long)(int)a1,a2,a3);
    case 84: return sys_unlinkat(AT_FDCWD_L,a1,0x200);
    case 87: return sys_unlinkat(AT_FDCWD_L,a1,0);
    case 217: return sys_getdents64(a1,a2,a3);
    case 96: { u64 t[2]; u32 k=kticks(); t[0]=wall_sec(); t[1]=(u64)(k%100)*10000; return a1&&ucopy_to(CP,a1,t,16)?RET_ERR(EFAULT):0; }
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
    case 228: return sys_clock_gettime(a1,a2);
    case 273: return 0;
    case 302: return a4?sys_getrlimit_k(a2,a4):0;
    case 318: { static u64 rs=0x9E3779B97F4A7C15ull; u8 b[256]; u64 n=a2>256?256:a2; rs^=kticks(); for(u64 i=0;i<n;i++){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; b[i]=(u8)rs; } return ucopy_to(CP,a1,b,n)?RET_ERR(EFAULT):n; }
    case 334: return RET_ERR(ENOSYS);
    case 40:  return sys_kcopy(a2,a3,a1,0,a4);                       /* sendfile(out,in,*off,count) */
    case 326: return sys_kcopy(a1,a2,a3,a4,a5);                      /* copy_file_range(in,*off_in,out,*off_out,len,flags) */
    case 97:  return sys_getrlimit_k(a1,a2);
    case 160: return 0;                                             /* setrlimit */
    case 201: { u64 t=wall_sec(); if(a1&&ucopy_to(CP,a1,&t,8)) return RET_ERR(EFAULT); return t; }
    case 164: { u64 t[2]; if(!a1||ucopy_from(CP,t,a1,16)) return RET_ERR(EFAULT); wall_set(t[0]); return 0; }
    case 227: return sys_settime(a1,a2);
    case 229: { u64 t[2]={0,10000000ull}; return a2&&ucopy_to(CP,a2,t,16)?RET_ERR(EFAULT):0; }
    case 247: return sys_waitid(a1,a2,a3,a4);
    case 299: return sys_recvmmsg(a1,a2,a3,a4);
    case 307: return sys_sendmmsg(a1,a2,a3,a4);
    case 295: case 327: return sys_preadv(a1,a2,a3,a4,0);
    case 296: case 328: return sys_preadv(a1,a2,a3,a4,1);
    case 436: return sys_close_range(a1,a2,a3);
    case 125: return sys_capget(a1,a2);
    case 126: case 113: case 114: case 117: case 119: case 122: case 123: case 116: case 135: case 170: case 171: case 151: case 152: case 325: case 277: case 187: case 324: return 0;
    case 118: case 120: { int z[3]={0,0,0}; if(a1&&ucopy_to(CP,a1,z,4)) return RET_ERR(EFAULT); if(a2&&ucopy_to(CP,a2,z,4)) return RET_ERR(EFAULT); if(a3&&ucopy_to(CP,a3,z,4)) return RET_ERR(EFAULT); return 0; }
    case 115: return 0;                                             /* getgroups: ninguno */
    case 140: return 20;                                            /* getpriority: nice 0 */
    case 143: { int z=0; return a2&&ucopy_to(CP,a2,&z,4)?RET_ERR(EFAULT):0; }
    case 147: return (a1==1||a1==2)?99:0;
    case 148: return (a1==1||a1==2)?1:0;
    case 309: { u32 z=0; if(a1&&ucopy_to(CP,a1,&z,4)) return RET_ERR(EFAULT); if(a2&&ucopy_to(CP,a2,&z,4)) return RET_ERR(EFAULT); return 0; }
    case 272: return RET_ERR(EPERM);                                /* unshare: sin espacios de nombres */
    case 188: case 189: case 190: return RET_ERR(95);               /* setxattr: no soportado */
    case 191: case 192: case 193: case 197: case 198: case 199: return RET_ERR(61);   /* getxattr/removexattr: no existe */
    case 194: case 195: case 196: return 0;                         /* listxattr: ninguno */
    default:
        kprintf("[syscall] numero %lu aun no implementado -> ENOSYS\n",nr); return RET_ERR(ENOSYS);
    }
}

/* ---------- punto de entrada desde ring 3 (lo llama proc64.S con la CR3 del kernel) ---------- */
/* rastro: diagnostico. 1 = muestra las llamadas que fallan (con la ruta si la tiene), 2 = muestra todas. Se activa con el comando "rastro". */
int krastro;
static int sc_path_arg(u64 nr){ switch(nr){ case 2: case 4: case 6: case 21: case 59: case 89: case 80: case 82: case 83: case 84: case 87: case 88: case 90: return 1; case 257: case 262: case 267: case 332: case 439: case 258: case 263: case 264: case 259: return 2; } return 0; }
static void rastro_log(u64 nr,u64 a1,u64 a2,u64 rv,int pre,u64 a3,u64 a4,u64 a5){
    char ps[80]; ps[0]=0; int pa=sc_path_arg(nr); if(pa){ if(ustr(CP,ps,sizeof ps,pa==1?a1:a2)) k_strcpy(ps,"?",sizeof ps); }
    if(pre) kprintf("[rs %d] %lu %s ...\n",cur->pid,nr,ps);
    else if((long)rv<0&&(long)rv>-4096) kprintf("[rs %d] %lu %s -> -%ld (a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx)\n",cur->pid,nr,ps,-(long)rv,a1,a2,a3,a4,a5);
    else kprintf("[rs %d] %lu %s -> %ld (a1=%lx a2=%lx a3=%lx a4=%lx)\n",cur->pid,nr,ps,(long)rv,a1,a2,a3,a4);
}
void kernel_entry(struct regs *r){
    cur->frame=r;
    int from_sys=0;
    if(r->int_no==0x80){
        cur->sysnr=(int)r->rax; from_sys=1;
        u64 nr0=r->rax,b1=r->rdi,b2=r->rsi;
        if(krastro&&(nr0==59||krastro>1)) rastro_log(nr0,b1,b2,0,1,0,0,0);
        u64 rv=sys_dispatch(r);
        if(krastro&&rv!=EXEC_DONE&&(krastro>1||((long)rv<0&&(long)rv>-4096&&nr0!=61&&nr0!=7&&nr0!=232&&nr0!=202))) rastro_log(nr0,b1,b2,rv,0,r->rdx,r->r10,r->r8);
        if(rv!=EXEC_DONE) r->rax=rv;
    } else if(r->int_no<32){
        u64 cr2=0; if(r->int_no==14) __asm__ volatile("mov %%cr2,%0":"=r"(cr2));
        int pf_ok=(r->int_no==14&&!(r->err&1)&&vma_fault(CP,cr2,(r->err&2)!=0)==0);      /* primera vez que se toca la pagina: se crea y el programa sigue */
        if(!pf_ok){
            int sig=(r->int_no==14||r->int_no==13||r->int_no==12||r->int_no==11)?11:r->int_no==6?4:(r->int_no==0||r->int_no==16||r->int_no==19)?8:r->int_no==17?7:5;
            if(sig_fault(r,sig,cr2)){
                kprintf("\n[proceso %d] excepcion %lu en el programa: rip=0x%lx err=0x%lx cr2=0x%lx -> terminado\n",cur->pid,r->int_no,r->rip,r->err,cr2);
                do_exit_group(-sig);
            }
        }
    } else {
        isr_handler64(r);
        if(r->int_no==32){ static int q; if(++q>=2){ q=0; Task *n=pick_next(); if(n!=cur){ poll_wakeups(); run_task(pick_next()); } } }
    }
    r->rflags|=0x200;
    if(cur->killed) do_exit_group(-cur->killed);
    sig_deliver(r,from_sys);
}

/* ---------- rootfs (archivo cpio "newc" entregado como modulo Multiboot: -initrd en QEMU, "module" en GRUB) ---------- */
static u32 hex8(const u8 *p){ u32 v=0; for(int i=0;i<8;i++){ u8 c=p[i]; v=v*16+(u32)(c<='9'?c-'0':(c|32)-'a'+10); } return v; }
static FsNode *mkpath(const char *path,int want_parent_only,const char **leaf){
    FsNode *cur2=fs_root(); char part[FS_NAME]; const char *p=path;
    for(;;){
        while(*p=='/') p++;
        int n=0; while(p[n]&&p[n]!='/') n++;
        if(!n) return cur2;
        if(n>=FS_NAME) return 0;
        memcpy(part,p,(size_t)n); part[n]=0;
        int last=p[n]==0;
        if(last&&want_parent_only){ *leaf=p; return cur2; }
        FsNode *c=fs_find(cur2,part);
        if(!c){ c=fs_mkdir(cur2,part); if(!c) return 0; c->sys=1; }
        if(c->link) c=fs_resolve(cur2,part);
        if(!c||!c->dir) return 0;
        cur2=c; p+=n;
    }
}
/* cpio "newc": cuando un archivo tiene varios nombres (enlaces duros), solo el ultimo trae los datos; los anteriores vienen vacios.
 * Se anotan los vacios y, al llegar el que trae los datos, todos apuntan a ellos. */
struct HlPend { u32 ino,dev; FsNode *f; };
static struct HlPend *hl_tab; static int hl_n, hl_cap;
static void hl_add(u32 ino,u32 dev,FsNode *f){
    if(hl_n==hl_cap){ int nc=hl_cap?hl_cap*2:512; struct HlPend *t=kmalloc((size_t)nc*sizeof *t); if(!t) return; if(hl_tab){ memcpy(t,hl_tab,(size_t)hl_n*sizeof *t); kfree(hl_tab); } hl_tab=t; hl_cap=nc; }
    hl_tab[hl_n].ino=ino; hl_tab[hl_n].dev=dev; hl_tab[hl_n].f=f; hl_n++;
}
static int hl_fill(u32 ino,u32 dev,const char *data,int fsz){
    int k=0; for(int i=0;i<hl_n;i++){ if(hl_tab[i].ino==ino&&hl_tab[i].dev==dev){ fs_adopt(hl_tab[i].f,data,fsz); k++; } else hl_tab[i-k]=hl_tab[i]; }
    hl_n-=k; return k;
}
void rootfs_load(u32 mods_n,u32 mods_addr){
    struct mod { u32 s,e,c,pad; } *m=(struct mod*)(uintptr_t)mods_addr; int files=0, links=0, hard=0; u64 bytes=0;
    for(u32 i=0;i<mods_n;i++){
        const u8 *p=(const u8*)(uintptr_t)m[i].s, *end=(const u8*)(uintptr_t)m[i].e;
        while(p+110<=end && p[0]=='0'&&p[1]=='7'&&p[2]=='0'&&p[3]=='7'&&p[4]=='0'&&p[5]=='1'){
            u32 mode=hex8(p+14), fsz=hex8(p+54), nsz=hex8(p+94), nlink=hex8(p+38), ino=hex8(p+6), dev=hex8(p+62)*4096u+hex8(p+70);
            const char *name=(const char*)p+110; const u8 *data=(const u8*)(((uintptr_t)p+110+nsz+3)&~3ull); p=(const u8*)(((uintptr_t)data+fsz+3)&~3ull);
            if(!k_strcmp(name,"TRAILER!!!")) break;
            while(name[0]=='.'&&name[1]=='/'){ name+=2; }
            if(!k_strcmp(name,".")||!name[0]) continue;
            if((mode&0170000)==0040000){ mkpath(name,0,0); continue; }
            if((mode&0170000)==0120000){                                /* enlace simbolico: el contenido es la ruta destino */
                const char *leaf=0; FsNode *d=mkpath(name,1,&leaf); if(!d||!leaf||k_strlen(leaf)>=FS_NAME||fsz>=300) continue;
                char tg[300]; memcpy(tg,data,fsz); tg[fsz]=0;
                FsNode *old=fs_find(d,leaf); if(old) fs_remove(old);
                FsNode *f=fs_symlink(d,leaf,tg); if(f){ f->sys=1; links++; }
                continue; }
            if((mode&0170000)!=0100000) continue;                       /* dispositivos y otros especiales: omitidos */
            const char *leaf=0; FsNode *d=mkpath(name,1,&leaf); if(!d||!leaf||k_strlen(leaf)>=FS_NAME) continue;
            FsNode *f=fs_find(d,leaf); if(f&&f->dir) continue;
            if(!f) f=fs_create(d,leaf,"",0);
            if(f){ f->sys=1; files++;
                if(nlink>1&&fsz==0) hl_add(ino,dev,f);                                                  /* nombre extra de un archivo: los datos llegan despues */
                else{ fs_adopt(f,(const char*)data,(int)fsz); bytes+=fsz; if(nlink>1) hard+=hl_fill(ino,dev,(const char*)data,(int)fsz); } }   /* sin copiar: el archivo apunta a la memoria del modulo */
        }
    }
    dev_create(); etc_defaults(); refresh_resolv();
    kprintf("[ok] rootfs: %d archivos y %d enlaces (%lu KB, %d nombres extra) desde el modulo de arranque\n",files,links,bytes/1024,hard);
    if(hl_tab){ kfree(hl_tab); hl_tab=0; hl_n=hl_cap=0; }
}
