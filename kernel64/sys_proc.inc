/* sys_proc.inc — archivos generados al abrirlos: /proc/... y /sys/... (se incluye dentro de proc64.c).
 * Los programas de Linux los leen para saber cuantos nucleos hay, cuanta memoria, su propia linea de comandos, etc. */
uint32_t kram_kb(void);
void kheap_info(u64 *used,u64 *freeb);

static void pf_s(char *o,int max,int *n,const char *s){ while(*s&&*n<max-1) o[(*n)++]=*s++; o[*n]=0; }
static void pf_u(char *o,int max,int *n,u64 v){ char t[24]; int k=0; if(!v) t[k++]='0'; while(v){ t[k++]=(char)('0'+v%10); v/=10; } while(k&&*n<max-1) o[(*n)++]=t[--k]; o[*n]=0; }
static void pf_x(char *o,int max,int *n,u64 v){ char t[20]; int k=0; if(!v) t[k++]='0'; while(v){ int d=(int)(v&15); t[k++]=(char)(d<10?'0'+d:'a'+d-10); v>>=4; } while(k&&*n<max-1) o[(*n)++]=t[--k]; o[*n]=0; }
#define PS(s)  pf_s(o,max,&n,(s))
#define PU(v)  pf_u(o,max,&n,(u64)(v))
#define PX(v)  pf_x(o,max,&n,(u64)(v))

static Task *task_by_pid(int pid){ for(int i=0;i<MAXT;i++) if(tasks[i].state!=T_FREE&&tasks[i].leader&&tasks[i].pid==pid) return &tasks[i]; return 0; }
static u64 proc_rss_kb(Proc *pr){ u64 t=0; for(int i=0;i+1<pr->slab_n;i++) t+=(u64)slab_cap(i); if(pr->slab_n) t+=(u64)pr->slab_used; return t*4; }

