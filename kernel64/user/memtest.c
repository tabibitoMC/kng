/* memtest.c — prueba de memoria y archivos con glibc normal (se ejecuta dentro de kng).
 * mmap/munmap/mprotect/mremap/madvise, memoria compartida (memfd, shm_open, shmget), pila grande, SIGSEGV en paginas protegidas,
 * y archivos: crear, escribir 3 MB, pread/pwrite, truncar, enlaces simbolicos, renombrar, borrar con el archivo abierto, carpetas grandes, stat/statx. */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/sendfile.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/syscall.h>
static int bad, good;
#define SAY(...) do{ printf(__VA_ARGS__); fflush(stdout); }while(0)
#define CHECK(c,name) do{ if(c){ SAY("OK  %s\n",name); good++; } else { SAY("FALLO %s (errno=%d %s)\n",name,errno,strerror(errno)); bad++; } }while(0)

static sigjmp_buf jb; static volatile int got_segv;
static void h_segv(int s,siginfo_t *si,void *uc){ (void)uc; (void)si; got_segv=s; siglongjmp(jb,1); }

static int deep(int n){ volatile char buf[4096]; buf[0]=(char)n; buf[4095]=(char)n; if(n<=0) return buf[0]; return deep(n-1)+buf[4095]-buf[0]+1; }

static void test_mem(void){
    size_t L=1<<20;
    unsigned char *a=mmap(0,64*L,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(a!=MAP_FAILED,"mmap anonimo de 64 MB (sin usar todavia)");
    int ok=1; for(size_t i=0;i<64*L;i+=4096*37){ if(a[i]!=0) ok=0; a[i]=(unsigned char)(i>>12); }
    for(size_t i=0;i<64*L;i+=4096*37) if(a[i]!=(unsigned char)(i>>12)) ok=0;
    CHECK(ok,"paginas se crean en ceros al primer uso y guardan datos");
    CHECK(munmap(a+16*L,16*L)==0,"munmap de un pedazo en medio");
    got_segv=0; struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=h_segv; sa.sa_flags=SA_SIGINFO|SA_NODEFER; sigaction(SIGSEGV,&sa,0);
    if(!sigsetjmp(jb,1)){ volatile unsigned char x=a[16*L+100]; (void)x; }
    CHECK(got_segv==SIGSEGV,"tocar memoria liberada da SIGSEGV");
    CHECK(a[0]==0&&a[63*L]==0,"lo que queda sigue accesible");
    munmap(a,16*L); munmap(a+32*L,32*L);

    unsigned char *r=mmap(0,1ull<<36,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);   /* reserva de 64 GB como hacen los navegadores */
    CHECK(r!=MAP_FAILED,"reserva de 64 GB con PROT_NONE");
    CHECK(mprotect(r+(1<<20),8<<20,PROT_READ|PROT_WRITE)==0,"mprotect habilita solo 8 MB");
    r[(1<<20)+5]=7; r[(8<<20)+100]=9; CHECK(r[(1<<20)+5]==7&&r[(8<<20)+100]==9,"se puede escribir en la parte habilitada");
    got_segv=0; if(!sigsetjmp(jb,1)){ r[100]=1; }
    CHECK(got_segv==SIGSEGV,"la parte PROT_NONE da SIGSEGV");
    CHECK(munmap(r,1ull<<36)==0,"munmap de la reserva de 64 GB");

    unsigned char *m=mmap(0,4*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); memset(m,0x5a,4*4096);
    madvise(m,4096*2,MADV_DONTNEED); CHECK(m[10]==0&&m[2*4096+10]==0x5a,"madvise(DONTNEED) deja ceros solo donde se pidio");
    unsigned char *m2=mremap(m,4*4096,40*4096,MREMAP_MAYMOVE); CHECK(m2!=MAP_FAILED&&m2[2*4096+10]==0x5a&&m2[39*4096]==0,"mremap crece y conserva los datos");
    munmap(m2,40*4096);

    void *fx=mmap((void*)0x2000000000ull,3*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0); CHECK(fx==(void*)0x2000000000ull,"MAP_FIXED en direccion pedida");
    ((char*)fx)[5000]=3; munmap(fx,3*4096);

    CHECK(deep(1500)>0,"pila: recursion de 6 MB");

    int fd=memfd_create("kng",0); CHECK(fd>=0,"memfd_create");
    CHECK(ftruncate(fd,5*4096)==0,"ftruncate del memfd");
    unsigned char *s=mmap(0,5*4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); CHECK(s!=MAP_FAILED,"mmap compartido del memfd");
    s[0]=1; pid_t c=fork();
    if(c==0){ s[0]=42; s[4*4096]=43; _exit(0); }
    int st=0; waitpid(c,&st,0);
    CHECK(s[0]==42&&s[4*4096]==43,"memoria compartida: lo que escribe el hijo lo ve el padre");
    char tmp[4]; CHECK(pread(fd,tmp,1,0)==1&&tmp[0]==42,"read del memfd ve lo mismo que el mapeo");
    munmap(s,5*4096); close(fd);

    int sid=shmget(IPC_PRIVATE,3*4096,IPC_CREAT|0600); CHECK(sid>=0,"shmget");
    unsigned char *sh=shmat(sid,0,0); CHECK(sh!=(void*)-1,"shmat");
    sh[0]=9; c=fork();
    if(c==0){ sh[0]++; sh[3*4096-1]=77; shmdt(sh); _exit(0); }
    waitpid(c,&st,0);
    CHECK(sh[0]==10&&sh[3*4096-1]==77,"System V: memoria compartida entre procesos");
    CHECK(shmdt(sh)==0&&shmctl(sid,IPC_RMID,0)==0,"shmdt y shmctl(IPC_RMID)");

    unsigned char *pv=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); pv[0]=5; c=fork();
    if(c==0){ pv[0]=6; _exit(pv[0]==6?0:1); }
    waitpid(c,&st,0); CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==0&&pv[0]==5,"fork: la memoria privada no se comparte");
    munmap(pv,4096);

    /* memoria que se pide y se devuelve muchas veces no debe agotar nada */
    int okk=1; for(int i=0;i<300;i++){ unsigned char *t=mmap(0,2<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(t==MAP_FAILED){ okk=0; break; } for(int j=0;j<2<<20;j+=4096) t[j]=1; munmap(t,2<<20); }
    CHECK(okk,"300 veces pedir 2 MB, usarlos y devolverlos");
    void *big=malloc(48<<20); CHECK(big!=0,"malloc de 48 MB"); if(big){ memset(big,1,48<<20); free(big); }
}

