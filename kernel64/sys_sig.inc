/* sys_sig.inc — senales para programas de usuario (se incluye dentro de proc64.c).
 *
 * Una senal se encola en la tarea (sigpend) y se entrega al salir al modo usuario: si hay manejador se arma un marco "rt_sigframe"
 * (ucontext + siginfo + estado SSE) en la pila del usuario y se salta al manejador; rt_sigreturn restaura todo. Sin manejador:
 * accion por omision (terminar o ignorar). Los errores de la CPU (SIGSEGV, SIGFPE, SIGILL) tambien pasan por aqui. */

#define SIGBIT(s) (1ull<<((s)-1))
#define SA_SIGINFO   0x4ull
#define SA_ONSTACK   0x08000000ull
#define SA_RESTART   0x10000000ull
#define SA_NODEFER   0x40000000ull
#define SA_RESETHAND 0x80000000ull
#define UNBLOCKABLE (SIGBIT(9)|SIGBIT(19))

static int sig_default_ignored(int s){ return s==17||s==23||s==28||s==18||s==19||s==20||s==21||s==22; }

/* encola 'sig' en la tarea 't' (sin entregarla todavia) */
static void sig_queue_task(Task *t,int sig){
    if(!t->pr||sig<1||sig>64) return;
    if(sig==9){ for(int i=0;i<MAXT;i++) if(tasks[i].pr==t->pr&&tasks[i].state!=T_FREE) tasks[i].killed=9; return; }
    struct SigAct *a=&t->pr->sa[sig];
    if(a->h==1) return;
    if(a->h==0&&sig_default_ignored(sig)) return;
    t->sigpend|=SIGBIT(sig);
}
/* senal dirigida al proceso: la recibe un hilo que no la tenga bloqueada */
static void sig_queue_proc(Proc *pr,int sig){
    Task *best=0,*any=0;
    for(int i=0;i<MAXT;i++){ Task *t=&tasks[i]; if(t->pr!=pr||t->state==T_FREE||t->state==T_ZOMBIE) continue; if(!any) any=t; if(!best&&!(t->sigmask&SIGBIT(sig))) best=t; }
    if(!best) best=any;
    if(best) sig_queue_task(best,sig);
}
static void sig_self(int sig){ sig_queue_task(cur,sig); }