static int gen_proc_pid(Task *tk,const char *rest,char *o,int max){
    int n=0; o[0]=0; Proc *pr=tk->pr; if(!pr) return -1;
    if(!k_strcmp(rest,"status")){
        PS("Name:\t"); PS(tk->name); PS("\nState:\tR (running)\nTgid:\t"); PU(tk->pid); PS("\nPid:\t"); PU(tk->pid); PS("\nPPid:\t"); PU(tk->ppid);
        PS("\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nThreads:\t"); PU(pr->nthreads); PS("\nVmSize:\t"); PU(proc_rss_kb(pr)+4096); PS(" kB\nVmRSS:\t"); PU(proc_rss_kb(pr));
        PS(" kB\nSigQ:\t0/1024\nSigBlk:\t"); PX(tk->sigmask); PS("\nCpus_allowed_list:\t0\n"); return n; }
    if(!k_strcmp(rest,"stat")){
        PU(tk->pid); PS(" ("); PS(tk->name); PS(") R "); PU(tk->ppid); PS(" "); PU(tk->pid); PS(" "); PU(tk->pid); PS(" 0 -1 0 0 0 0 0 0 0 0 0 20 0 "); PU(pr->nthreads);
        PS(" 0 0 "); PU((proc_rss_kb(pr)+4096)*1024); PS(" "); PU(proc_rss_kb(pr)/4); PS(" 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n"); return n; }
    if(!k_strcmp(rest,"statm")){ PU((proc_rss_kb(pr)+4096)/4); PS(" "); PU(proc_rss_kb(pr)/4); PS(" 0 0 0 0 0\n"); return n; }
    if(!k_strcmp(rest,"cmdline")){ int l=k_strlen(pr->exe); for(int i=0;i<l&&n<max-2;i++) o[n++]=pr->exe[i]; o[n++]=0; o[n]=0; return n; }
    if(!k_strcmp(rest,"environ")) return 0;
    if(!k_strcmp(rest,"maps")){
        PS("555555554000-5555555c0000 r-xp 00000000 00:00 0 "); PS(pr->exe); PS("\n");
        PX(pr->brk0); PS("-"); PX(pr->brk); PS(" rw-p 00000000 00:00 0 [heap]\n");
        PX(STACK_TOP-STACK_SIZE); PS("-"); PX(STACK_TOP); PS(" rw-p 00000000 00:00 0 [stack]\n"); return n; }
    if(!k_strcmp(rest,"limits")){ PS("Limit                     Soft Limit           Hard Limit           Units\nMax open files            64                   64                   files\nMax stack size            8388608              unlimited            bytes\n"); return n; }
    if(!k_strcmp(rest,"cgroup")){ PS("0::/\n"); return n; }
    if(!k_strcmp(rest,"mountinfo")||!k_strcmp(rest,"mounts")){ PS("rootfs / rootfs rw 0 0\n"); return n; }
    if(!k_strcmp(rest,"oom_score_adj")||!k_strcmp(rest,"oom_adj")){ PS("0\n"); return n; }
    if(!k_strcmp(rest,"comm")){ PS(tk->name); PS("\n"); return n; }
    return -1;
}
/* devuelve el tamano del texto generado o -1 si ese archivo no existe */
static int gen_proc(const char *path,char *o,int max){
    int n=0; o[0]=0;
    if(k_startswith(path,"/proc/")){
        const char *p=path+6;
        if(*p>='0'&&*p<='9'){ int pid=0; while(*p>='0'&&*p<='9') pid=pid*10+(*p++-'0'); if(*p=='/') p++; Task *t=task_by_pid(pid); return t?gen_proc_pid(t,p,o,max):-1; }
        if(k_startswith(p,"self/")||!k_strcmp(p,"self")||k_startswith(p,"thread-self/")){ while(*p&&*p!='/') p++; if(*p=='/') p++; return gen_proc_pid(cur,p,o,max); }
        if(!k_strcmp(p,"cpuinfo")){
            u32 a,b,c,d; __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0)); char v[13]; memcpy(v,&b,4); memcpy(v+4,&d,4); memcpy(v+8,&c,4); v[12]=0;
            PS("processor\t: 0\nvendor_id\t: "); PS(v); PS("\ncpu family\t: 6\nmodel\t\t: 60\nmodel name\t: Koenigreich virtual CPU\nstepping\t: 1\ncpu MHz\t\t: 2000.000\ncache size\t: 4096 KB\nphysical id\t: 0\nsiblings\t: 1\ncore id\t\t: 0\ncpu cores\t: 1\nfpu\t\t: yes\n");
            PS("flags\t\t: fpu de tsc msr pae cx8 apic sep cmov pat clflush mmx fxsr sse sse2 syscall nx lm constant_tsc nopl\nbogomips\t: 4000.00\nclflush size\t: 64\naddress sizes\t: 36 bits physical, 48 bits virtual\n\n"); return n; }
        if(!k_strcmp(p,"meminfo")){
            u64 used,fr; kheap_info(&used,&fr); u64 tot=kram_kb(); u64 fk=fr/1024;
            PS("MemTotal:       "); PU(tot); PS(" kB\nMemFree:        "); PU(fk); PS(" kB\nMemAvailable:   "); PU(fk); PS(" kB\nBuffers:        0 kB\nCached:         0 kB\nSwapCached:     0 kB\nSwapTotal:      0 kB\nSwapFree:       0 kB\n"); return n; }
        if(!k_strcmp(p,"version")){ PS("Linux version 6.1.0-kng (kng@konigreich) (gcc) #1 SMP kng-compat\n"); return n; }
        if(!k_strcmp(p,"uptime")){ u32 k=kticks(); PU(k/100); PS("."); PU((k%100)/10); PU(k%10); PS(" "); PU(k/100); PS(".00\n"); return n; }
        if(!k_strcmp(p,"loadavg")){ PS("0.00 0.00 0.00 1/"); int c=0; for(int i=0;i<MAXT;i++) if(tasks[i].state!=T_FREE&&tasks[i].state!=T_ZOMBIE) c++; PU(c); PS(" "); PU(next_tid); PS("\n"); return n; }
        if(!k_strcmp(p,"stat")){ u32 k=kticks(); PS("cpu  "); PU(k); PS(" 0 0 0 0 0 0 0 0 0\ncpu0 "); PU(k); PS(" 0 0 0 0 0 0 0 0 0\nbtime 1700000000\nprocesses "); PU(next_tid); PS("\n"); return n; }
        if(!k_strcmp(p,"filesystems")){ PS("nodev\trootfs\nnodev\tproc\nnodev\ttmpfs\n"); return n; }
        if(!k_strcmp(p,"mounts")){ PS("rootfs / rootfs rw 0 0\nproc /proc proc rw 0 0\ntmpfs /dev tmpfs rw 0 0\n"); return n; }
        if(!k_strcmp(p,"sys/kernel/osrelease")){ PS("6.1.0-kng\n"); return n; }
        if(!k_strcmp(p,"sys/kernel/ostype")){ PS("Linux\n"); return n; }
        if(!k_strcmp(p,"sys/kernel/hostname")){ PS("kng\n"); return n; }
        if(!k_strcmp(p,"sys/kernel/pid_max")){ PS("32768\n"); return n; }
        if(!k_strcmp(p,"sys/kernel/threads-max")){ PS("4096\n"); return n; }
        if(!k_strcmp(p,"sys/kernel/random/boot_id")){ PS("6b6e6700-0000-4000-8000-"); PX(0x1000000000ull|kticks()); PS("\n"); return n; }
        if(!k_strcmp(p,"sys/vm/overcommit_memory")){ PS("1\n"); return n; }
        if(!k_strcmp(p,"sys/vm/max_map_count")){ PS("65530\n"); return n; }
        if(!k_strcmp(p,"sys/kernel/yama/ptrace_scope")){ PS("1\n"); return n; }
        return -1;
    }
    if(k_startswith(path,"/sys/devices/system/cpu/")){
        const char *p=path+24;
        if(!k_strcmp(p,"online")||!k_strcmp(p,"present")||!k_strcmp(p,"possible")||!k_strcmp(p,"kernel_max")){ PS(!k_strcmp(p,"kernel_max")?"0\n":"0\n"); return n; }
        return -1;
    }
    if(!k_strcmp(path,"/sys/kernel/mm/transparent_hugepage/enabled")){ PS("never\n"); return n; }
    return -1;
}
static int is_virtual_path(const char *path){ return k_startswith(path,"/proc/")||k_startswith(path,"/sys/"); }
static u64 virt_open(const char *path,u64 flags){
    char full[200]; if(abs_path(path,full,sizeof full)) return RET_ERR(ENOENT);
    if(!is_virtual_path(full)) return RET_ERR(ENOENT);
    char *buf=kmalloc(8192); if(!buf) return RET_ERR(ENOMEM);
    int n=gen_proc(full,buf,8192); if(n<0){ kfree(buf); return RET_ERR(ENOENT); }
    int fd=fd_alloc(CP,3); if(fd<0){ kfree(buf); return RET_ERR(EMFILE); }
    KObj *o=obj_new(11); if(!o){ kfree(buf); return RET_ERR(ENOMEM); }
    o->data=buf; o->size=n;
    FD *f=&CP->fd[fd]; memset(f,0,sizeof *f); f->type=11; f->obj=o; f->flags=(int)flags; f->cloexec=(flags&0x80000)?1:0; return (u64)fd;
}
