/* sys_tty.inc — la consola vista como terminal de Linux (tty): modo "linea" o "crudo", eco, Ctrl+C, ioctl de termios.
 * Los programas de Linux (bash, busybox sh, vi, ...) preguntan con ioctl(TCGETS) si estan en un terminal y lo ponen en modo crudo
 * para leer tecla por tecla. Aqui se guarda ese estado y se aplica a las teclas que llegan de la ventana de terminal (o del teclado). */

struct KTermios { u32 iflag, oflag, cflag, lflag; u8 line; u8 cc[19]; };
#define TL_ISIG   0x01
#define TL_ICANON 0x02
#define TL_ECHO   0x08
static struct KTermios tty={0x500,5,0x4BF,0x8A3B,0,{3,28,127,21,4,0,1,0,17,19,26,0,18,15,23,22,0,0,0}};
static u8 inl[256]; static int inl_n, inl_r, inl_ready, inl_eof;       /* linea en edicion (modo canonico) */
static u8 rawq[1024]; static int raw_h, raw_t;                          /* cola de teclas (modo crudo) */
static int raw_avail(void){ return (raw_h-raw_t+(int)sizeof rawq)%(int)sizeof rawq; }
static void raw_push(u8 c){ int n=(raw_h+1)%(int)sizeof rawq; if(n!=raw_t){ rawq[raw_h]=c; raw_h=n; } }
static int tty_input_ready(void){ return (tty.lflag&TL_ICANON)?(inl_ready&&(inl_r<inl_n||inl_eof)):raw_avail()>0; }
static void tty_reset_input(void){ inl_n=inl_r=inl_ready=inl_eof=0; raw_h=raw_t=0; }
static void tty_echo(const char *s,int n){ if(tty.lflag&TL_ECHO) k_write_console(s,n); }

/* senal para el programa (o programas) que corren "pegados" al terminal */
static void tty_signal(int sig){
    for(int i=0;i<MAXT;i++){ Task *t=&tasks[i]; if(t->state==T_FREE||t->state==T_ZOMBIE||!t->leader||!t->pr) continue;
        if(t->fg||t->bg==2) sig_queue_proc(t->pr,sig); }
}
static void stdin_char(int c){
    if(c>=0x400) c&=31;                                           /* Ctrl+letra */
    if((tty.lflag&TL_ISIG)&&c>=0&&c<32){
        int sig=c==tty.cc[0]?2:c==tty.cc[1]?3:0;
        if(sig){ tty_signal(sig); if(tty.lflag&TL_ECHO) k_write_console(sig==2?"^C\n":"^\\\n",sig==2?3:3); inl_n=inl_r=inl_ready=0; inl_eof=0; return; }
    }
    if(c>=0x11&&c<=0x16){                                         /* flechas / Re Pag / Av Pag de kbd.c */
        if(tty.lflag&TL_ICANON) return;
        static const char *seq[6]={"\033[A","\033[B","\033[D","\033[C","\033[5~","\033[6~"};
        const char *q=seq[c-0x11]; while(*q) raw_push((u8)*q++); return;
    }
    if(!(tty.lflag&TL_ICANON)){                                   /* crudo: cada tecla se entrega tal cual */
        if(c=='\b') c=127;
        raw_push((u8)c);
        if(tty.lflag&TL_ECHO){ if(c==127){ k_write_console("\b",1); } else if(c>=32||c=='\n'||c=='\t'){ char ch=(char)c; k_write_console(&ch,1); } }
        return;
    }
    if(inl_ready) return;                                         /* ya hay una linea completa esperando a ser leida */
    if(c=='\r') c='\n';
    if(c=='\b'||c==127||c==tty.cc[2]){
        if(inl_n>0){ while(inl_n>0&&(inl[inl_n-1]&0xC0)==0x80) inl_n--; inl_n--; tty_echo("\b",1); }
    }
    else if(c==tty.cc[3]){ while(inl_n>0){ while(inl_n>0&&(inl[inl_n-1]&0xC0)==0x80) inl_n--; inl_n--; tty_echo("\b",1); } }
    else if(c==tty.cc[4]){ inl_ready=1; if(!inl_n) inl_eof=1; }
    else if(c=='\n'){ if(inl_n<255) inl[inl_n++]='\n'; tty_echo("\n",1); inl_ready=1; }
    else if((c>=32||c=='\t')&&inl_n<254){ inl[inl_n++]=(u8)c; char ch=(char)c; tty_echo(&ch,1); }
}
static void stdin_pump(void){ int c; while((c=kgetc())>=0) stdin_char(c); }
void proc_stdin_char(int c){ stdin_char(c); }                     /* teclas que llegan de la ventana de terminal */

static u64 tty_ioctl(u64 req,u64 arg){
    switch(req){
    case 0x5401: return ucopy_to(CP,arg,&tty,sizeof tty)?RET_ERR(EFAULT):0;                          /* TCGETS */
    case 0x5402: case 0x5403: case 0x5404: { struct KTermios n; if(ucopy_from(CP,&n,arg,sizeof n)) return RET_ERR(EFAULT);
        tty=n; if(req==0x5404) tty_reset_input(); return 0; }                                           /* TCSETS / TCSETSW / TCSETSF */
    case 0x5413: { u16 ws[4]={24,80,0,0}; return ucopy_to(CP,arg,ws,8)?RET_ERR(EFAULT):0; }          /* TIOCGWINSZ */
    case 0x5414: return 0;                                                                            /* TIOCSWINSZ */
    case 0x540F: case 0x5429: { int v=cur->pid; return ucopy_to(CP,arg,&v,4)?RET_ERR(EFAULT):0; }     /* TIOCGPGRP / TIOCGSID */
    case 0x5410: case 0x540E: case 0x5422: case 0x5409: case 0x5415: case 0x5416: return 0;           /* TIOCSPGRP, TIOCSCTTY, TIOCNOTTY, TCSBRK... */
    case 0x540B: tty_reset_input(); return 0;                                                         /* TCFLSH */
    case 0x541B: { int v=(tty.lflag&TL_ICANON)?(inl_ready?inl_n-inl_r:0):raw_avail(); return ucopy_to(CP,arg,&v,4)?RET_ERR(EFAULT):0; }   /* FIONREAD */
    }
    return RET_ERR(ENOTTY);
}
