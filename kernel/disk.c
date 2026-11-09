/* disk.c — controlador ATA por E/S de puertos (PIO, LBA28) y almacenamiento KNGFS.
 * Seguridad: nunca escribe en un disco sin firma "KNGFS001" salvo orden explicita del usuario (disco formatear si). */
#include "disk.h"
#include "fs.h"

#define KNGFS_MAGIC "KNGFS001"
#define KNGFS_MAXB (2*1024*1024)          /* tope del arbol serializado */

#ifdef HOSTDISK                            /* simulador del host: un "disco" de 4 MB en memoria */
static uint8_t fake[4*1024*1024]; static int fake_ok=1;
int disk_init(void){ return fake_ok?0:-1; }
uint32_t disk_sectors(void){ return sizeof fake/512; }
const char *disk_model(void){ return "disco simulado"; }
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
int disk_init(void){
    static const uint16_t ports[2]={0x1F0,0x170};
    for(int c=0;c<2;c++) for(int sl=0;sl<2;sl++) if(identify(ports[c],sl)==0) return 0;
    slot=-1; return -1;
}
uint32_t disk_sectors(void){ return slot>=0?nsect:0; }
const char *disk_model(void){ return slot>=0?model:"(sin disco)"; }
int disk_read(uint32_t lba,void *buf,int n){
    if(slot<0||lba+(uint32_t)n>nsect) return -1;
    for(int i=0;i<n;i++){
        uint32_t l=lba+(uint32_t)i; if(wait_bsy()) return -1; select_(slot,l);
        o8((uint16_t)(base+2),1); o8((uint16_t)(base+3),(uint8_t)l); o8((uint16_t)(base+4),(uint8_t)(l>>8)); o8((uint16_t)(base+5),(uint8_t)(l>>16)); o8((uint16_t)(base+7),0x20);
        if(wait_drq()) return -1;
        insw_(base,(uint8_t*)buf+i*512,256);
    }
    return 0;
}
int disk_write(uint32_t lba,const void *buf,int n){
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
#endif

/* ---- KNGFS: sector 0 = firma + tamano + suma; sectores 1.. = arbol serializado ---- */
static int mounted; static uint32_t saved_gen, next_try;
static int bcmp_(const uint8_t *a,const char *b,int n){ for(int i=0;i<n;i++) if(a[i]!=(uint8_t)b[i]) return 1; return 0; }
static uint32_t sum(const uint8_t *p,int n){ uint32_t a=1,b=0; for(int i=0;i<n;i++){ a=(a+p[i])%65521u; b=(b+a)%65521u; } return (b<<16)|a; }
int kngfs_active(void){ return mounted; }
int kngfs_save(void){
    if(!mounted) return -1;
    uint8_t *buf=kmalloc(KNGFS_MAXB+512); if(!buf) return -1;
    int n=fs_serialize(buf+512,KNGFS_MAXB);
    if(n<0){ kfree(buf); return -2; }                                       /* el arbol no cabe */
    int secs=(n+511)/512; if(1+(uint32_t)secs>disk_sectors()){ kfree(buf); return -2; }
    memset(buf,0,512); memcpy(buf,KNGFS_MAGIC,8);
    uint32_t v[2]={(uint32_t)n,sum(buf+512,n)}; memcpy(buf+8,v,8);
    int r=disk_write(1,buf+512,secs); if(!r) r=disk_write(0,buf,1);         /* la cabecera va al final: si se corta, queda la copia anterior */
    kfree(buf); if(!r) saved_gen=fs_gen(); return r;
}
int kngfs_mount(void){
    if(disk_init()) return -1;
    uint8_t hd[512]; if(disk_read(0,hd,1)||bcmp_(hd,KNGFS_MAGIC,8)) return -2;
    uint32_t v[2]; memcpy(v,hd+8,8); if(v[0]==0||v[0]>KNGFS_MAXB) return -3;
    int secs=(int)((v[0]+511)/512); uint8_t *buf=kmalloc((size_t)secs*512); if(!buf) return -4;
    if(disk_read(1,buf,secs)||sum(buf,(int)v[0])!=v[1]||fs_deserialize(buf,(int)v[0])){ kfree(buf); return -5; }
    kfree(buf); mounted=1; saved_gen=fs_gen(); return 0;
}
int kngfs_format(void){ if(disk_init()) return -1; mounted=1; int r=kngfs_save(); if(r) mounted=0; return r; }
void kngfs_tick(void){                                                       /* cada ~3 s, si hubo cambios */
    if(!mounted||fs_gen()==saved_gen) return;
    uint32_t t=kticks(); if((int)(t-next_try)<0) return; next_try=t+300;
    (void)kngfs_save();
}
