/* kng-kernel64 0.1 — kernel propio de Königreich OS para x86_64 (modo largo).
 * Arranca con Multiboot 1 (boot64.S pasa a 64 bits), IDT de 64 bits, PIC, PIT 100 Hz, teclado y raton PS/2, RTC,
 * heap, interfaz grafica (la misma de 32 bits: gfx/gui/fs/html) y red (la misma pila: net.c).
 * Todavia SIN procesos/modo usuario/planificador: las aplicaciones corren dentro del kernel (como en 32 bits).
 * Escrito desde cero a partir de la documentacion publica (Intel SDM, OSDev). */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "kng.h"
#include "kbd.h"
#include "gui.h"
#include "net.h"
#include "fs.h"

typedef uint64_t u64;

static inline void outb(uint16_t p,uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outw(uint16_t p,uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline u64 irq_save(void){ u64 f; __asm__ volatile("pushfq; pop %0; cli":"=r"(f)::"memory"); return f; }
static inline void irq_restore(u64 f){ if(f&0x200) __asm__ volatile("sti":::"memory"); }

/* ---------- Consola: VGA texto + puerto serie COM1 ---------- */
static volatile uint16_t *const VGA=(uint16_t*)0xB8000;
static int cx, cy; static const uint8_t COLOR=0x0B;
static void serial_init(void){ outb(0x3F9,0); outb(0x3FB,0x80); outb(0x3F8,1); outb(0x3F9,0); outb(0x3FB,0x03); outb(0x3FA,0xC7); outb(0x3FC,0x0B); }
static void serial_put(char c){ int t=100000; while(!(inb(0x3FD)&0x20) && --t); outb(0x3F8,(uint8_t)c); }
static int serial_has(void){ return inb(0x3FD)&1; }
static void vga_scroll(void){ for(int i=0;i<80*24;i++) VGA[i]=VGA[i+80]; for(int i=80*24;i<80*25;i++) VGA[i]=(uint16_t)((COLOR<<8)|' '); cy=24; }
void (*kcon_hook)(char);
static int gui_mode;
static void putc(char c){
    if(gui_mode){ if(c=='\n') serial_put('\r'); serial_put(c); if(kcon_hook) kcon_hook(c); return; }
    if(c=='\n'){ serial_put('\r'); serial_put('\n'); cx=0; cy++; }
    else if(c=='\b'){ serial_put('\b'); serial_put(' '); serial_put('\b'); if(cx>0){ cx--; VGA[cy*80+cx]=(uint16_t)((COLOR<<8)|' '); } }
    else { serial_put(c); VGA[cy*80+cx]=(uint16_t)((COLOR<<8)|(uint8_t)c); cx++; }
    if(cx>=80){ cx=0; cy++; }
    if(cy>=25) vga_scroll();
}
static void clear(void){ for(int i=0;i<80*25;i++) VGA[i]=(uint16_t)((COLOR<<8)|' '); cx=cy=0; }
static void puts_(const char*s){ while(*s) putc(*s++); }
static void print_num(u64 v,unsigned base,int neg){
    char b[66]; int i=0;
    if(!v) b[i++]='0';
    while(v){ b[i++]="0123456789abcdef"[v%base]; v/=base; }
    if(neg) putc('-');
    while(i) putc(b[--i]);
}
void kprintf(const char*f,...){
    u64 fl=irq_save();
    va_list a; va_start(a,f);
    for(;*f;f++){
        if(*f!='%'){ putc(*f); continue; }
        int lng=0; f++; if(*f=='l'){ lng=1; f++; }
        switch(*f){
            case 's': { const char*s=va_arg(a,const char*); puts_(s?s:"(null)"); break; }
            case 'c': putc((char)va_arg(a,int)); break;
            case 'd': { long long v=lng?va_arg(a,long long):va_arg(a,int); print_num(v<0?(u64)-v:(u64)v,10,v<0); break; }
            case 'u': print_num(lng?va_arg(a,u64):va_arg(a,uint32_t),10,0); break;
            case 'x': print_num(lng?va_arg(a,u64):va_arg(a,uint32_t),16,0); break;
            case '%': putc('%'); break;
            default: putc('%'); putc(*f);
        }
    }
    va_end(a); irq_restore(fl);
}
static int streq(const char*a,const char*b){ while(*a&&*a==*b){a++;b++;} return *a==*b; }
static int startswith(const char*s,const char*p){ while(*p){ if(*s++!=*p++) return 0; } return 1; }

void *memset(void *d,int c,size_t n){ void *r=d; u64 v=(uint8_t)c; v*=0x0101010101010101ull; size_t q=n>>3;
    __asm__ volatile("rep stosq":"+D"(d),"+c"(q):"a"(v):"memory"); n&=7; __asm__ volatile("rep stosb":"+D"(d),"+c"(n):"a"(v):"memory"); return r; }
void *memcpy(void *d,const void*s,size_t n){ void *r=d; size_t q=n>>3;
    __asm__ volatile("rep movsq":"+D"(d),"+S"(s),"+c"(q)::"memory"); n&=7; __asm__ volatile("rep movsb":"+D"(d),"+S"(s),"+c"(n)::"memory"); return r; }

/* ---------- IDT + PIC ---------- */
struct idt_e { uint16_t lo, sel; uint8_t ist, flags; uint16_t mid; uint32_t hi, zero; } __attribute__((packed));
struct dtr   { uint16_t limit; u64 base; } __attribute__((packed));
static struct idt_e idt[256];
extern u64 isr_table64[];
struct regs { u64 r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rbx,rdx,rcx,rax,int_no,err,rip,cs,rflags,rsp,ss; };
typedef void (*irq_fn)(void);
static irq_fn irq_handlers[16];
static void idt_init(void){
    for(int i=0;i<48;i++){
        u64 a=isr_table64[i];
        idt[i].lo=(uint16_t)a; idt[i].mid=(uint16_t)(a>>16); idt[i].hi=(uint32_t)(a>>32);
        idt[i].sel=0x08; idt[i].ist=0; idt[i].flags=0x8E; idt[i].zero=0;
    }
    struct dtr d={sizeof(idt)-1,(u64)(uintptr_t)idt};
    __asm__ volatile("lidt %0"::"m"(d));
}
static void pic_init(void){
    outb(0x20,0x11); outb(0xA0,0x11); outb(0x21,32); outb(0xA1,40); outb(0x21,4); outb(0xA1,2); outb(0x21,1); outb(0xA1,1);
    outb(0x21,0xFC); outb(0xA1,0xFF);
}
static const char *exc_names[]={"Division por cero","Debug","NMI","Breakpoint","Overflow","Bound","Opcode invalido","Sin FPU","Doble falta","",
    "TSS invalido","Segmento ausente","Falta de pila","Proteccion general","Falta de pagina"};
static volatile uint32_t ticks;
int  proc_exec(char *path,int argc,char **argv,int bg);
void proc_list(void);
void proc_ps(void);
int  proc_kill(int pid,int sig);
void proc_pump(void);
void proc_set_idle(void (*f)(void));
void rootfs_load(uint32_t n,uint32_t addr);
void isr_handler64(struct regs *r){
    if(r->int_no<32){
        u64 cr2=0; if(r->int_no==14) __asm__ volatile("mov %%cr2,%0":"=r"(cr2));
        kprintf("\n[PANIC] excepcion %lu (%s) rip=0x%lx err=0x%lx",r->int_no,r->int_no<15?exc_names[r->int_no]:"reservada",r->rip,r->err);
        if(r->int_no==14) kprintf(" cr2=0x%lx",cr2);
        kprintf("\n");
        gui_panic(r->int_no<15?exc_names[r->int_no]:"reservada","rip",r->rip,r->err,cr2,r->int_no==14);
        for(;;) __asm__ volatile("cli;hlt");
    }
    int irq=(int)r->int_no-32;
    if(irq_handlers[irq]) irq_handlers[irq]();
    if(irq>=8) outb(0xA0,0x20);
    outb(0x20,0x20);
}

/* ---------- Timer PIT 100 Hz ---------- */
static void timer_irq(void){ ticks++; }
static void pit_init(void){ uint32_t div=1193182/100; outb(0x43,0x36); outb(0x40,(uint8_t)(div&0xFF)); outb(0x40,(uint8_t)(div>>8)); irq_handlers[0]=timer_irq; }

/* ---------- Teclado PS/2 (set 1; distribuciones en kbd.c) ---------- */
#define KB_N 128
static volatile unsigned short kb_buf[KB_N]; static volatile int kb_h, kb_t;
static void kb_push(unsigned short c){ int n=(kb_h+1)%KB_N; if(n!=kb_t){ kb_buf[kb_h]=c; kb_h=n; } }
static volatile unsigned char rk_buf[256]; static volatile int rk_h, rk_t;       /* bytes crudos del teclado, para la pantalla X */
static void rk_push(unsigned char b){ int n=(rk_h+1)&255; if(n!=rk_t){ rk_buf[rk_h]=b; rk_h=n; } }
static int rk_pop(void){ if(rk_t==rk_h) return -1; int b=rk_buf[rk_t]; rk_t=(rk_t+1)&255; return b; }
static void kb_irq(void){ unsigned char raw=inb(0x60); rk_push(raw); int c=kbd_feed(raw); if(c) kb_push((unsigned short)c); }   /* traduccion en kbd.c (US/Latam/ES) */

/* ---------- Raton PS/2 (IRQ12) ---------- */
struct mev { int dx, dy, b; };
static volatile struct mev mq[64]; static volatile int mq_h, mq_t;
static void mouse_irq(void){
    static uint8_t pk[3]; static int n; uint8_t d=inb(0x60);
    if(n==0 && !(d&8)) return;
    pk[n++]=d; if(n<3) return; n=0;
    if(pk[0]&0xC0) return;
    int dx=pk[1], dy=pk[2]; if(pk[0]&0x10) dx-=256; if(pk[0]&0x20) dy-=256;
    int nx=(mq_h+1)%64;
    if(nx!=mq_t){ mq[mq_h].dx=dx; mq[mq_h].dy=-dy; mq[mq_h].b=pk[0]&7; mq_h=nx; }
}
static int mouse_pop(struct mev *e){ if(mq_t==mq_h) return 0; e->dx=mq[mq_t].dx; e->dy=mq[mq_t].dy; e->b=mq[mq_t].b; mq_t=(mq_t+1)%64; return 1; }
static void kbc_wait_w(void){ int t=100000; while((inb(0x64)&2)&&t--); }
static void kbc_wait_r(void){ int t=100000; while(!(inb(0x64)&1)&&t--); }
static void mouse_cmd(uint8_t v){ kbc_wait_w(); outb(0x64,0xD4); kbc_wait_w(); outb(0x60,v); kbc_wait_r(); (void)inb(0x60); }
static void mouse_init(void){
    kbc_wait_w(); outb(0x64,0xA8);
    kbc_wait_w(); outb(0x64,0x20); kbc_wait_r(); uint8_t st=inb(0x60)|2;
    kbc_wait_w(); outb(0x64,0x60); kbc_wait_w(); outb(0x60,st);
    mouse_cmd(0xF6); mouse_cmd(0xF4);
    irq_handlers[12]=mouse_irq;
    outb(0x21,inb(0x21)&~4); outb(0xA1,inb(0xA1)&~0x10);
}
static int getc_nb(void){
    if(kb_t!=kb_h){ int c=kb_buf[kb_t]; kb_t=(kb_t+1)%KB_N; return c; }
    if(serial_has()){ char c=(char)inb(0x3F8); return c=='\r'?'\n':(c==127?'\b':c); }
    return -1;
}

/* ---------- Memoria: mapa de Multiboot + heap ---------- */
struct mb_info { uint32_t flags, mem_lower, mem_upper, boot_dev, cmdline, mods_n, mods_addr,
                 sym[4], mmap_len, mmap_addr, drives_len, drives_addr, config, loader, apm, vbe_ctrl, vbe_mode_info;
                 uint16_t vbe_mode, vbe_seg, vbe_off, vbe_len;
                 uint32_t fb_lo, fb_hi, fb_pitch, fb_w, fb_h; uint8_t fb_bpp, fb_type; } __attribute__((packed));
struct mb_mmap { uint32_t size; u64 base, len; uint32_t type; } __attribute__((packed));
extern char _kend;
static u64 ram_kb, heap_start, heap_end;
static void mem_init(struct mb_info *mi){
    u64 best_s=0, best_e=0, kend=((u64)(uintptr_t)&_kend+0xFFF)&~0xFFFull, LIM=0x80000000ull;   /* el heap queda bajo 2 GB (zona con cache) */
    if(mi->flags&(1u<<6)){
        u64 p=mi->mmap_addr, e=p+mi->mmap_len;
        while(p<e){
            struct mb_mmap *m=(struct mb_mmap*)(uintptr_t)p;
            if(m->type==1){
                ram_kb+=m->len/1024;
                u64 s=m->base, en=m->base+m->len; if(s<0x100000) s=0x100000; if(s<kend) s=kend; if(en>LIM) en=LIM;
                if(en>s && en-s>best_e-best_s){ best_s=s; best_e=en; }
            }
            p+=m->size+4;
        }
    } else { ram_kb=mi->mem_lower+mi->mem_upper; best_s=kend; best_e=0x100000ull+(u64)mi->mem_upper*1024; if(best_e>LIM) best_e=LIM; }
    if(mi->flags&(1u<<3)){                           /* modulos de arranque (rootfs): el heap empieza despues de ellos */
        struct { uint32_t s,e,c,pad; } *md=(void*)(uintptr_t)mi->mods_addr;
        for(uint32_t i=0;i<mi->mods_n;i++) if(md[i].s<best_e && md[i].e>best_s) best_s=md[i].e;
    }
    heap_start=(best_s+0xFFF)&~0xFFFull; heap_end=best_e;
}
struct hblk { u64 size, used; struct hblk *next, *prev; };      /* 32 bytes: mantiene la alineacion de 16 */
#define HDR ((u64)sizeof(struct hblk))
static struct hblk *heap_head; static u64 heap_total;
static void heap_init(u64 want){
    u64 avail=heap_end>heap_start?heap_end-heap_start:0;
    if(want>avail) want=avail;
    want&=~0xFFFull;
    heap_head=(struct hblk*)(uintptr_t)heap_start; heap_total=want;
    heap_head->size=want-HDR; heap_head->used=0; heap_head->next=heap_head->prev=0;
}
void *kmalloc(size_t n){
    if(!n||!heap_head) return 0;
    n=(n+15)&~(size_t)15;
    u64 f=irq_save(); void *r=0;
    for(struct hblk *b=heap_head;b;b=b->next){
        if(b->used||b->size<n) continue;
        if(b->size>=n+HDR+16){
            struct hblk *s=(struct hblk*)((uint8_t*)b+HDR+n);
            s->size=b->size-n-HDR; s->used=0; s->next=b->next; s->prev=b;
            if(b->next) b->next->prev=s;
            b->next=s; b->size=n;
        }
        b->used=1; r=(uint8_t*)b+HDR; break;
    }
    irq_restore(f); return r;
}
void kfree(void *p){
    if(!p) return;
    struct hblk *b=(struct hblk*)((uint8_t*)p-HDR); u64 f=irq_save();
    if(b->used){
        b->used=0;
        if(b->next && !b->next->used){ b->size+=HDR+b->next->size; b->next=b->next->next; if(b->next) b->next->prev=b; }
        if(b->prev && !b->prev->used){ struct hblk *pv=b->prev; pv->size+=HDR+b->size; pv->next=b->next; if(b->next) b->next->prev=pv; }
    }
    irq_restore(f);
}
static void heap_stats(u64 *used,u64 *freeb,u64 *largest){
    u64 u=0,fr=0,l=0; u64 f=irq_save();
    for(struct hblk *b=heap_head;b;b=b->next){ if(b->used) u+=b->size; else { fr+=b->size; if(b->size>l) l=b->size; } }
    irq_restore(f); *used=u; *freeb=fr; *largest=l;
}

/* ---------- Shell del kernel ---------- */
static int tokenize(char *l,char **a,int max){        /* separa por espacios; 'comillas' y "comillas" agrupan; las comillas se quitan */
    int n=0; char *p=l;
    while(*p&&n<max){
        while(*p==' ') p++;
        if(!*p) break;
        a[n++]=p; char *o=p; char q=0;
        while(*p&&(q||*p!=' ')){
            if(q){ if(*p==q){ q=0; p++; } else *o++=*p++; }
            else if(*p=='\''||*p=='"'){ q=*p++; }
            else *o++=*p++; }
        if(*p) p++;
        *o=0; }
    return n;
}
/* busca un programa por nombre en las carpetas habituales (PATH) */
static int find_program(const char *name,char *out,int max){
    static const char *dirs[]={"/bin","/usr/bin","/usr/local/bin","/sbin","/usr/sbin","/opt/bin"};
    if(!fs_root()) return 0;
    if(name[0]=='/'||(name[0]=='.'&&name[1]=='/')){ FsNode *f=fs_resolve(fs_root(),name); if(f&&!f->dir&&f->data){ k_strcpy(out,name,max); return 1; } return 0; }
    for(unsigned i=0;i<sizeof dirs/sizeof *dirs;i++){
        char p[160]; k_strcpy(p,dirs[i],sizeof p); int l=k_strlen(p); p[l++]='/'; k_strcpy(p+l,name,(int)sizeof p-l);
        FsNode *f=fs_resolve(fs_root(),p); if(f&&!f->dir&&(f->data||f->size)){ k_strcpy(out,p,max); return 1; } }
    return 0;
}
static int launch(char *path,int n,char **a,int bg){       /* en modo grafico el programa queda pegado a la ventana sin congelar el escritorio */
    int mode=bg?1:(gui_mode?2:0);
    int rc=proc_exec(path,n,a,mode);
    if(mode==0&&rc>=0) kprintf("[proceso termino con codigo %d]\n",rc); else if(mode==0&&rc<-1) kprintf("[proceso terminado por la senal %d]\n",-rc);
    return rc;
}
static int try_program(char *l){
    char *a[48]; int n=tokenize(l,a,48); if(!n) return 0;
    int bg=0; if(n>1&&streq(a[n-1],"&")){ bg=1; n--; }
    char path[160]; if(!find_program(a[0],path,sizeof path)) return 0;
    launch(path,n,a,bg); return 1;
}
/* pantalla: servidor X real (Xvfb) + puente de teclado/raton, mostrado en una ventana del escritorio */
static int x_up, x_gw=1024, x_gh=700;
static void x_start(void){
    char xvfb[160], xin[160];
    if(!find_program("Xvfb",xvfb,sizeof xvfb)){ kprintf("pantalla: falta el servidor X. Instalalo con:  instalar xvfb\n"); return; }
    FsNode *v=fs_find(fs_root(),"var"); FsNode *lib=v?fs_find(v,"lib"):0; if(v&&!lib){ lib=fs_mkdir(v,"lib"); if(lib) lib->sys=1; }
    FsNode *xd=lib?fs_find(lib,"xvfb"):0; if(lib&&!xd){ xd=fs_mkdir(lib,"xvfb"); if(xd) xd->sys=1; }
    int gw=1280, gh=800; if(gui_mode&&gui_screen_size(&gw,&gh)==0){ gw-=80; gh-=170; if(gw>1600) gw=1600; if(gh>1000) gh=1000; }
    gw&=~7; gh&=~7; if(gw<640) gw=640; if(gh<480) gh=480; x_gw=gw; x_gh=gh;
    char geo[40]; { int n=0; int v1=gw; char t[12]; int k=0; while(v1){ t[k++]=(char)('0'+v1%10); v1/=10; } while(k) geo[n++]=t[--k]; geo[n++]='x'; v1=gh; k=0; while(v1){ t[k++]=(char)('0'+v1%10); v1/=10; } while(k) geo[n++]=t[--k]; const char *e="x24"; while(*e) geo[n++]=*e++; geo[n]=0; }
    char *a[16]; int n=0; a[n++]="Xvfb"; a[n++]=":0"; a[n++]="-screen"; a[n++]="0"; a[n++]=geo; a[n++]="-fbdir"; a[n++]="/var/lib/xvfb"; a[n++]="-ac"; a[n++]="-nolisten"; a[n++]="tcp";
    kprintf("pantalla: iniciando el servidor X (%s)...\n",geo);
    proc_exec(xvfb,n,a,1);
    if(find_program("kng-xinput",xin,sizeof xin)){ char *b[2]={"kng-xinput",0}; proc_exec(xin,1,b,1); }
    else kprintf("pantalla: falta kng-xinput (teclado y raton no llegaran a los programas)\n");
    x_up=1;
}
static void cmd_pantalla(void){ if(!x_up) x_start(); if(x_up&&gui_mode) gui_open_xscreen(x_gw,x_gh); }
/* abrir <programa> [argumentos]: arranca la pantalla X si hace falta y abre el programa dentro de ella */
static void cmd_abrir(char *args){
    char *a[40]; int n=tokenize(args,a,38); if(!n){ kprintf("uso: abrir <programa> [argumentos]   (ej: abrir brave)\n"); return; }
    char w[160]; if(!find_program("abrir-x",w,sizeof w)){ kprintf("abrir: falta /usr/local/bin/abrir-x (sistema base incompleto)\n"); return; }
    cmd_pantalla(); if(!x_up) return;
    char *b[42]; b[0]="abrir-x"; for(int i=0;i<n;i++) b[i+1]=a[i]; b[n+1]=0;
    proc_exec(w,n+1,b,1);
}
static void run(char *l){
    while(*l==' ') l++;
    if(!*l) return;
    if(streq(l,"help")) kprintf("help info mem heap uptime clear echo <txt> reboot halt | ejecutar <ruta> [&] programas procesos matar <pid> rastro [0-2] | red ping <host> dns <host> wget <url> | pantalla abrir <programa> actualizar instalar quitar buscar (ayuda-programas)\n");
    else if(streq(l,"info")) kprintf("kng-kernel %s (%s, Multiboot)\nParte de Konigreich OS. Modo largo, IDT, heap, interfaz grafica, red. Sin procesos todavia.\n",KNG_VERSION,KNG_ARCH);
    else if(streq(l,"mem")) kprintf("RAM usable: %lu KB | heap: %lu KB\n",ram_kb,heap_total/1024);
    else if(streq(l,"heap")){ u64 u,f,m; heap_stats(&u,&f,&m); kprintf("heap: total %lu KB | usado %lu B | libre %lu B | bloque libre mayor %lu B\n",heap_total/1024,u,f,m); }
    else if(streq(l,"uptime")) kprintf("%u.%u s\n",ticks/100,(ticks%100)/10);
    else if(startswith(l,"ejecutar ")||startswith(l,"run ")){
        char *a[48]; int bg=0; char *p=l; while(*p&&*p!=' ') p++;
        int n=tokenize(p,a,48);
        if(n>1&&streq(a[n-1],"&")){ bg=1; n--; }
        if(n){ char path[160]; if(!find_program(a[0],path,sizeof path)) k_strcpy(path,a[0],sizeof path); launch(path,n,a,bg); } else kprintf("uso: ejecutar <ruta> [argumentos] [&]\n"); }
    else if(startswith(l,"rastro")){ extern int krastro; krastro=l[6]==' '&&l[7]>='0'&&l[7]<='2'?l[7]-'0':1; kprintf("rastro de llamadas al sistema: nivel %d (0 apagado, 1 solo errores, 2 todo)\n",krastro); }
    else if(startswith(l,"xevento ")){                      /* prueba: manda un evento de entrada (tipo codigo valor, decimales) al servidor X */
        int v[3]={0,0,0},k=0,neg=0; for(const char *q=l+8;*q&&k<3;q++){ if(*q==' '){ k++; neg=0; } else if(*q=='-') neg=1; else if(*q>='0'&&*q<='9') v[k]=neg?v[k]*10-(*q-'0'):v[k]*10+(*q-'0'); }
        kinput_event(v[0],v[1],v[2]); kprintf("evento enviado %d %d %d\n",v[0],v[1],v[2]); }
    else if(startswith(l,"abrir ")||streq(l,"abrir")) cmd_abrir(l+(l[5]?6:5));
    else if(streq(l,"pantalla")) cmd_pantalla();
    else if(streq(l,"procesos")||streq(l,"ps")) proc_ps();
    else if(startswith(l,"matar ")){ int pid=0; for(const char *q=l+6;*q>='0'&&*q<='9';q++) pid=pid*10+(*q-'0'); kprintf(proc_kill(pid,9)?"senal enviada\n":"no existe ese proceso\n"); }
    else if(streq(l,"programas")||streq(l,"progs")) proc_list();
    else if(streq(l,"sh")||streq(l,"selftest")) kprintf("no disponible en el kernel de 64 bits todavia\n");
    else if(streq(l,"clear")) clear();
    else if(startswith(l,"echo ")) kprintf("%s\n",l+5);
    else if(streq(l,"reboot")){ outb(0x64,0xFE); for(;;) __asm__ volatile("hlt"); }
    else if(streq(l,"halt")){ kprintf("Sistema detenido.\n"); for(;;) __asm__ volatile("cli;hlt"); }
    else { char cp[200]; k_strcpy(cp,l,sizeof cp); if(!try_program(cp)) kprintf("comando desconocido: %s (prueba 'help')\n",l); }
}

/* ---------- API para la interfaz grafica (kng.h) ---------- */
void k_write_console(const char *s,int n){ for(int i=0;i<n;i++) putc(s[i]); }
uint32_t kticks(void){ return ticks; }
int kgetc(void){ return getc_nb(); }
void kshell_exec(char *l){ if(net_cmd(l)) return; run(l); }
void proc_stdin_char(int c);
void kproc_key(int c){ proc_stdin_char(c); }
void kheap_info(uint64_t *used,uint64_t *freeb){ uint64_t m; heap_stats(used,freeb,&m); }
uint32_t kram_kb(void){ return ram_kb>0xFFFFFFFFull?0xFFFFFFFFu:(uint32_t)ram_kb; }
extern uint64_t pd[2048];
static void fb_wc(uint32_t addr,uint32_t bytes){      /* framebuffer en write-combining (PAT); ver kernel/kernel.c */
    uint32_t a,b,c,d; __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1));
    if(!(d&(1u<<16)) || addr<0x80000000u) return;
    uint32_t i0=addr>>21, i1=(addr+bytes-1)>>21, nm=net_mmio_addr();
    if(nm && (nm>>21)>=i0 && (nm>>21)<=i1) return;
    __asm__ volatile("wbinvd":::"memory");
    __asm__ volatile("wrmsr"::"c"(0x277),"a"(0x00070106u),"d"(0x00070406u):"memory");
    for(uint32_t i=i0;i<=i1 && i<2048;i++) pd[i]=(pd[i]&~0x10ull)|0x08ull;
    uint64_t cr3; __asm__ volatile("mov %%cr3,%0":"=r"(cr3)); __asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");
}
void kreboot(void){ outb(0x64,0xFE); for(;;) __asm__ volatile("hlt"); }
void khalt(void){ kprintf("Apagando...\n"); outw(0x604,0x2000); outw(0xB004,0x2000); outw(0x4004,0x3400); for(;;) __asm__ volatile("cli;hlt"); }
static uint8_t cmos(uint8_t r){ outb(0x70,r); return inb(0x71); }
/* fecha y hora completas del reloj de la placa (se supone UTC), en segundos desde 1970 */
static uint64_t days_from_civil(int y,int m,int d){ y-=m<=2; int era=(y>=0?y:y-399)/400; unsigned yoe=(unsigned)(y-era*400); unsigned doy=(153u*(unsigned)(m+(m>2?-3:9))+2)/5+(unsigned)d-1; unsigned doe=yoe*365+yoe/4-yoe/100+doy; return (uint64_t)((long)era*146097+(long)doe-719468); }
uint64_t krtc_epoch(void){
    int t=0; while((cmos(0x0A)&0x80)&&++t<10000);
    uint8_t b=cmos(0x0B); int bcd=!(b&4);
    #define BCDV(v) (bcd?((((v)>>4)*10)+((v)&15)):(v))
    int s=BCDV(cmos(0)), mi=BCDV(cmos(2)); uint8_t hr=cmos(4); int pm=hr&0x80; int h=BCDV(hr&0x7F);
    if(!(b&2)){ if(h==12) h=0; if(pm) h+=12; }
    int d=BCDV(cmos(7)), mo=BCDV(cmos(8)), y=BCDV(cmos(9)); uint8_t c=cmos(0x32); int cen=BCDV(c); if(cen<19||cen>30) cen=20;
    y+=cen*100; if(mo<1||mo>12||d<1||d>31) return 1767225600ull;     /* reloj ilegible: 2026-01-01 */
    return days_from_civil(y,mo,d)*86400ull+(uint64_t)h*3600+(uint64_t)mi*60+(uint64_t)s;
}
int krtc(int *h,int *m){
    int t=0; while((cmos(0x0A)&0x80) && ++t<10000);
    uint8_t b=cmos(0x0B), hh=cmos(4), mm=cmos(2); int pm=hh&0x80; hh&=0x7F;
    if(!(b&4)){ hh=(uint8_t)((hh&0x0F)+(hh>>4)*10); mm=(uint8_t)((mm&0x0F)+(mm>>4)*10); }
    if(!(b&2)){ hh%=12; if(pm) hh+=12; }
    *h=hh%24; *m=mm%60; return 0;
}