static void test_files(void){
    mkdir("/tmp/mt",0755);
    int fd=open("/tmp/mt/grande.bin",O_RDWR|O_CREAT|O_TRUNC,0644); CHECK(fd>=0,"crear archivo");
    static char blk[4096]; for(int i=0;i<4096;i++) blk[i]=(char)(i*7);
    int ok=1; for(int i=0;i<768;i++){ blk[0]=(char)i; if(write(fd,blk,4096)!=4096){ ok=0; break; } }
    CHECK(ok,"escribir 3 MB en bloques de 4 KB");
    struct stat sb; fstat(fd,&sb); CHECK(sb.st_size==768*4096&&S_ISREG(sb.st_mode),"fstat da el tamano correcto");
    char rb[8]; CHECK(pread(fd,rb,4,100*4096)==4&&rb[0]==100,"pread en el medio");
    CHECK(pwrite(fd,"HOLA",4,5)==4,"pwrite");
    char rb2[5]={0}; lseek(fd,5,SEEK_SET); read(fd,rb2,4); CHECK(!strcmp(rb2,"HOLA"),"lseek y read despues de pwrite");
    CHECK(ftruncate(fd,10)==0&&lseek(fd,0,SEEK_END)==10,"ftruncate recorta");
    CHECK(ftruncate(fd,1<<20)==0,"ftruncate agranda"); char z=1; pread(fd,&z,1,5000); CHECK(z==0,"lo agrandado esta en ceros");
    close(fd);

    fd=open("/tmp/mt/ap.txt",O_WRONLY|O_CREAT|O_TRUNC|O_APPEND,0644); write(fd,"uno\n",4); close(fd);
    fd=open("/tmp/mt/ap.txt",O_WRONLY|O_APPEND); write(fd,"dos\n",4); close(fd);
    fd=open("/tmp/mt/ap.txt",O_RDONLY); char ap[16]={0}; read(fd,ap,15); close(fd); CHECK(!strcmp(ap,"uno\ndos\n"),"O_APPEND");
    CHECK(open("/tmp/mt/ap.txt",O_CREAT|O_EXCL|O_WRONLY,0644)<0&&errno==EEXIST,"O_EXCL falla si existe");
    CHECK(open("/tmp/mt/no-existe",O_RDONLY)<0&&errno==ENOENT,"abrir algo que no existe da ENOENT");

    CHECK(symlink("ap.txt","/tmp/mt/enlace")==0,"symlink");
    char lb[64]={0}; ssize_t n=readlink("/tmp/mt/enlace",lb,sizeof lb-1); CHECK(n==6&&!strcmp(lb,"ap.txt"),"readlink");
    fd=open("/tmp/mt/enlace",O_RDONLY); ap[0]=0; if(fd>=0){ read(fd,ap,15); close(fd); } CHECK(!strcmp(ap,"uno\ndos\n"),"abrir a traves de un enlace simbolico");
    struct stat ls; lstat("/tmp/mt/enlace",&ls); CHECK(S_ISLNK(ls.st_mode),"lstat ve el enlace"); stat("/tmp/mt/enlace",&ls); CHECK(S_ISREG(ls.st_mode)&&ls.st_size==8,"stat sigue el enlace");
    symlink("/tmp/mt","/tmp/mtdir"); fd=open("/tmp/mtdir/ap.txt",O_RDONLY); CHECK(fd>=0,"enlace a carpeta en medio de la ruta"); if(fd>=0) close(fd);
    symlink("bucle2","/tmp/mt/bucle1"); symlink("bucle1","/tmp/mt/bucle2"); CHECK(open("/tmp/mt/bucle1",O_RDONLY)<0,"bucle de enlaces no cuelga el sistema");

    CHECK(rename("/tmp/mt/ap.txt","/tmp/mt/ap2.txt")==0&&access("/tmp/mt/ap2.txt",F_OK)==0&&access("/tmp/mt/ap.txt",F_OK)<0,"rename");
    fd=open("/tmp/mt/temp",O_RDWR|O_CREAT,0600); write(fd,"abc",3); CHECK(unlink("/tmp/mt/temp")==0&&access("/tmp/mt/temp",F_OK)<0,"unlink con el archivo abierto");
    char tb[4]={0}; lseek(fd,0,SEEK_SET); CHECK(read(fd,tb,3)==3&&!strcmp(tb,"abc"),"el archivo borrado sigue legible mientras esta abierto"); close(fd);

    char longn[100]; memset(longn,'x',80); longn[80]=0; char lp[160]; snprintf(lp,sizeof lp,"/tmp/mt/%s",longn);
    fd=open(lp,O_CREAT|O_WRONLY,0644); CHECK(fd>=0,"nombre de 80 letras"); if(fd>=0) close(fd);

    mkdir("/tmp/mt/dir",0755); for(int i=0;i<300;i++){ char p[64]; snprintf(p,sizeof p,"/tmp/mt/dir/f%03d",i); int f=open(p,O_CREAT|O_WRONLY,0644); if(f>=0) close(f); }
    DIR *d=opendir("/tmp/mt/dir"); int cnt=0; struct dirent *de; while(d&&(de=readdir(d))) if(de->d_name[0]=='f') cnt++; if(d) closedir(d);
    CHECK(cnt==300,"readdir lista 300 archivos");
    CHECK(rmdir("/tmp/mt/dir")<0&&errno==ENOTEMPTY,"rmdir de carpeta llena da ENOTEMPTY");
    int dfd=open("/tmp/mt/dir",O_RDONLY|O_DIRECTORY); int f2=openat(dfd,"f007",O_RDONLY); CHECK(dfd>=0&&f2>=0,"openat con carpeta de referencia"); if(f2>=0) close(f2);
    struct stat s2; CHECK(fstatat(dfd,"f008",&s2,0)==0,"fstatat"); CHECK(unlinkat(dfd,"f009",0)==0,"unlinkat"); close(dfd);
    struct statx sx; CHECK(syscall(SYS_statx,AT_FDCWD,"/tmp/mt/ap2.txt",0,0x7ff,&sx)==0&&sx.stx_size==8,"statx");
    char cw[128]; chdir("/tmp/mt"); getcwd(cw,sizeof cw); CHECK(!strcmp(cw,"/tmp/mt"),"chdir y getcwd"); CHECK(access("ap2.txt",R_OK)==0,"ruta relativa tras chdir"); chdir("/");

    /* mmap de archivo, privado y compartido */
    fd=open("/tmp/mt/mapa.bin",O_RDWR|O_CREAT|O_TRUNC,0644); for(int i=0;i<3;i++){ memset(blk,'A'+i,4096); write(fd,blk,4096); }
    char *fm=mmap(0,3*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0); CHECK(fm!=MAP_FAILED&&fm[0]=='A'&&fm[4096]=='B'&&fm[8191]=='B'&&fm[8192]=='C',"mmap privado de archivo lee su contenido");
    fm[0]='Z'; char c0; pread(fd,&c0,1,0); CHECK(c0=='A',"escribir en mapa privado no cambia el archivo"); munmap(fm,3*4096);
    int sfd=shm_open("/kngtest",O_RDWR|O_CREAT,0600); CHECK(sfd>=0,"shm_open"); ftruncate(sfd,4096);
    char *sm=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,sfd,0); CHECK(sm!=MAP_FAILED,"mmap compartido de /dev/shm"); sm[0]=11; pid_t c=fork();
    if(c==0){ sm[0]=22; _exit(0); } int st=0; waitpid(c,&st,0); CHECK(sm[0]==22,"shm_open: lo ve el padre"); shm_unlink("/kngtest"); munmap(sm,4096); close(sfd); close(fd);

    /* enlaces duros reales (shadow/groupadd toma su candado con link() y exige st_nlink==2) */
    { int a=open("/tmp/mt/hl-a",O_RDWR|O_CREAT|O_TRUNC,0644); write(a,"uno",3);
      CHECK(link("/tmp/mt/hl-a","/tmp/mt/hl-b")==0,"link crea un enlace duro");
      struct stat sa,sb; fstat(a,&sa); stat("/tmp/mt/hl-b",&sb); CHECK(sa.st_nlink==2&&sb.st_nlink==2&&sa.st_ino==sb.st_ino,"los dos nombres tienen st_nlink=2 y el mismo inodo");
      write(a,"dos",3); char hb[16]={0}; int b=open("/tmp/mt/hl-b",O_RDONLY); read(b,hb,15); close(b); CHECK(!strcmp(hb,"unodos"),"escribir por un nombre se ve por el otro");
      unlink("/tmp/mt/hl-a"); stat("/tmp/mt/hl-b",&sb); memset(hb,0,sizeof hb); b=open("/tmp/mt/hl-b",O_RDONLY); read(b,hb,15); close(b);
      CHECK(sb.st_nlink==1&&!strcmp(hb,"unodos"),"al borrar un nombre el otro conserva el contenido y st_nlink=1"); close(a); }

    /* /proc/cpuinfo con las banderas reales (el script de Chromium exige sse3 = "pni") */
    { char ci[8192]={0}; int f=open("/proc/cpuinfo",O_RDONLY); ssize_t r=f>=0?read(f,ci,sizeof ci-1):-1; if(f>=0) close(f);
      CHECK(r>0&&strstr(ci,"flags")&&strstr(ci," sse2")&&strstr(ci," lm"),"/proc/cpuinfo lista las banderas del procesador"); }

    /* /proc/self/fd/N (Chromium reabre asi su memoria compartida en solo lectura) */
    { int a=open("/tmp/mt/ap2.txt",O_RDONLY); char pth[64]; snprintf(pth,sizeof pth,"/proc/self/fd/%d",a);
      int b=open(pth,O_RDONLY); char pb[16]={0}; if(b>=0){ if(read(b,pb,4)<0) pb[0]=0; close(b); } CHECK(a>=0&&b>=0&&!strncmp(pb,"uno\n",4),"open(/proc/self/fd/N) reabre el mismo archivo");
      int found=0; DIR *dd=opendir("/proc/self/fd"); struct dirent *de2; char want[16]; snprintf(want,sizeof want,"%d",a);
      while(dd&&(de2=readdir(dd))) if(!strcmp(de2->d_name,want)) found=1; if(dd) closedir(dd); CHECK(found,"/proc/self/fd lista los descriptores abiertos");
      int m=(int)syscall(319,"kng-test",0); if(m>=0){ if(ftruncate(m,4096)<0) m=-1; }
      char *w=m>=0?mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,m,0):MAP_FAILED; if(w!=MAP_FAILED) strcpy(w,"memoria compartida");
      snprintf(pth,sizeof pth,"/proc/self/fd/%d",m); int ro=m>=0?open(pth,O_RDONLY):-1;
      char *r=ro>=0?mmap(0,4096,PROT_READ,MAP_SHARED,ro,0):MAP_FAILED;
      CHECK(r!=MAP_FAILED&&!strcmp(r,"memoria compartida"),"memfd reabierto en solo lectura y mapeado ve los mismos datos");
      if(a>=0) close(a); if(m>=0) close(m); if(ro>=0) close(ro); }

    /* sendfile y copy_file_range (las usan apt, curl y los navegadores para copiar archivos) */
    { static char big[10000], chk[10000]; for(int i=0;i<10000;i++) big[i]=(char)('a'+i%26);
      int a=open("/tmp/mt/sf-a",O_RDWR|O_CREAT|O_TRUNC,0644); write(a,big,10000);
      int b=open("/tmp/mt/sf-b",O_RDWR|O_CREAT|O_TRUNC,0644); lseek(a,0,SEEK_SET);
      ssize_t r=sendfile(b,a,NULL,10000); CHECK(r==10000,"sendfile copia 10000 bytes");
      memset(chk,0,sizeof chk); pread(b,chk,10000,0); CHECK(!memcmp(chk,big,10000),"sendfile: el contenido copiado es igual");
      int c2=open("/tmp/mt/sf-c",O_RDWR|O_CREAT|O_TRUNC,0644); off_t off=100; r=sendfile(c2,a,&off,500);
      memset(chk,0,sizeof chk); pread(c2,chk,500,0); CHECK(r==500&&off==600&&!memcmp(chk,big+100,500),"sendfile con desplazamiento actualiza el offset");
      int c3=open("/tmp/mt/sf-d",O_RDWR|O_CREAT|O_TRUNC,0644); long long io=0,oo=0; r=syscall(326,a,&io,c3,&oo,4096,0);
      memset(chk,0,sizeof chk); pread(c3,chk,4096,0); CHECK(r==4096&&io==4096&&oo==4096&&!memcmp(chk,big,4096),"copy_file_range copia y avanza los desplazamientos");
      close(a); close(b); close(c2); close(c3); }
}
int main(void){
    SAY("== memtest: memoria ==\n"); test_mem();
    SAY("== memtest: archivos ==\n"); test_files();
    SAY("memtest resultado: %d bien, %d mal -> %s\n",good,bad,bad?"HAY FALLOS":"TODO OK");
    return bad?1:0;
}
