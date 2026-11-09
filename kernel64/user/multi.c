/* multi.c — prueba de varios procesos e hilos con glibc normal: fork, wait, pipe, execve, pthreads, mutex, sleep, kill. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>
#define SAY(...) do{ printf(__VA_ARGS__); fflush(stdout); }while(0)
static int counter; static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static void *work(void *a){ for(int i=0;i<1000;i++){ pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); } return (void*)((long)a*2); }
int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"hijo")){ SAY("  [exec] programa nuevo cargado, argc=%d\n",argc); return 7; }
    int ok=0;
    pid_t p=fork();
    if(p==0){ SAY("  [hijo] hola desde fork, pid=%d\n",(int)getpid()); _exit(3); }
    int st=0; waitpid(p,&st,0);
    if(WIFEXITED(st)&&WEXITSTATUS(st)==3){ SAY("fork+wait OK (estado %d)\n",WEXITSTATUS(st)); ok++; } else SAY("fork+wait FALLO st=%d\n",st);
    int fd[2]; if(pipe(fd)){ SAY("pipe FALLO\n"); return 1; }
    p=fork();
    if(p==0){ close(fd[0]); const char *m="mensaje por pipe"; write(fd[1],m,strlen(m)); close(fd[1]); _exit(0); }
    close(fd[1]); char buf[64]={0}; int n=(int)read(fd[0],buf,63); close(fd[0]); waitpid(p,&st,0);
    if(n>0&&!strcmp(buf,"mensaje por pipe")){ SAY("pipe OK (%s)\n",buf); ok++; } else SAY("pipe FALLO n=%d\n",n);
    p=fork();
    if(p==0){ char *av[]={argv[0],"hijo",NULL}; execv(argv[0],av); SAY("execv FALLO\n"); _exit(99); }
    waitpid(p,&st,0);
    if(WIFEXITED(st)&&WEXITSTATUS(st)==7){ SAY("execve OK (hijo devolvio 7)\n"); ok++; } else SAY("execve FALLO st=%d\n",st);
    pthread_t th[4];
    for(long i=0;i<4;i++) pthread_create(&th[i],NULL,work,(void*)(i+1));
    long sum=0; for(int i=0;i<4;i++){ void *r; pthread_join(th[i],&r); sum+=(long)r; }
    if(counter==4000&&sum==20){ SAY("hilos OK (contador=%d, suma=%ld)\n",counter,sum); ok++; } else SAY("hilos FALLO contador=%d suma=%ld\n",counter,sum);
    usleep(50000); SAY("sleep OK\n"); ok++;
    p=fork();
    if(p==0){ for(;;) usleep(10000); }
    usleep(30000); kill(p,SIGKILL); waitpid(p,&st,0);
    if(WIFSIGNALED(st)&&WTERMSIG(st)==SIGKILL){ SAY("kill OK\n"); ok++; } else SAY("kill FALLO st=%d\n",st);
    SAY(ok==6?"TODO OK (6/6)\n":"HAY FALLOS (%d/6)\n",ok);
    return ok==6?0:1;
}
