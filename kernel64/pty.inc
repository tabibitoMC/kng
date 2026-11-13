/* ---------- pseudoterminales: /dev/ptmx (maestro) y /dev/pts/N (esclavo) ----------
 * Lo usan xterm y otros emuladores de terminal: el emulador lee/escribe el maestro y el shell corre en el esclavo.
 * Disciplina de linea minima: modo canonico con eco y borrado, ICRNL en la entrada, ONLCR en la salida, Ctrl+C. */
#define NPTY 8
#define PTYQ 16384
struct PtyQ { u8 b[PTYQ]; int r, n; };
struct Pty { int used, mref, sref, sopened, locked, eof; struct KTermios tio; u16 ws[4]; struct PtyQ in, out; u8 line[1024]; int ln; FsNode *node; };
static struct Pty ptys[NPTY];
#define DEV_PTY0 32                                      /* aux = DEV_PTY0 + 2*i (maestro) o +1 (esclavo) */
static int is_pty(int dev){ return dev>=DEV_PTY0&&dev<DEV_PTY0+2*NPTY; }
static struct Pty *pty_of(int dev){ return &ptys[(dev-DEV_PTY0)/2]; }
static int pty_master(int dev){ return ((dev-DEV_PTY0)&1)==0; }
static int q_space(struct PtyQ *q){ return PTYQ-q->n; }
static void q_put(struct PtyQ *q,u8 c){ if(q->n<PTYQ){ q->b[(q->r+q->n)%PTYQ]=c; q->n++; } }
static int q_get(struct PtyQ *q){ if(!q->n) return -1; int c=q->b[q->r]; q->r=(q->r+1)%PTYQ; q->n--; return c; }
static void pty_out(struct Pty *p,u8 c){ if((p->tio.oflag&1)&&(p->tio.oflag&4)&&c=='\n') q_put(&p->out,'\r'); q_put(&p->out,c); }   /* OPOST|ONLCR */
static void pty_echo(struct Pty *p,u8 c){ if(!(p->tio.lflag&TL_ECHO)) return; if(c<32&&c!='\n'&&c!='\t'&&c!='\r'){ q_put(&p->out,'^'); q_put(&p->out,(u8)(c+64)); } else pty_out(p,c); }
static void pty_sig(int slave,int sig){
    for(int i=0;i<MAXT;i++){ Task *t=&tasks[i]; if(t->state==T_FREE||t->state==T_ZOMBIE||!t->leader||!t->pr) continue;
        for(int k=0;k<MAXFD;k++) if(t->pr->fd[k].type==10&&t->pr->fd[k].aux==slave){ sig_queue_proc(t->pr,sig); break; } }
}
static void pty_input(struct Pty *p,int slave,u8 c){              /* tecla que el emulador manda al programa */
    struct KTermios *t=&p->tio;
    if((t->iflag&0x100)&&c=='\r') c='\n';                      /* ICRNL */
    else if((t->iflag&0x40)&&c=='\n') c='\r';                  /* INLCR */
    if(t->lflag&TL_ISIG){ int s=c==t->cc[0]?2:c==t->cc[1]?3:c==t->cc[10]?20:0;
        if(s){ pty_echo(p,c); p->ln=0; pty_sig(slave,s); return; } }
    if(t->lflag&TL_ICANON){
        if(c==t->cc[2]||c==8){ if(p->ln>0){ p->ln--; if(t->lflag&TL_ECHO){ q_put(&p->out,'\b'); q_put(&p->out,' '); q_put(&p->out,'\b'); } } return; }   /* VERASE */
        if(c==t->cc[3]){ while(p->ln>0){ p->ln--; if(t->lflag&TL_ECHO){ q_put(&p->out,'\b'); q_put(&p->out,' '); q_put(&p->out,'\b'); } } return; }     /* VKILL */
        if(c==t->cc[4]){ if(!p->ln) p->eof=1; for(int i=0;i<p->ln;i++) q_put(&p->in,p->line[i]); p->ln=0; return; }   /* VEOF */
        if(c=='\n'){ pty_echo(p,c); for(int i=0;i<p->ln;i++) q_put(&p->in,p->line[i]); q_put(&p->in,'\n'); p->ln=0; return; }
        if(p->ln<(int)sizeof p->line-1){ p->line[p->ln++]=c; pty_echo(p,c); }
        return;
    }
    q_put(&p->in,c); pty_echo(p,c);
}
static int pty_open_master(void){
    for(int i=0;i<NPTY;i++) if(!ptys[i].used){
        struct Pty *p=&ptys[i]; FsNode *keep=p->node; memset(p,0,sizeof *p); p->node=keep; p->used=1; p->mref=1; p->locked=1; p->tio=tty; p->ws[0]=24; p->ws[1]=80;
        FsNode *root=fs_root(), *dev=root?fs_find(root,"dev"):0, *pts=dev?fs_find(dev,"pts"):0;
        if(dev&&!pts){ pts=fs_mkdir(dev,"pts"); if(pts) pts->sys=1; }
        if(pts&&!p->node){ char nm[4]={(char)('0'+i),0}; FsNode *n=fs_find(pts,nm); if(!n) n=fs_create(pts,nm,"",0); if(n){ n->sys=1; n->dev=DEV_PTY0+2*i+1; } p->node=n; }
        return DEV_PTY0+2*i;
    }
    return -1;
}
static void pty_ref(int dev){ if(!is_pty(dev)) return; struct Pty *p=pty_of(dev); if(pty_master(dev)) p->mref++; else { p->sref++; p->sopened=1; } }
static void pty_unref(int dev){ if(!is_pty(dev)) return; struct Pty *p=pty_of(dev); if(pty_master(dev)){ if(--p->mref<=0) p->used=0; } else if(p->sref>0) p->sref--; }
static int pty_events(int dev){
    struct Pty *p=pty_of(dev); int e=0;
    if(pty_master(dev)){ if(p->out.n) e|=PIN; if(p->sopened&&p->sref<=0) e|=PHUP; if(q_space(&p->in)>1024) e|=POUT; }
    else { if(p->in.n||p->eof) e|=PIN; if(!p->used||p->mref<=0) e|=PHUP; if(q_space(&p->out)>1024) e|=POUT; }
    return e;
}
static int w_pty(Task *t){ FD *f=&t->pr->fd[t->pipe_fd]; if(f->type!=10||!is_pty(f->aux)) return 1; return pty_events(f->aux)&(PIN|PHUP); }
static int w_pty_out(Task *t){ FD *f=&t->pr->fd[t->pipe_fd]; if(f->type!=10||!is_pty(f->aux)) return 1; return pty_events(f->aux)&(POUT|PHUP); }
static u64 pty_read(FD *f,int fdn,u64 buf,u64 n){
    struct Pty *p=pty_of(f->aux); int m=pty_master(f->aux); u8 tmp[512];
    for(;;){
        struct PtyQ *q=m?&p->out:&p->in;
        if(q->n){ u64 d=0; int canon=!m&&(p->tio.lflag&TL_ICANON);
            while(d<n&&q->n){ u64 k=0; int stop=0; while(k<sizeof tmp&&d+k<n&&q->n){ int c=q_get(q); tmp[k++]=(u8)c; if(canon&&c=='\n'){ stop=1; break; } }
                if(ucopy_to(CP,buf+d,tmp,k)) return d?d:RET_ERR(EFAULT); d+=k; if(stop) break; }
            return d; }
        if(!m&&p->eof){ p->eof=0; return 0; }
        if(m&&p->sopened&&p->sref<=0) return RET_ERR(5);                      /* EIO: el programa del esclavo termino */
        if(!m&&(!p->used||p->mref<=0)) return 0;
        if(!m&&!(p->tio.lflag&TL_ICANON)&&p->tio.cc[6]==0) return 0;   /* VMIN=0: no esperar */
        if(f->flags&0x800) return RET_ERR(EAGAIN);
        cur->pipe_fd=fdn; if(block_fn(w_pty,0)==1) return RET_ERR(EINTR);
    }
}
static u64 pty_write(FD *f,int fdn,u64 buf,u64 n){
    struct Pty *p=pty_of(f->aux); int m=pty_master(f->aux); u8 tmp[512]; u64 d=0;
    while(d<n){
        if(m?q_space(&p->in)<1100:q_space(&p->out)<1100){
            if(!m&&p->mref<=0) return d?d:RET_ERR(5);
            if(f->flags&0x800) return d?d:RET_ERR(EAGAIN);
            cur->pipe_fd=fdn; if(block_fn(w_pty_out,0)==1) return d?d:RET_ERR(EINTR); continue; }
        u64 k=n-d; if(k>sizeof tmp) k=sizeof tmp; if(ucopy_from(CP,tmp,buf+d,k)) return d?d:RET_ERR(EFAULT);
        for(u64 i=0;i<k;i++){ if(m) pty_input(p,f->aux+1,tmp[i]); else pty_out(p,tmp[i]); }
        d+=k;
    }
    return n;
}
static u64 pty_ioctl(FD *f,u64 req,u64 arg){
    struct Pty *p=pty_of(f->aux); int idx=(f->aux-DEV_PTY0)/2;
    switch(req){
    case 0x5401: return ucopy_to(CP,arg,&p->tio,sizeof p->tio)?RET_ERR(EFAULT):0;                                 /* TCGETS */
    case 0x5402: case 0x5403: case 0x5404: { struct KTermios n; if(ucopy_from(CP,&n,arg,sizeof n)) return RET_ERR(EFAULT);
        int was=p->tio.lflag&TL_ICANON; p->tio=n;
        if(was&&!(n.lflag&TL_ICANON)){ for(int i=0;i<p->ln;i++) q_put(&p->in,p->line[i]); p->ln=0; }          /* lo escrito pasa a la cola cruda */
        return 0; }
    case 0x5413: return ucopy_to(CP,arg,p->ws,8)?RET_ERR(EFAULT):0;                                               /* TIOCGWINSZ */
    case 0x5414: { if(ucopy_from(CP,p->ws,arg,8)) return RET_ERR(EFAULT); pty_sig(DEV_PTY0+2*idx+1,28); return 0; }   /* TIOCSWINSZ + SIGWINCH */
    case 0x80045430: return ucopy_to(CP,arg,&idx,4)?RET_ERR(EFAULT):0;                                             /* TIOCGPTN */
    case 0x40045431: { int v=0; if(arg&&ucopy_from(CP,&v,arg,4)) return RET_ERR(EFAULT); p->locked=v!=0; return 0; }   /* TIOCSPTLCK */
    case 0x80045439: { int v=p->locked; return ucopy_to(CP,arg,&v,4)?RET_ERR(EFAULT):0; }                          /* TIOCGPTLCK */
    case 0x540F: case 0x5429: { int v=cur->pid; return ucopy_to(CP,arg,&v,4)?RET_ERR(EFAULT):0; }                 /* TIOCGPGRP / TIOCGSID */
    case 0x541B: { int v=pty_master(f->aux)?p->out.n:p->in.n; return ucopy_to(CP,arg,&v,4)?RET_ERR(EFAULT):0; }   /* FIONREAD */
    case 0x540B: if(!pty_master(f->aux)){ p->in.n=0; p->ln=0; } return 0;                                          /* TCFLSH */
    case 0x5410: case 0x540E: case 0x5422: case 0x5409: case 0x5415: case 0x5416: case 0x5425: case 0x5420: case 0x540A: return 0;
    }
    return RET_ERR(ENOTTY);
}