static u8 sigbuf[1024];
static int sig_frame(struct regs *r,int sig,int code,u64 addr,int from_sys){
    Proc *pr=cur->pr; struct SigAct *a=&pr->sa[sig];
    u64 oldmask=cur->has_saved?cur->sigsaved:cur->sigmask; cur->has_saved=0;
    if(from_sys && (long)r->rax==-EINTR && (a->flags&SA_RESTART)){
        int nr=cur->sysnr; if(nr==0||nr==1||nr==19||nr==20||nr==43||nr==44||nr==45||nr==46||nr==47||nr==61||nr==288){ r->rax=(u64)nr; r->rip-=2; } }
    u64 top=r->rsp-128;
    if((a->flags&SA_ONSTACK)&&cur->alt_size&&!(r->rsp>=cur->alt_sp&&r->rsp<cur->alt_sp+cur->alt_size)) top=cur->alt_sp+cur->alt_size;
    top&=~15ull;
    u64 fp_va=(top-512)&~15ull, uc_va=fp_va-448, si_va=uc_va+320, sp=uc_va-8;
    u8 *b=sigbuf; memset(b,0,sizeof sigbuf);
    u64 *w=(u64*)(void*)b; w[0]=a->restorer;
    u8 *uc=b+8; *(u64*)(uc+16)=cur->alt_sp; *(u32*)(uc+24)=(r->rsp>=cur->alt_sp&&r->rsp<cur->alt_sp+cur->alt_size&&cur->alt_size)?1:0; *(u64*)(uc+32)=cur->alt_size;
    u64 *mc=(u64*)(void*)(uc+40);
    mc[0]=r->r8; mc[1]=r->r9; mc[2]=r->r10; mc[3]=r->r11; mc[4]=r->r12; mc[5]=r->r13; mc[6]=r->r14; mc[7]=r->r15;
    mc[8]=r->rdi; mc[9]=r->rsi; mc[10]=r->rbp; mc[11]=r->rbx; mc[12]=r->rdx; mc[13]=r->rax; mc[14]=r->rcx; mc[15]=r->rsp; mc[16]=r->rip; mc[17]=r->rflags;
    *(u16*)(void*)(uc+40+144)=0x33; *(u16*)(void*)(uc+40+150)=0x2B;
    mc[19]=r->err; mc[20]=r->int_no; mc[21]=oldmask; mc[22]=addr; mc[23]=fp_va;
    *(u64*)(uc+296)=oldmask;
    u8 *si=b+328; *(u32*)(si+0)=(u32)sig; *(u32*)(si+8)=(u32)code;
    if(addr||sig==11||sig==7||sig==4||sig==8) *(u64*)(si+16)=addr; else { *(u32*)(si+16)=(u32)cur->pid; *(u32*)(si+20)=0; }
    if(cur->state==T_READY){ __asm__ volatile("fxsave %0":"=m"(*(u8(*)[512])cur->fx)); }
    memcpy(b+456,cur->fx,512);
    if(ucopy_to(pr,sp,b,968)) return -1;
    r->rsp=sp; r->rip=a->h; r->rdi=(u64)sig; r->rsi=si_va; r->rdx=uc_va; r->rax=0; r->rflags&=~0x400ull;
    cur->sigmask|=a->mask&~UNBLOCKABLE; if(!(a->flags&SA_NODEFER)) cur->sigmask|=SIGBIT(sig);
    if(a->flags&SA_RESETHAND){ a->h=0; }
    return 0;
}
/* llamar al salir de cualquier entrada al kernel desde ring 3 */
static void sig_deliver(struct regs *r,int from_sys){
    for(;;){
        u64 p=cur->sigpend&~cur->sigmask; if(!p) break;
        int sig=1; while(!(p&SIGBIT(sig))) sig++;
        cur->sigpend&=~SIGBIT(sig);
        struct SigAct *a=&cur->pr->sa[sig];
        if(a->h==1) continue;
        if(a->h==0){ if(sig_default_ignored(sig)) continue; do_exit_group(-sig); }
        if(sig_frame(r,sig,sig==17?1:0,0,from_sys)){ kprintf("[proceso %d] no se pudo armar el marco de la senal %d en la pila -> terminado\n",cur->pid,sig); do_exit_group(-11); }
        return;                                          /* una por vez: las demas se entregan al volver del manejador */
    }
    if(cur->has_saved){ cur->sigmask=cur->sigsaved; cur->has_saved=0; }
}
/* error de CPU del programa: si tiene manejador se le avisa, si no se termina */
static int sig_fault(struct regs *r,int sig,u64 addr){
    struct SigAct *a=&cur->pr->sa[sig];
    if(a->h<=1||(cur->sigmask&SIGBIT(sig))) return -1;
    return sig_frame(r,sig,sig==11?1:sig==7?1:sig==8?1:1,addr,0);
}
static u64 sys_sigreturn(struct regs *r){
    u8 b[320]; u64 uc=r->rsp;
    if(ucopy_from(cur->pr,b,uc,320)){ kprintf("[proceso %d] rt_sigreturn con pila invalida -> terminado\n",cur->pid); do_exit_group(-11); }
    u64 *mc=(u64*)(void*)(b+40);
    r->r8=mc[0]; r->r9=mc[1]; r->r10=mc[2]; r->r11=mc[3]; r->r12=mc[4]; r->r13=mc[5]; r->r14=mc[6]; r->r15=mc[7];
    r->rdi=mc[8]; r->rsi=mc[9]; r->rbp=mc[10]; r->rbx=mc[11]; r->rdx=mc[12]; r->rax=mc[13]; r->rcx=mc[14]; r->rsp=mc[15]; r->rip=mc[16];
    r->rflags=(mc[17]&0x4DD5ull)|0x202; r->cs=0x23; r->ss=0x1B;
    cur->sigmask=*(u64*)(b+296)&~UNBLOCKABLE;
    u64 fp=mc[23]; if(fp&&!ucopy_from(cur->pr,cur->fx,fp,512)) __asm__ volatile("fxrstor %0"::"m"(*(u8(*)[512])cur->fx));
    return EXEC_DONE;
}

