/* ulib.h — mini libc de modo usuario para kng-kernel (syscalls con numeros de Linux i386). */
#ifndef ULIB_H
#define ULIB_H
typedef unsigned int u32;
static inline int sys(int n,int a,int b,int c){
    int r; __asm__ volatile("int $0x80":"=a"(r):"0"(n),"b"(a),"c"(b),"d"(c):"memory"); return r;
}
static inline void exit(int c){ sys(1,c,0,0); for(;;); }
static inline int  read(int fd,void*b,u32 n){ return sys(3,fd,(int)b,(int)n); }
static inline int  write(int fd,const void*b,u32 n){ return sys(4,fd,(int)b,(int)n); }
static inline int  getpid(void){ return sys(20,0,0,0); }
static inline int  getppid(void){ return sys(64,0,0,0); }
static inline int  fork(void){ return sys(2,0,0,0); }
static inline int  waitpid(int pid,int*st,int opt){ return sys(7,pid,(int)st,opt); }
static inline int  exec(const char*name){ return sys(11,(int)name,0,0); }   /* programa incrustado por nombre */
static inline int  brk(u32 a){ return sys(45,(int)a,0,0); }                 /* devuelve el brk resultante */
static inline int  sched_yield(void){ return sys(158,0,0,0); }
static inline int  sleep_ms(u32 ms){ int ts[2]={(int)(ms/1000),(int)((ms%1000)*1000000)}; return sys(162,(int)ts,0,0); }
static inline u32  uptime_ticks(void){ return (u32)sys(500,0,0,0); }
static inline void puts_(const char*s){ u32 n=0; while(s[n]) n++; write(1,s,n); }
static inline void putnum(int v){
    char b[12]; int i=0; unsigned u=v<0?-(unsigned)v:(unsigned)v;
    if(!u) b[i++]='0';
    while(u){ b[i++]=(char)('0'+u%10); u/=10; }
    if(v<0) write(1,"-",1);
    while(i) write(1,&b[--i],1);
}
#endif