static void idle64(void){ net_poll(); if(gui_mode) gui_frame(); }
void kmain64(uint32_t magic, struct mb_info *mi){
    serial_init(); clear();
    uint32_t fb_addr=0,fb_pitch=0,fb_w=0,fb_h=0,fb_ok=0;
    if(magic==0x2BADB002 && (mi->flags&(1u<<12)) && mi->fb_type==1 && mi->fb_bpp==32 && mi->fb_hi==0 && mi->fb_w>=640 && mi->fb_h>=480){
        fb_addr=mi->fb_lo; fb_pitch=mi->fb_pitch; fb_w=mi->fb_w; fb_h=mi->fb_h; fb_ok=1; }
    kprintf("== kng-kernel %s (%s) :: Konigreich OS ==\n",KNG_VERSION,KNG_ARCH);
    if(magic!=0x2BADB002){ kprintf("Arranque no Multiboot (magic=0x%x). Detenido.\n",magic); for(;;) __asm__ volatile("hlt"); }
    kprintf("[ok] modo largo (64 bits), paginacion de 4 GB en identidad\n");
    idt_init(); pic_init(); pit_init(); irq_handlers[1]=kb_irq; mouse_init();
    mem_init(mi); heap_init(~0ull);
    __asm__ volatile("sti");
    kprintf("[ok] IDT, PIC, PIT(100Hz), teclado y raton PS/2\n");
    kprintf("[ok] RAM utilizable: %lu KB, heap %lu KB\n",ram_kb,heap_total/1024);
    if(!fs_root()) fs_init();
    proc_set_idle(idle64);
    if(!fb_ok && (mi->flags&8) && mi->mods_n) rootfs_load(mi->mods_n,mi->mods_addr);
    if(net_init()==0){ char nb[90]; net_info(nb,sizeof nb); kprintf("[ok] %s\n",nb); } else kprintf("[i] sin tarjeta de red compatible (e1000 / RTL8139)\n");
    if(fb_ok){
        kprintf("[ok] framebuffer %ux%u 32 bpp en 0x%x. Iniciando escritorio...\n",fb_w,fb_h,fb_addr);
        fb_wc(fb_addr,fb_pitch*fb_h);
        if(gui_init((uint32_t*)(uintptr_t)fb_addr,(int)fb_w,(int)fb_h,(int)fb_pitch)==0){
            gui_mode=1;
            if((mi->flags&8) && mi->mods_n) rootfs_load(mi->mods_n,mi->mods_addr);
            for(;;){
                { int rb; while((rb=rk_pop())>=0) gui_rawkey(rb); }
                int c; while((c=getc_nb())>=0) gui_key(c);
                struct mev e; while(mouse_pop(&e)) gui_mouse_event(e.dx,e.dy,e.b);
                net_poll(); proc_pump(); gui_frame();
                __asm__ volatile("hlt");
            }
        }
        kprintf("[!] no se pudo iniciar la interfaz grafica (memoria). Modo texto.\n");
    } else kprintf("[i] sin framebuffer (arranque en modo texto).\n");
    kprintf("Escribe 'help'.\n");
    char line[128]; int n=0; kprintf("kng> ");
    for(;;){
        int c=getc_nb();
        if(c<0){ net_poll(); proc_pump(); __asm__ volatile("hlt"); continue; }
        if(c=='\n'){ putc('\n'); line[n]=0; kshell_exec(line); n=0; kprintf("kng> "); }
        else if(c=='\b'){ if(n>0){ n--; putc('\b'); } }
        else if(c>=32 && n<127){ line[n++]=(char)c; putc((char)c); }
    }
}
