/* sys_extra.inc — llamadas del sistema menos frecuentes que piden los programas grandes (navegadores, apt, LibreOffice, servidor X) */

/* ---------- reloj de pared: se lee del reloj de la placa (RTC) una vez y despues avanza con el temporizador ---------- */
extern u64 krtc_epoch(void);
static u64 wall_base; static int wall_ok;
static uint64_t wall_sec(void){ u64 up=kticks()/100; if(!wall_ok){ wall_ok=1; wall_base=krtc_epoch(); if(wall_base>up) wall_base-=up; } return wall_base+up; }
static void wall_set(u64 sec){ wall_ok=1; wall_base=sec-kticks()/100; }
static u64 sys_clock_gettime(u64 clk,u64 va){
    u32 k=kticks(); u64 t[2]; t[1]=(u64)(k%100)*10000000ull;
    t[0]=(clk==0||clk==5)?wall_sec():k/100;                        /* 0 y 5 = hora real; el resto = tiempo desde el arranque */
    return ucopy_to(CP,va,t,16)?RET_ERR(EFAULT):0;
}
static u64 sys_settime(u64 clk,u64 va){ u64 t[2]; if(clk!=0) return RET_ERR(EINVAL); if(ucopy_from(CP,t,va,16)) return RET_ERR(EFAULT); wall_set(t[0]); return 0; }

/* ---------- limites ---------- */
static u64 sys_getrlimit_k(u64 res,u64 va){
    u64 rl[2]={~0ull,~0ull};
    if(res==3) rl[0]=8ull<<20;                      /* pila */
    else if(res==7){ rl[0]=MAXFD; rl[1]=MAXFD; }    /* archivos abiertos */
    else if(res==6){ rl[0]=MAXT; rl[1]=MAXT; }      /* procesos/hilos */
    { extern int krastro; if(krastro){ int vi=vma_find(CP,va); kprintf("[rlimit %d res=%lu va=%lx vma=%d prot=%d]\n",cur->pid,res,va,vi,vi>=0?(int)CP->vma[vi].prot:-1); } }
    return va&&ucopy_to(CP,va,rl,16)?RET_ERR(EFAULT):0;
}

/* ---------- waitid ---------- */
static u64 sys_waitid(u64 idtype,u64 id,u64 infop,u64 options){
    long pid=idtype==1?(long)id:-1; u32 st=0; long r=wait_core(pid,options&1,&st,(options&0x1000000)!=0);
    if(r<0) return RET_ERR(-r);
    if(infop){ u8 si[128]; memset(si,0,sizeof si);
        if(r>0){ *(int*)(si+0)=17; int sig=(int)(st&0x7f); *(int*)(si+8)=sig?2:1; *(int*)(si+16)=(int)r; *(int*)(si+24)=sig?sig:(int)((st>>8)&0xff); }
        if(ucopy_to(CP,infop,si,sizeof si)) return RET_ERR(EFAULT); }
    return 0;
}

/* ---------- varios mensajes de una vez (el resolvedor de nombres de glibc usa sendmmsg) ---------- */
static u64 sys_sendmmsg(u64 fdn,u64 vec,u64 vlen,u64 flags){
    if(vlen>64) vlen=64;
    u64 n=0;
    for(;n<vlen;n++){ u64 r=sys_sendmsg(fdn,vec+n*64,flags); if((long)r<0) return n?n:r; u32 len=(u32)r; if(ucopy_to(CP,vec+n*64+56,&len,4)) return RET_ERR(EFAULT); }
    return n;
}
static u64 sys_recvmmsg(u64 fdn,u64 vec,u64 vlen,u64 flags){
    if(vlen>64) vlen=64;
    u64 n=0;
    for(;n<vlen;n++){ u64 r=sys_recvmsg(fdn,vec+n*64,n?(flags|0x40):flags); if((long)r<0){ if(n&&(long)r==-(long)EAGAIN) return n; return n?n:r; } u32 len=(u32)r; if(ucopy_to(CP,vec+n*64+56,&len,4)) return RET_ERR(EFAULT); }
    return n;
}
static u64 sys_preadv(u64 fdn,u64 iov,u64 cnt,u64 off,int write){
    u64 tot=0; for(u64 i=0;i<cnt&&i<64;i++){ u64 v[2]; if(ucopy_from(CP,v,iov+i*16,16)) return RET_ERR(EFAULT);
        u64 rv=sys_pio(fdn,v[0],v[1],off+tot,write); if((long)rv<0) return tot?tot:rv; tot+=rv; if(rv<v[1]) break; }
    return tot;
}
static u64 sys_close_range(u64 first,u64 last,u64 flags){
    if(first>last) return RET_ERR(EINVAL);
    if(last>=MAXFD) last=MAXFD-1;
    for(u64 f=first;f<=last;f++) if(CP->fd[f].type){ if(flags&4) CP->fd[f].cloexec=1; else fd_close(&CP->fd[f]); }
    return 0;
}
static u64 sys_capget(u64 hdr,u64 data){
    u32 h[2]; if(ucopy_from(CP,h,hdr,8)) return RET_ERR(EFAULT);
    if(h[0]!=0x20080522&&h[0]!=0x20071026&&h[0]!=0x19980330){ h[0]=0x20080522; ucopy_to(CP,hdr,h,4); return RET_ERR(EINVAL); }
    if(!data) return 0;
    u32 d[6]={~0u,~0u,0,~0u,~0u,0}; return ucopy_to(CP,data,d,h[0]==0x19980330?12:24)?RET_ERR(EFAULT):0;
}

static uint32_t fs_now_fn(void){ return (uint32_t)wall_sec(); }
