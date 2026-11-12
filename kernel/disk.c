/* disk.c — controlador ATA por E/S de puertos (PIO, LBA28) y almacenamiento KNGFS.
 * Seguridad: nunca escribe en un disco sin firma "KNGFS001" salvo orden explicita del usuario (disco formatear si). */
#include "disk.h"
#include "fs.h"

#define KNGFS_MAGIC "KNGFS001"
#define KNGFS_MAXB (1536u*1024*1024)      /* tope de lo guardado (1,5 GB); el limite real es la RAM y el tamano del disco */

#ifdef HOSTDISK                            /* simulador del host: un "disco" de 4 MB en memoria */
static uint8_t fake[4*1024*1024]; static int fake_ok=1;
int disk_init(void){ return fake_ok?0:-1; }
uint32_t disk_sectors(void){ return sizeof fake/512; }
const char *disk_model(void){ return "disco simulado"; }
const char *disk_kind(void){ return "simulado"; }
int disk_read(uint32_t lba,void *b,int n){ if((lba+(uint32_t)n)*512>sizeof fake) return -1; memcpy(b,fake+lba*512,(size_t)n*512); return 0; }
int disk_write(uint32_t lba,const void *b,int n){ if((lba+(uint32_t)n)*512>sizeof fake) return -1; memcpy(fake+lba*512,b,(size_t)n*512); return 0; }
#else
static inline void o8(uint16_t p,uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t i8(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void insw_(uint16_t p,void *b,int n){ __asm__ volatile("rep insw":"+D"(b),"+c"(n):"d"(p):"memory"); }
static inline void outsw_(uint16_t p,const void *b,int n){ __asm__ volatile("rep outsw":"+S"(b),"+c"(n):"d"(p):"memory"); }
static uint16_t base; static int slot=-1; static uint32_t nsect; static char model[41];
static int wait_bsy(void){ for(int t=0;t<2000000;t++){ uint8_t s=i8((uint16_t)(base+7)); if(!(s&0x80)) return 0; } return -1; }
static int wait_drq(void){ for(int t=0;t<2000000;t++){ uint8_t s=i8((uint16_t)(base+7)); if(s&0x01||s&0x20) return -1; if(!(s&0x80)&&(s&0x08)) return 0; } return -1; }
static void select_(int sl,uint32_t lba){ o8((uint16_t)(base+6),(uint8_t)(0xE0|(sl<<4)|((lba>>24)&0xF))); for(int i=0;i<4;i++) (void)i8((uint16_t)(base+7)); }
static int identify(uint16_t b,int sl){
    base=b; o8((uint16_t)(b+6),(uint8_t)(0xA0|(sl<<4))); for(int i=0;i<4;i++) (void)i8((uint16_t)(b+7));
    o8((uint16_t)(b+2),0); o8((uint16_t)(b+3),0); o8((uint16_t)(b+4),0); o8((uint16_t)(b+5),0); o8((uint16_t)(b+7),0xEC);
    uint8_t s=i8((uint16_t)(b+7)); if(s==0||s==0xFF) return -1;
    if(wait_bsy()) return -1;
    if(i8((uint16_t)(b+4))||i8((uint16_t)(b+5))) return -1;                 /* ATAPI (CD) u otro: no es disco duro */
    if(wait_drq()) return -1;
    uint16_t id[256]; insw_(b,id,256);
    nsect=(uint32_t)id[60]|((uint32_t)id[61]<<16); if(!nsect) return -1;
    for(int i=0;i<20;i++){ model[i*2]=(char)(id[27+i]>>8); model[i*2+1]=(char)id[27+i]; } model[40]=0;
    for(int i=39;i>=0&&(model[i]==' '||!model[i]);i--) model[i]=0;
    slot=sl; return 0;
}
static int ata_init(void){
    static const uint16_t ports[2]={0x1F0,0x170};
    for(int c=0;c<2;c++) for(int sl=0;sl<2;sl++) if(identify(ports[c],sl)==0) return 0;
    slot=-1; return -1;
}
static int ata_read(uint32_t lba,void *buf,int n){
    if(slot<0||lba+(uint32_t)n>nsect) return -1;
    for(int i=0;i<n;i++){
        uint32_t l=lba+(uint32_t)i; if(wait_bsy()) return -1; select_(slot,l);
        o8((uint16_t)(base+2),1); o8((uint16_t)(base+3),(uint8_t)l); o8((uint16_t)(base+4),(uint8_t)(l>>8)); o8((uint16_t)(base+5),(uint8_t)(l>>16)); o8((uint16_t)(base+7),0x20);
        if(wait_drq()) return -1;
        insw_(base,(uint8_t*)buf+i*512,256);
    }
    return 0;
}
static int ata_write(uint32_t lba,const void *buf,int n){
    if(slot<0||lba+(uint32_t)n>nsect) return -1;
    for(int i=0;i<n;i++){
        uint32_t l=lba+(uint32_t)i; if(wait_bsy()) return -1; select_(slot,l);
        o8((uint16_t)(base+2),1); o8((uint16_t)(base+3),(uint8_t)l); o8((uint16_t)(base+4),(uint8_t)(l>>8)); o8((uint16_t)(base+5),(uint8_t)(l>>16)); o8((uint16_t)(base+7),0x30);
        if(wait_drq()) return -1;
        outsw_(base,(const uint8_t*)buf+i*512,256);
        if(wait_bsy()) return -1;
    }
    o8((uint16_t)(base+7),0xE7); return wait_bsy();                        /* vaciar la cache del disco */
}

/* ===== AHCI (SATA): el controlador de casi todas las PC y de VirtualBox. Sin interrupciones: se espera a que termine cada orden. ===== */
static inline void o32(uint16_t p,uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t i32(uint16_t p){ uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static uint32_t pcir(int b,int d,int f,int o){ o32(0xCF8,0x80000000u|((uint32_t)b<<16)|((uint32_t)d<<11)|((uint32_t)f<<8)|((uint32_t)o&0xFC)); return i32(0xCFC); }
static void pciw(int b,int d,int f,int o,uint32_t v){ o32(0xCF8,0x80000000u|((uint32_t)b<<16)|((uint32_t)d<<11)|((uint32_t)f<<8)|((uint32_t)o&0xFC)); o32(0xCFC,v); }
static volatile uint32_t *abar; static int aport=-1;
static uint8_t a_clb[1024] __attribute__((aligned(1024)));
static uint8_t a_fb[256] __attribute__((aligned(256)));
static uint8_t a_ct[256] __attribute__((aligned(128)));
static uint8_t a_buf[65536] __attribute__((aligned(4096)));
#define AR(r) abar[(r)/4]
#define AP(r) abar[(0x100+aport*0x80+(r))/4]
static int a_wait(int reg,uint32_t mask,uint32_t want){ for(int t=0;t<20000000;t++){ if((AP(reg)&mask)==want) return 0; } return -1; }
static int a_cmd(uint8_t cmd,uint64_t lba,int count,int write,int bytes){
    if(a_wait(0x20,0x88,0)) return -1;                                  /* TFD: ni ocupado ni pidiendo datos */
    uint32_t *h=(uint32_t*)a_clb; memset(a_clb,0,64);
    h[0]=5u|(write?1u<<6:0)|(1u<<16); h[1]=0; h[2]=(uint32_t)(uintptr_t)a_ct; h[3]=0;
    memset(a_ct,0,sizeof a_ct);
    uint8_t *f=a_ct; f[0]=0x27; f[1]=0x80; f[2]=cmd; f[7]=0x40;
    f[4]=(uint8_t)lba; f[5]=(uint8_t)(lba>>8); f[6]=(uint8_t)(lba>>16); f[8]=(uint8_t)(lba>>24); f[9]=(uint8_t)(lba>>32); f[10]=(uint8_t)(lba>>40);
    f[12]=(uint8_t)count; f[13]=(uint8_t)(count>>8);
    uint32_t *prd=(uint32_t*)(a_ct+0x80); prd[0]=(uint32_t)(uintptr_t)a_buf; prd[1]=0; prd[2]=0; prd[3]=(uint32_t)(bytes-1);
    AP(0x10)=0xFFFFFFFFu; AP(0x30)=0xFFFFFFFFu;
    __asm__ volatile("mfence":::"memory");
    AP(0x38)=1;
    for(int t=0;t<50000000;t++){
        if(AP(0x10)&(1u<<30)) return -1;                                /* error de archivo de tareas */
        if(!(AP(0x38)&1)) return (AP(0x20)&1)?-1:0;
    }
    return -1;
}
static uint64_t a_nsect; static char a_model[41];
static int a_port_up(int p){
    aport=p;
    AP(0x18)&=~((1u<<0)|(1u<<4));                                       /* parar el puerto antes de darle nuestras tablas */
    if(a_wait(0x18,(1u<<15)|(1u<<14),0)) return -1;
    memset(a_clb,0,sizeof a_clb); memset(a_fb,0,sizeof a_fb);
    AP(0x00)=(uint32_t)(uintptr_t)a_clb; AP(0x04)=0; AP(0x08)=(uint32_t)(uintptr_t)a_fb; AP(0x0C)=0;
    AP(0x30)=0xFFFFFFFFu; AP(0x10)=0xFFFFFFFFu;
    AP(0x18)|=1u<<4; AP(0x18)|=1u<<0;
    if(a_cmd(0xEC,0,0,0,512)) return -1;                               /* IDENTIFY */
    const uint16_t *id=(const uint16_t*)a_buf;
    a_nsect=(uint64_t)id[100]|((uint64_t)id[101]<<16)|((uint64_t)id[102]<<32)|((uint64_t)id[103]<<48);
    if(!a_nsect) a_nsect=(uint32_t)id[60]|((uint32_t)id[61]<<16);
    if(!a_nsect) return -1;
    for(int i=0;i<20;i++){ a_model[i*2]=(char)(id[27+i]>>8); a_model[i*2+1]=(char)id[27+i]; } a_model[40]=0;
    for(int i=39;i>=0&&(a_model[i]==' '||!a_model[i]);i--) a_model[i]=0;
    return 0;
}
static int ahci_init(void){
    for(int b=0;b<256;b++) for(int d=0;d<32;d++){
        if((pcir(b,d,0,0)&0xFFFF)==0xFFFF) continue;
        int nf=(pcir(b,d,0,0x0C)&0x00800000u)?8:1;
        for(int fn=0;fn<nf;fn++){
            uint32_t id=pcir(b,d,fn,0); if((id&0xFFFF)==0xFFFF) continue;
            if((pcir(b,d,fn,0x08)>>8)!=0x010601) continue;               /* clase 01, subclase 06 (SATA), interfaz 01 (AHCI) */
            uint32_t bar=pcir(b,d,fn,0x24); if(bar&1) continue;
            pciw(b,d,fn,0x04,pcir(b,d,fn,0x04)|0x06);                     /* memoria + bus master */
            abar=(volatile uint32_t*)(uintptr_t)(bar&~0xFu);
            AR(0x04)|=1u<<31;                                             /* modo AHCI */
            uint32_t pi=AR(0x0C);
            for(int p=0;p<32;p++){
                if(!(pi&(1u<<p))) continue;
                aport=p; uint32_t st=AP(0x28);
                if((st&0xF)!=3||AP(0x24)!=0x00000101u) continue;          /* hay un disco SATA (no un lector de CD) */
                if(a_port_up(p)==0) return 0;
            }
            aport=-1;
        }
    }
    return -1;
}
static int ahci_rw(uint64_t lba,void *buf,int n,int write){
    uint8_t *p=buf;
    while(n>0){
        int c=n>128?128:n;
        if(write) memcpy(a_buf,p,(size_t)c*512);
        if(a_cmd(write?0x35:0x25,lba,c,write,c*512)) return -1;          /* WRITE/READ DMA EXT */
        if(!write) memcpy(p,a_buf,(size_t)c*512);
        p+=c*512; lba+=(uint64_t)c; n-=c;
    }
    if(write&&a_cmd(0xEA,0,0,0,512)) return -1;                          /* vaciar la cache del disco */
    return 0;
}
/* ===== disco elegido ===== */
static int drv;                                                           /* 0 ninguno, 1 ATA (IDE), 2 AHCI (SATA) */
int disk_init(void){
    if(drv) return 0;
    if(ata_init()==0){ drv=1; return 0; }
    if(ahci_init()==0){ drv=2; return 0; }
    return -1;
}
uint32_t disk_sectors(void){ if(drv==1) return nsect; if(drv==2) return a_nsect>0xFFFFFFFFull?0xFFFFFFFFu:(uint32_t)a_nsect; return 0; }
const char *disk_model(void){ if(drv==1) return model; if(drv==2) return a_model[0]?a_model:"disco SATA"; return "(sin disco)"; }
const char *disk_kind(void){ return drv==1?"IDE":drv==2?"SATA (AHCI)":"ninguno"; }
int disk_read(uint32_t lba,void *buf,int n){ if(lba+(uint32_t)n>disk_sectors()) return -1; return drv==1?ata_read(lba,buf,n):drv==2?ahci_rw(lba,buf,n,0):-1; }
int disk_write(uint32_t lba,const void *buf,int n){ if(lba+(uint32_t)n>disk_sectors()) return -1; return drv==1?ata_write(lba,buf,n):drv==2?ahci_rw(lba,(void*)buf,n,1):-1; }
#endif

/* ---- KNGFS: sector 0 = firma + tamano + suma; sectores 1.. = arbol serializado ---- */
static int mounted; static uint32_t saved_gen, next_try;
static int bcmp_(const uint8_t *a,const char *b,int n){ for(int i=0;i<n;i++) if(a[i]!=(uint8_t)b[i]) return 1; return 0; }
static uint32_t sum(const uint8_t *p,uint32_t n){                         /* Adler-32 (modulo cada 5552 bytes: rapido) */
    uint32_t a=1,b=0;
    while(n){ uint32_t m=n<5552?n:5552; n-=m; while(m--){ a+=*p++; b+=a; } a%=65521u; b%=65521u; }
    return (b<<16)|a;
}
static uint32_t last_size;
int kngfs_active(void){ return mounted; }
uint32_t kngfs_last_size(void){ return last_size; }
int kngfs_save(void){
    if(!mounted) return -1;
    int need=fs_serialize(0,0); if(need<0||(uint32_t)need>KNGFS_MAXB) return -2;
    uint32_t secs=((uint32_t)need+511)/512; if(1+secs>disk_sectors()) return -2;
    uint8_t *buf=kmalloc((size_t)secs*512+512); if(!buf) return -3;           /* sin memoria para armar la copia */
    int n=fs_serialize(buf+512,(int)(secs*512));
    if(n<0){ kfree(buf); return -2; }
    memset(buf,0,512); memcpy(buf,KNGFS_MAGIC,8);
    uint32_t v[2]={(uint32_t)n,sum(buf+512,(uint32_t)n)}; memcpy(buf+8,v,8);
    int r=0; for(uint32_t i=0;i<secs&&!r;i+=2048){ uint32_t c=secs-i>2048?2048:secs-i; r=disk_write(1+i,buf+512+(size_t)i*512,(int)c); }
    if(!r) r=disk_write(0,buf,1);                                            /* la cabecera va al final: si se corta, queda la copia anterior */
    kfree(buf); if(!r){ saved_gen=fs_gen(); last_size=(uint32_t)n; } return r;
}
int kngfs_mount(void){
    if(mounted) return 0;
    if(disk_init()) return -1;
    uint8_t hd[512]; if(disk_read(0,hd,1)||bcmp_(hd,KNGFS_MAGIC,8)) return -2;
    uint32_t v[2]; memcpy(v,hd+8,8); if(v[0]==0||v[0]>KNGFS_MAXB) return -3;
    uint32_t secs=(v[0]+511)/512; if(1+secs>disk_sectors()) return -3;
    uint8_t *buf=kmalloc((size_t)secs*512); if(!buf) return -4;
    int r=0; for(uint32_t i=0;i<secs&&!r;i+=2048){ uint32_t c=secs-i>2048?2048:secs-i; r=disk_read(1+i,buf+(size_t)i*512,(int)c); }
    if(r||sum(buf,v[0])!=v[1]||fs_overlay(buf,(int)v[0])){ kfree(buf); return -5; }
    kfree(buf); mounted=1; saved_gen=fs_gen(); last_size=v[0]; return 0;
}
int kngfs_format(void){ if(disk_init()) return -1; mounted=1; int r=kngfs_save(); if(r) mounted=0; return r; }
void kngfs_tick(void){                                                       /* guarda solo si hubo cambios: cada 5 s si es poco, cada 3 min si es mucho */
    if(!mounted||fs_gen()==saved_gen) return;
    uint32_t t=kticks(); if((int)(t-next_try)<0) return;
    next_try=t+(last_size<(32u<<20)?500:18000);
    (void)kngfs_save();
}
