/* nettest.c — prueba de red, senales y E/S de eventos con glibc normal (se ejecuta dentro de kng).
 * Uso: nettest <puerto-tcp> <puerto-udp>   (el servidor de prueba corre en el PC anfitrion: 10.0.2.2) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <poll.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
static int bad, good;
#define SAY(...) do{ printf(__VA_ARGS__); fflush(stdout); }while(0)
#define CHECK(c,name) do{ if(c){ SAY("OK  %s\n",name); good++; } else { SAY("FALLO %s (errno=%d %s)\n",name,errno,strerror(errno)); bad++; } }while(0)

static volatile int got_usr1, got_alrm, got_chld, got_segv;
static void h_usr1(int s,siginfo_t *si,void *uc){ (void)uc; got_usr1=s*100+(si?1:0); }
static void h_alrm(int s){ got_alrm=s; }
static void h_chld(int s){ got_chld=s; }
static sigjmp_buf jb;
static void h_segv(int s,siginfo_t *si,void *uc){ (void)uc; got_segv=s; if(si&&si->si_addr==(void*)0x10) got_segv=1100; siglongjmp(jb,1); }

static int tcp_connect(const char *ip,int port){
    int s=socket(AF_INET,SOCK_STREAM,0); if(s<0) return -1;
    struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons((unsigned short)port); inet_pton(AF_INET,ip,&a.sin_addr);
    if(connect(s,(struct sockaddr*)&a,sizeof a)<0){ close(s); return -1; }
    return s;
}
static void test_signals(void){
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_sigaction=h_usr1; sa.sa_flags=SA_SIGINFO; sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1,&sa,0)==0,"sigaction SIGUSR1");
    kill(getpid(),SIGUSR1);
    CHECK(got_usr1==1001,"manejador de senal recibe SIGUSR1 con siginfo");
    /* bloqueo y pendiente */
    sigset_t m,old,pend; sigemptyset(&m); sigaddset(&m,SIGUSR1); got_usr1=0;
    sigprocmask(SIG_BLOCK,&m,&old); kill(getpid(),SIGUSR1);
    int blocked_ok=(got_usr1==0); sigpending(&pend); int pend_ok=sigismember(&pend,SIGUSR1);
    sigprocmask(SIG_SETMASK,&old,0);
    CHECK(blocked_ok&&pend_ok&&got_usr1==1001,"senal bloqueada queda pendiente y llega al desbloquear");
    /* alarm interrumpe pause */
    signal(SIGALRM,h_alrm); got_alrm=0; alarm(1);
    int r=pause();
    CHECK(r==-1&&errno==EINTR&&got_alrm==SIGALRM,"alarm(1) despierta pause() con EINTR");
    /* nanosleep interrumpido */
    got_alrm=0; alarm(1); struct timespec ts={5,0}; r=nanosleep(&ts,0);
    CHECK(r==-1&&errno==EINTR,"nanosleep interrumpido por senal");
    /* SIGCHLD */
    signal(SIGCHLD,h_chld); got_chld=0; pid_t p=fork(); if(p==0) _exit(5);
    int st=0; waitpid(p,&st,0);
    CHECK(got_chld==SIGCHLD&&WEXITSTATUS(st)==5,"SIGCHLD al terminar un hijo");
    signal(SIGCHLD,SIG_DFL);
    /* SIGSEGV con manejador y recuperacion */
    memset(&sa,0,sizeof sa); sa.sa_sigaction=h_segv; sa.sa_flags=SA_SIGINFO|SA_NODEFER; sigaction(SIGSEGV,&sa,0);
    if(sigsetjmp(jb,1)==0){ volatile int *bad_p=(int*)0x10; *bad_p=1; }
    CHECK(got_segv==1100,"SIGSEGV entregado al manejador (direccion de fallo correcta) y programa sigue");
    signal(SIGSEGV,SIG_DFL);
    /* SIGPIPE ignorada -> EPIPE */
    signal(SIGPIPE,SIG_IGN); int pf[2]; pipe(pf); close(pf[0]); errno=0; r=(int)write(pf[1],"x",1);
    CHECK(r==-1&&errno==EPIPE,"escribir en pipe sin lector devuelve EPIPE (SIGPIPE ignorada)");
    close(pf[1]);
    /* senal por omision termina al hijo */
    p=fork(); if(p==0){ signal(SIGPIPE,SIG_DFL); for(;;) pause(); }
    usleep(200000); kill(p,SIGTERM); waitpid(p,&st,0);
    CHECK(WIFSIGNALED(st)&&WTERMSIG(st)==SIGTERM,"kill(SIGTERM) termina un proceso sin manejador");
    /* estado de coma flotante se conserva en el manejador */
    volatile double fx=1.5; kill(getpid(),SIGUSR1); fx=fx*2.0; CHECK(fx==3.0,"coma flotante intacta tras un manejador");
}
static void test_events(void){
    int ev=eventfd(0,EFD_NONBLOCK); uint64_t v=3; write(ev,&v,8); v=0; ssize_t r=read(ev,&v,8);
    CHECK(r==8&&v==3,"eventfd escribir/leer");
    errno=0; r=read(ev,&v,8); CHECK(r==-1&&errno==EAGAIN,"eventfd vacio no bloqueante -> EAGAIN");
    int tf=timerfd_create(CLOCK_MONOTONIC,0); struct itimerspec its; memset(&its,0,sizeof its); its.it_value.tv_nsec=200*1000*1000;
    timerfd_settime(tf,0,&its,0); uint64_t exp=0; r=read(tf,&exp,8);
    CHECK(r==8&&exp>=1,"timerfd espera y dispara");
    int pf[2]; pipe(pf); int ep=epoll_create1(0); struct epoll_event e; e.events=EPOLLIN; e.data.u64=77; epoll_ctl(ep,EPOLL_CTL_ADD,pf[0],&e);
    struct epoll_event out[4]; int n=epoll_wait(ep,out,4,100); CHECK(n==0,"epoll_wait sin datos agota el tiempo");
    write(pf[1],"hi",2); n=epoll_wait(ep,out,4,1000); CHECK(n==1&&out[0].data.u64==77&&(out[0].events&EPOLLIN),"epoll avisa que el pipe tiene datos");
    struct pollfd pfd={pf[0],POLLIN,0}; n=poll(&pfd,1,0); CHECK(n==1&&(pfd.revents&POLLIN),"poll avisa lectura lista");
    fd_set rs; FD_ZERO(&rs); FD_SET(pf[0],&rs); struct timeval tv={0,0}; n=select(pf[0]+1,&rs,0,0,&tv); CHECK(n==1&&FD_ISSET(pf[0],&rs),"select avisa lectura lista");
    /* poll interrumpido por alarma */
    got_alrm=0; signal(SIGALRM,h_alrm); alarm(1); int p2[2]; pipe(p2); struct pollfd q={p2[0],POLLIN,0}; errno=0; n=poll(&q,1,5000);
    CHECK(n==-1&&errno==EINTR,"poll con espera larga interrumpido por alarma");
    close(ev); close(tf); close(ep);
}
static void test_local(void){
    /* socketpair */
    int sv[2]; CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,sv)==0,"socketpair");
    write(sv[0],"abc",3); char b[16]={0}; ssize_t r=read(sv[1],b,sizeof b); CHECK(r==3&&!strcmp(b,"abc"),"socketpair transporta datos");
    /* paso de descriptores */
    int pf[2]; pipe(pf);
    struct msghdr mh; struct iovec io; char dummy='x'; char cbuf[CMSG_SPACE(sizeof(int))]; memset(&mh,0,sizeof mh); memset(cbuf,0,sizeof cbuf);
    io.iov_base=&dummy; io.iov_len=1; mh.msg_iov=&io; mh.msg_iovlen=1; mh.msg_control=cbuf; mh.msg_controllen=sizeof cbuf;
    struct cmsghdr *c=CMSG_FIRSTHDR(&mh); c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS; c->cmsg_len=CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(c),&pf[1],sizeof(int));
    CHECK(sendmsg(sv[0],&mh,0)==1,"sendmsg con SCM_RIGHTS");
    struct msghdr m2; char d2; struct iovec i2={&d2,1}; char cb2[CMSG_SPACE(sizeof(int))]; memset(&m2,0,sizeof m2); m2.msg_iov=&i2; m2.msg_iovlen=1; m2.msg_control=cb2; m2.msg_controllen=sizeof cb2;
    r=recvmsg(sv[1],&m2,0); int newfd=-1; struct cmsghdr *c2=CMSG_FIRSTHDR(&m2); if(c2&&c2->cmsg_type==SCM_RIGHTS) memcpy(&newfd,CMSG_DATA(c2),sizeof(int));
    CHECK(r==1&&newfd>=0,"recvmsg recibe un descriptor");
    if(newfd>=0){ write(newfd,"zz",2); char z[4]={0}; r=read(pf[0],z,2); CHECK(r==2&&z[0]=='z',"el descriptor recibido escribe en el pipe original"); }
    /* AF_UNIX con nombre */
    unlink("/tmp/kng-test.sock");
    int ls=socket(AF_UNIX,SOCK_STREAM,0); struct sockaddr_un ua; memset(&ua,0,sizeof ua); ua.sun_family=AF_UNIX; strcpy(ua.sun_path,"/tmp/kng-test.sock");
    CHECK(bind(ls,(struct sockaddr*)&ua,sizeof ua)==0&&listen(ls,4)==0,"AF_UNIX bind+listen");
    pid_t p=fork();
    if(p==0){ int cs=socket(AF_UNIX,SOCK_STREAM,0); if(connect(cs,(struct sockaddr*)&ua,sizeof ua)<0) _exit(2); write(cs,"hola unix",9); char bb[16]; ssize_t n=read(cs,bb,sizeof bb); _exit(n==5?0:3); }
    int as=accept(ls,0,0); char ub[32]={0}; r=read(as,ub,sizeof ub); write(as,"gracias"+0,5); int st; waitpid(p,&st,0);
    CHECK(r==9&&!strncmp(ub,"hola unix",9)&&WEXITSTATUS(st)==0,"AF_UNIX cliente/servidor entre procesos");
    close(as); close(ls);
    /* TCP por la interfaz local (127.0.0.1) */
    int srv=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons(5555); a.sin_addr.s_addr=htonl(INADDR_ANY);
    CHECK(bind(srv,(struct sockaddr*)&a,sizeof a)==0&&listen(srv,4)==0,"TCP bind+listen en 5555");
    p=fork();
    if(p==0){ int cs=tcp_connect("127.0.0.1",5555); if(cs<0) _exit(2); write(cs,"ping local",10); char bb[16]={0}; ssize_t n=read(cs,bb,sizeof bb); _exit(n==4&&!strncmp(bb,"pong",4)?0:3); }
    struct sockaddr_in pa; socklen_t pl=sizeof pa; int cs2=accept(srv,(struct sockaddr*)&pa,&pl); char tb[32]={0}; r=read(cs2,tb,sizeof tb); write(cs2,"pong",4); waitpid(p,&st,0);
    CHECK(r==10&&!strncmp(tb,"ping local",10)&&WEXITSTATUS(st)==0,"TCP 127.0.0.1 cliente/servidor entre procesos");
    close(cs2); close(srv);
}
static void test_net(int tcp_port,int udp_port){
    /* TCP: peticion HTTP corta */
    int s=tcp_connect("10.0.2.2",tcp_port); CHECK(s>=0,"TCP connect a 10.0.2.2");
    if(s>=0){
        const char *rq="GET /hola HTTP/1.0\r\nHost: x\r\n\r\n"; CHECK(write(s,rq,strlen(rq))==(ssize_t)strlen(rq),"TCP send");
        char buf[4096]; int tot=0; ssize_t n; while((n=read(s,buf+tot,sizeof buf-1-tot))>0) tot+=(int)n; buf[tot]=0;
        CHECK(tot>0&&strstr(buf,"KNG-HTTP-OK"),"TCP recibe la respuesta hasta el cierre (EOF)");
        close(s);
    }
    /* TCP: descarga grande (ventana, segmentos, retransmision) */
    s=tcp_connect("10.0.2.2",tcp_port);
    if(s>=0){
        const char *rq="GET /big HTTP/1.0\r\n\r\n"; write(s,rq,strlen(rq));
        static char big[600000]; int tot=0; ssize_t n; while(tot<(int)sizeof big-1&&(n=read(s,big+tot,sizeof big-1-tot))>0) tot+=(int)n;
        char *body=strstr(big,"\r\n\r\n"); long bl=body?tot-(long)(body+4-big):-1; unsigned sum=0; if(body) for(long i=0;i<bl;i++) sum=sum*31+(unsigned char)body[4+i];
        SAY("    descarga grande: %d bytes en total, cuerpo %ld, suma %u\n",tot,bl,sum);
        CHECK(bl==400000&&sum==1491745728u,"TCP descarga de 400000 bytes con suma correcta");
        close(s);
    } else CHECK(0,"TCP segunda conexion");
    /* conexion rechazada */
    errno=0; int rs=tcp_connect("10.0.2.2",1); CHECK(rs<0&&(errno==ECONNREFUSED||errno==ETIMEDOUT||errno==ECONNRESET),"connect a puerto cerrado falla");
    /* connect no bloqueante + poll */
    int ns=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0); struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons((unsigned short)tcp_port); inet_pton(AF_INET,"10.0.2.2",&a.sin_addr);
    int cr=connect(ns,(struct sockaddr*)&a,sizeof a); int e1=errno; struct pollfd pf={ns,POLLOUT,0}; int pr=poll(&pf,1,5000); int soerr=-1; socklen_t sl=sizeof soerr; getsockopt(ns,SOL_SOCKET,SO_ERROR,&soerr,&sl);
    CHECK(cr<0&&e1==EINPROGRESS&&pr==1&&soerr==0,"connect no bloqueante + poll(POLLOUT) + SO_ERROR");
    close(ns);
    /* UDP */
    int u=socket(AF_INET,SOCK_DGRAM,0); struct sockaddr_in ua; memset(&ua,0,sizeof ua); ua.sin_family=AF_INET; ua.sin_port=htons((unsigned short)udp_port); inet_pton(AF_INET,"10.0.2.2",&ua.sin_addr);
    sendto(u,"ping-udp",8,0,(struct sockaddr*)&ua,sizeof ua);
    struct pollfd up={u,POLLIN,0}; int pn=poll(&up,1,3000); char ub[64]={0}; struct sockaddr_in from; socklen_t fl=sizeof from; ssize_t r=-1; if(pn==1) r=recvfrom(u,ub,sizeof ub,0,(struct sockaddr*)&from,&fl);
    CHECK(r==13&&!strncmp(ub,"eco:ping-udp",12)&&ntohs(from.sin_port)==udp_port,"UDP enviar y recibir eco");
    close(u);
    /* resolucion de nombres */
    struct addrinfo hints,*res=0; memset(&hints,0,sizeof hints); hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    int g=getaddrinfo("localhost",0,&hints,&res); CHECK(g==0&&res&&((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr==htonl(INADDR_LOOPBACK),"getaddrinfo(localhost) por /etc/hosts"); if(res) freeaddrinfo(res);
    res=0; g=getaddrinfo("github.com",0,&hints,&res);
    if(g==0&&res){ char ipb[32]; inet_ntop(AF_INET,&((struct sockaddr_in*)res->ai_addr)->sin_addr,ipb,sizeof ipb); SAY("OK  getaddrinfo(github.com) por DNS real -> %s\n",ipb); good++; freeaddrinfo(res); }
    else SAY("INFO getaddrinfo(github.com) no resolvio (%s); depende de la red del anfitrion\n",gai_strerror(g));
}
static void test_files(void){
    char b[4096]; int fd=open("/proc/self/status",O_RDONLY); ssize_t n=fd>=0?read(fd,b,sizeof b-1):-1; if(n>0) b[n]=0; CHECK(n>0&&strstr(b,"Name:"),"/proc/self/status se puede leer"); if(fd>=0) close(fd);
    fd=open("/proc/cpuinfo",O_RDONLY); n=fd>=0?read(fd,b,sizeof b-1):-1; if(n>0) b[n]=0; CHECK(n>0&&strstr(b,"processor"),"/proc/cpuinfo se puede leer"); if(fd>=0) close(fd);
    fd=open("/proc/meminfo",O_RDONLY); n=fd>=0?read(fd,b,sizeof b-1):-1; if(n>0) b[n]=0; CHECK(n>0&&strstr(b,"MemTotal"),"/proc/meminfo se puede leer"); if(fd>=0) close(fd);
    fd=open("/dev/urandom",O_RDONLY); unsigned char r1[16],r2[16]; n=fd>=0?read(fd,r1,16):-1; ssize_t n2=fd>=0?read(fd,r2,16):-1; CHECK(n==16&&n2==16&&memcmp(r1,r2,16)!=0,"/dev/urandom da bytes distintos"); if(fd>=0) close(fd);
    fd=open("/dev/null",O_WRONLY); CHECK(fd>=0&&write(fd,"x",1)==1,"/dev/null acepta escrituras"); if(fd>=0) close(fd);
    char lk[128]; n=readlink("/proc/self/exe",lk,sizeof lk-1); if(n>0) lk[n]=0; CHECK(n>0&&strstr(lk,"nettest"),"readlink(/proc/self/exe)");
}
int main(int argc,char **argv){
    int tp=argc>1?atoi(argv[1]):18080, up=argc>2?atoi(argv[2]):18081;
    SAY("== nettest: senales ==\n"); test_signals();
    SAY("== nettest: eventos (epoll, poll, select, eventfd, timerfd) ==\n"); test_events();
    SAY("== nettest: /proc y /dev ==\n"); test_files();
    SAY("== nettest: sockets locales ==\n"); test_local();
    SAY("== nettest: red (servidor de prueba en 10.0.2.2) ==\n"); test_net(tp,up);
    SAY("nettest resultado: %d bien, %d mal -> %s\n",good,bad,bad?"HAY FALLOS":"TODO OK");
    return bad?1:0;
}