static u64 sys_rt_sigaction(u64 sig,u64 act,u64 oact,u64 size){
    if(sig<1||sig>64||sig==9||sig==19||size!=8) return RET_ERR(EINVAL);
    struct SigAct *a=&CP->sa[sig];
    if(oact&&ucopy_to(CP,oact,a,32)) return RET_ERR(EFAULT);
    if(act){ struct SigAct n; if(ucopy_from(CP,&n,act,32)) return RET_ERR(EFAULT); n.mask&=~UNBLOCKABLE; *a=n;
        if(n.h==1||(n.h==0&&sig_default_ignored((int)sig))) for(int i=0;i<MAXT;i++) if(tasks[i].pr==CP) tasks[i].sigpend&=~SIGBIT((int)sig); }
    return 0;
}
static u64 sys_sigprocmask(u64 how,u64 set,u64 oset,u64 size){
    if(size!=8) return RET_ERR(EINVAL);
    if(oset&&ucopy_to(CP,oset,&cur->sigmask,8)) return RET_ERR(EFAULT);
    if(set){ u64 m; if(ucopy_from(CP,&m,set,8)) return RET_ERR(EFAULT); m&=~UNBLOCKABLE;
        if(how==0) cur->sigmask|=m; else if(how==1) cur->sigmask&=~m; else if(how==2) cur->sigmask=m; else return RET_ERR(EINVAL); }
    return 0;
}
static u64 sys_sigsuspend(u64 maskva,u64 size){
    if(size!=8) return RET_ERR(EINVAL);
    u64 m; if(ucopy_from(CP,&m,maskva,8)) return RET_ERR(EFAULT);
    cur->sigsaved=cur->sigmask; cur->has_saved=1; cur->sigmask=m&~UNBLOCKABLE;
    while(!INTR()) block_fn(w_never,0);
    return RET_ERR(EINTR);
}
static u64 sys_pause(void){ while(!INTR()) block_fn(w_never,0); return RET_ERR(EINTR); }
static u64 sys_sigaltstack(u64 ss,u64 oss){
    if(oss){ u8 o[24]; memset(o,0,24); *(u64*)o=cur->alt_sp; *(u32*)(o+8)=(cur->alt_size?0:2); *(u64*)(o+16)=cur->alt_size; if(ucopy_to(CP,oss,o,24)) return RET_ERR(EFAULT); }
    if(ss){ u8 n[24]; if(ucopy_from(CP,n,ss,24)) return RET_ERR(EFAULT); u32 fl=*(u32*)(n+8);
        if(fl&2){ cur->alt_sp=0; cur->alt_size=0; } else { if(*(u64*)(n+16)<2048) return RET_ERR(12); cur->alt_sp=*(u64*)n; cur->alt_size=*(u64*)(n+16); } }
    return 0;
}
static u64 sys_alarm_set(u64 which,u64 newv,u64 oldv){          /* setitimer: solo ITIMER_REAL hace algo; las otras aceptan y no hacen nada */
    if(which!=0){ if(oldv){ u8 z[32]; memset(z,0,32); ucopy_to(CP,oldv,z,32); } return 0; }
    u32 now=kticks();
    if(oldv){ u64 v[4]; u64 left=CP->alarm_at?(u64)(int)((u32)CP->alarm_at-now):0; if((long)left<0) left=0; v[0]=CP->alarm_int/100; v[1]=(CP->alarm_int%100)*10000; v[2]=left/100; v[3]=(left%100)*10000; if(ucopy_to(CP,oldv,v,32)) return RET_ERR(EFAULT); }
    if(newv){ u64 v[4]; if(ucopy_from(CP,v,newv,32)) return RET_ERR(EFAULT);
        u64 val=v[2]*100+v[3]/10000, itv=v[0]*100+v[1]/10000; if(v[2]||v[3]) { if(!val) val=1; CP->alarm_at=(u64)(now+val); if(!CP->alarm_at) CP->alarm_at=1; } else CP->alarm_at=0;
        CP->alarm_int=itv; }
    return 0;
}
static u64 sys_alarm(u64 secs){
    u32 now=kticks(); u64 left=CP->alarm_at?(u64)(int)((u32)CP->alarm_at-now):0; if((long)left<0) left=0; left=(left+99)/100;
    if(secs){ CP->alarm_at=(u64)(now+secs*100); } else CP->alarm_at=0;
    CP->alarm_int=0; return left;
}
static u64 sys_kill(int pid,int sig,int tid_only,int tid){
    if(sig<0||sig>64) return RET_ERR(EINVAL);
    if(!tid_only&&pid<=0) pid=cur->pid;
    int found=0; Proc *done=0;
    for(int i=0;i<MAXT;i++){ Task *t=&tasks[i]; if(t->state==T_FREE||t->state==T_ZOMBIE) continue;
        if(tid_only?(t->tid!=tid):(t->pid!=pid)) continue;
        found=1;
        if(!sig) continue;
        if(tid_only) sig_queue_task(t,sig);
        else if(done!=t->pr){ done=t->pr; sig_queue_proc(t->pr,sig); }
    }
    return found?0:RET_ERR(ESRCH);
}
