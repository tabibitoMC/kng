/* kng-kernel 0.5 — kernel propio de Königreich OS (i386, Multiboot).
 * Consola VGA + serie, GDT/IDT, PIC, PIT, teclado PS/2, memoria física, paginación,
 * heap (kmalloc/kfree), hilos con planificador expropiativo, PMM con liberacion (bitmap),
 * procesos de usuario en ring 3 (espacio propio, TSS), ELF32, fork/exec/wait/brk, syscalls (numeros Linux i386) y shell.
 * Escrito desde cero; usa como referencia la documentación pública (Intel SDM, OSDev). */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "kng.h"
#include "gui.h"
#include "kbd.h"
#include "net.h"


/* ---------- E/S de puertos ---------- */
static inline void outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

static inline uint32_t irq_save(void){ uint32_t f; __asm__ volatile("pushf; pop %0; cli":"=r"(f)::"memory"); return f; }
static inline void irq_restore(uint32_t f){ if(f&0x200) __asm__ volatile("sti":::"memory"); }

/* ---------- Consola: VGA texto + puerto serie COM1 ---------- */
static volatile uint16_t *const VGA = (uint16_t*)0xB8000;
static int cx, cy;
static const uint8_t COLOR = 0x0B; /* cian claro sobre negro */

static void serial_init(void){
    outb(0x3F9,0); outb(0x3FB,0x80); outb(0x3F8,1); outb(0x3F9,0);
    outb(0x3FB,0x03); outb(0x3FA,0xC7); outb(0x3FC,0x0B);
}
static void serial_put(char c){ while(!(inb(0x3FD)&0x20)); outb(0x3F8,(uint8_t)c); }
static int serial_has(void){ return inb(0x3FD)&1; }

static void vga_scroll(void){
    for(int i=0;i<80*24;i++) VGA[i]=VGA[i+80];
    for(int i=80*24;i<80*25;i++) VGA[i]=(COLOR<<8)|' ';
    cy=24;
}
void (*kcon_hook)(char);
static int gui_mode;
static void putc(char c){
    if(gui_mode){ if(c=='\n') serial_put('\r'); serial_put(c); if(kcon_hook) kcon_hook(c); return; }   /* en modo grafico: serie + ventana de terminal */
    if(c=='\n'){ serial_put('\r'); serial_put('\n'); cx=0; cy++; }
    else if(c=='\b'){ serial_put('\b'); serial_put(' '); serial_put('\b');
        if(cx>0){ cx--; VGA[cy*80+cx]=(COLOR<<8)|' '; } }
    else { serial_put(c); VGA[cy*80+cx]=(COLOR<<8)|(uint8_t)c; cx++; }
    if(cx>=80){ cx=0; cy++; }
    if(cy>=25) vga_scroll();
}
static void clear(void){
    for(int i=0;i<80*25;i++) VGA[i]=(COLOR<<8)|' ';
    cx=cy=0;
}
static void puts_(const char*s){ while(*s) putc(*s++); }

static void print_num(uint32_t v, unsigned base, int neg){
    char b[34]; int i=0;
    if(!v) b[i++]='0';
    while(v){ b[i++]="0123456789abcdef"[v%base]; v/=base; }
    if(neg) putc('-');
    while(i) putc(b[--i]);
}
void kprintf(const char*f, ...){
    uint32_t fl=irq_save();
    va_list a; va_start(a,f);
    for(; *f; f++){
        if(*f!='%'){ putc(*f); continue; }
        switch(*++f){
            case 's': { const char*s=va_arg(a,const char*); puts_(s?s:"(null)"); break; }
            case 'c': putc((char)va_arg(a,int)); break;
            case 'd': { int v=va_arg(a,int); print_num(v<0?-(uint32_t)v:(uint32_t)v,10,v<0); break; }
            case 'u': print_num(va_arg(a,uint32_t),10,0); break;
            case 'x': print_num(va_arg(a,uint32_t),16,0); break;
            case '%': putc('%'); break;
            default: putc('%'); putc(*f);
        }
    }
    va_end(a);
    irq_restore(fl);
}

/* ---------- Cadenas ---------- */
static int streq(const char*a,const char*b){ while(*a&&*a==*b){a++;b++;} return *a==*b; }
static int startswith(const char*s,const char*p){ while(*p){ if(*s++!=*p++) return 0; } return 1; }

void *memset(void *d,int c,size_t n){          /* por palabras de 32 bits: la interfaz limpia/copia megabytes por cuadro */
    void *r=d; uint32_t v=(uint8_t)c; v|=v<<8; v|=v<<16; size_t q=n>>2;
    __asm__ volatile("rep stosl":"+D"(d),"+c"(q):"a"(v):"memory");
    n&=3; __asm__ volatile("rep stosb":"+D"(d),"+c"(n):"a"(v):"memory"); return r; }
void *memcpy(void *d,const void*s,size_t n){
    void *r=d; size_t q=n>>2;
    __asm__ volatile("rep movsl":"+D"(d),"+S"(s),"+c"(q)::"memory");
    n&=3; __asm__ volatile("rep movsb":"+D"(d),"+S"(s),"+c"(n)::"memory"); return r; }
static int atoi_(const char*s){ int v=0; while(*s>='0'&&*s<='9') v=v*10+(*s++-'0'); return v; }

/* ---------- GDT ---------- */
struct gdt_e { uint16_t lim, base_lo; uint8_t base_mid, access, gran, base_hi; } __attribute__((packed));
struct dtr   { uint16_t limit; uint32_t base; } __attribute__((packed));
static struct gdt_e gdt[6];
struct tss_t { uint32_t prev,esp0,ss0,esp1,ss1,esp2,ss2,cr3,eip,eflags,eax,ecx,edx,ebx,esp,ebp,esi,edi,
               es,cs,ss,ds,fs,gs,ldt; uint16_t trap,iomap; } __attribute__((packed));
static struct tss_t tss;
static void gdt_set(int i, uint8_t access, uint8_t gran){
    gdt[i].lim=0xFFFF; gdt[i].base_lo=0; gdt[i].base_mid=0;
    gdt[i].access=access; gdt[i].gran=gran; gdt[i].base_hi=0;
}
/* selectores: 0x08 codigo K, 0x10 datos K, 0x18 codigo U (0x1B con RPL3), 0x20 datos U (0x23), 0x28 TSS */
static void gdt_init(void){
    gdt_set(0,0,0); gdt_set(1,0x9A,0xCF); gdt_set(2,0x92,0xCF); gdt_set(3,0xFA,0xCF); gdt_set(4,0xF2,0xCF);
    tss.ss0=0x10; tss.iomap=sizeof(tss);
    uint32_t tb=(uint32_t)&tss, tl=sizeof(tss)-1;
    gdt[5].lim=tl&0xFFFF; gdt[5].base_lo=tb&0xFFFF; gdt[5].base_mid=(tb>>16)&0xFF;
    gdt[5].access=0x89; gdt[5].gran=(tl>>16)&0x0F; gdt[5].base_hi=tb>>24;
    struct dtr d={sizeof(gdt)-1,(uint32_t)gdt};
    __asm__ volatile("lgdt %0\n"
        "mov $0x10,%%ax\n mov %%ax,%%ds\n mov %%ax,%%es\n mov %%ax,%%fs\n mov %%ax,%%gs\n mov %%ax,%%ss\n"
        "ljmp $0x08,$1f\n1:\n mov $0x28,%%ax\n ltr %%ax\n"::"m"(d):"ax","memory");
}

/* ---------- IDT + PIC ---------- */
struct idt_e { uint16_t lo; uint16_t sel; uint8_t zero, flags; uint16_t hi; } __attribute__((packed));
static struct idt_e idt[256];
extern uint32_t isr_table[];
struct regs { uint32_t edi,esi,ebp,esp,ebx,edx,ecx,eax,int_no,err,eip,cs,eflags,useresp,ss; };
typedef void (*irq_fn)(void);
static irq_fn irq_handlers[16];

static void idt_init(void){
    for(int i=0;i<48;i++){
        uint32_t a=isr_table[i];
        idt[i].lo=a&0xFFFF; idt[i].hi=a>>16; idt[i].sel=0x08; idt[i].zero=0; idt[i].flags=0x8E;
    }
    extern void isr128(void);
    { uint32_t a=(uint32_t)isr128;                      /* int 0x80: puerta DPL3 para syscalls de ring 3 */
      idt[128].lo=a&0xFFFF; idt[128].hi=a>>16; idt[128].sel=0x08; idt[128].zero=0; idt[128].flags=0xEE; }
    struct dtr d={sizeof(idt)-1,(uint32_t)idt};
    __asm__ volatile("lidt %0"::"m"(d));
}
static void pic_init(void){
    outb(0x20,0x11); outb(0xA0,0x11);
    outb(0x21,32);   outb(0xA1,40);
    outb(0x21,4);    outb(0xA1,2);
    outb(0x21,1);    outb(0xA1,1);
    outb(0x21,0xFC); outb(0xA1,0xFF); /* sólo IRQ0 (timer) e IRQ1 (teclado) */
}

static const char *exc_names[]={"Division por cero","Debug","NMI","Breakpoint","Overflow","Bound",
    "Opcode invalido","Sin FPU","Doble falta","","TSS invalido","Segmento ausente","Falta de pila",
    "Proteccion general","Falta de pagina"};

static struct regs *schedule(struct regs *r);
static struct regs *syscall_dispatch(struct regs *r);
static struct regs *user_fault(struct regs *r, uint32_t cr2);
struct regs *isr_handler(struct regs *r){
    if(r->int_no==128) return syscall_dispatch(r);
    if(r->int_no<32){
        uint32_t cr2=0;
        if(r->int_no==14) __asm__ volatile("mov %%cr2,%0":"=r"(cr2));
        if((r->cs&3)==3) return user_fault(r,cr2);      /* fallo en ring 3: se mata el proceso, el kernel sigue */
        kprintf("\n[PANIC] excepcion %u (%s) eip=0x%x err=0x%x", r->int_no,
            r->int_no<15?exc_names[r->int_no]:"reservada", r->eip, r->err);
        if(r->int_no==14) kprintf(" cr2=0x%x", cr2);
        kprintf("\n");
        if(gui_mode) gui_panic(r->int_no<15?exc_names[r->int_no]:"reservada","eip",r->eip,r->err,cr2,r->int_no==14);
        for(;;) __asm__ volatile("cli;hlt");
    }
    int irq=r->int_no-32;
    if(irq_handlers[irq]) irq_handlers[irq]();
    if(irq>=8) outb(0xA0,0x20);
    outb(0x20,0x20);
    if(irq==0) return schedule(r);   /* cambio de contexto: devolvemos la pila de la tarea elegida */
    return r;
}

/* ---------- Timer PIT 100 Hz ---------- */
static volatile uint32_t ticks;
static void timer_irq(void){ ticks++; }
static void pit_init(void){
    uint32_t div=1193182/100;
    outb(0x43,0x36); outb(0x40,div&0xFF); outb(0x40,div>>8);
    irq_handlers[0]=timer_irq;
}

/* ---------- Teclado PS/2 (set 1; distribuciones en kbd.c) ---------- */
#define KB_N 128
static volatile unsigned char kb_buf[KB_N]; static volatile int kb_h, kb_t;
static void kb_push(unsigned char c){ int n=(kb_h+1)%KB_N; if(n!=kb_t){ kb_buf[kb_h]=c; kb_h=n; } }
static void kb_irq(void){ int c=kbd_feed(inb(0x60)); if(c) kb_push((unsigned char)c); }   /* traduccion en kbd.c (US/Latam/ES) */

/* ---------- Raton PS/2 (IRQ12) ---------- */
struct mev { int dx, dy, b; };
static volatile struct mev mq[64]; static volatile int mq_h, mq_t;
static void mouse_irq(void){
    static uint8_t pk[3]; static int n;
    uint8_t d=inb(0x60);
    if(n==0 && !(d&8)) return;                      /* desincronizado: esperar primer byte valido */
    pk[n++]=d;
    if(n<3) return;
    n=0;
    if(pk[0]&0xC0) return;                          /* desbordamiento */
    int dx=pk[1], dy=pk[2];
    if(pk[0]&0x10) dx-=256;
    if(pk[0]&0x20) dy-=256;
    int nx=(mq_h+1)%64;
    if(nx!=mq_t){ mq[mq_h].dx=dx; mq[mq_h].dy=-dy; mq[mq_h].b=pk[0]&7; mq_h=nx; }
}
static int mouse_pop(struct mev *e){
    if(mq_t==mq_h) return 0;
    e->dx=mq[mq_t].dx; e->dy=mq[mq_t].dy; e->b=mq[mq_t].b; mq_t=(mq_t+1)%64; return 1;
}
static void kbc_wait_w(void){ int t=100000; while((inb(0x64)&2)&&t--); }
static void kbc_wait_r(void){ int t=100000; while(!(inb(0x64)&1)&&t--); }
static void mouse_cmd(uint8_t v){ kbc_wait_w(); outb(0x64,0xD4); kbc_wait_w(); outb(0x60,v); kbc_wait_r(); (void)inb(0x60); }
static void mouse_init(void){
    kbc_wait_w(); outb(0x64,0xA8);                              /* habilitar puerto auxiliar */
    kbc_wait_w(); outb(0x64,0x20); kbc_wait_r(); uint8_t st=inb(0x60)|2;
    kbc_wait_w(); outb(0x64,0x60); kbc_wait_w(); outb(0x60,st);  /* habilitar IRQ12 */
    mouse_cmd(0xF6); mouse_cmd(0xF4);                           /* valores por defecto + activar envio de paquetes */
    irq_handlers[12]=mouse_irq;
    outb(0x21,inb(0x21)&~4); outb(0xA1,inb(0xA1)&~0x10);        /* desenmascarar cascada (IRQ2) e IRQ12 */
}
static int getc_nb(void){
    if(kb_t!=kb_h){ int c=kb_buf[kb_t]; kb_t=(kb_t+1)%KB_N; return c; }
    if(serial_has()){ char c=inb(0x3F8); return c=='\r'?'\n':(c==127?'\b':c); }
    return -1;
}

/* ---------- Memoria física ---------- */
struct mb_info { uint32_t flags, mem_lower, mem_upper, boot_dev, cmdline, mods_n, mods_addr,
                 sym[4], mmap_len, mmap_addr, drives_len, drives_addr, config, loader, apm, vbe_ctrl, vbe_mode_info;
                 uint16_t vbe_mode, vbe_seg, vbe_off, vbe_len;
                 uint32_t fb_lo, fb_hi, fb_pitch, fb_w, fb_h; uint8_t fb_bpp, fb_type; } __attribute__((packed));
struct mb_mmap { uint32_t size; uint64_t base, len; uint32_t type; } __attribute__((packed));
extern char _kend;
static uint32_t ram_usable_kb, pmm_next, pmm_end, pages_alloc;

static void mem_init(struct mb_info *mi){
    uint32_t best_base=0, best_len=0;
    if(mi->flags&(1<<6)){
        uint32_t p=mi->mmap_addr, e=p+mi->mmap_len;
        while(p<e){
            struct mb_mmap *m=(struct mb_mmap*)p;
            if(m->type==1 && m->base<0xFFFFFFFFull){
                uint32_t b=(uint32_t)m->base, l=(m->len>0xFFFFFFFFull-b)?(0xFFFFFFFFu-b):(uint32_t)m->len;
                ram_usable_kb+=l/1024;
                if(b>=0x100000 && l>best_len){ best_base=b; best_len=l; }
            }
            p+=m->size+4;
        }
    } else ram_usable_kb=mi->mem_lower+mi->mem_upper;
    uint32_t kend=((uint32_t)&_kend+0xFFF)&~0xFFFu;
    if(kend>best_base){ best_len-= (kend-best_base<best_len)?(kend-best_base):best_len; best_base=kend; }
    pmm_next=(best_base+0xFFF)&~0xFFFu; pmm_end=best_base+best_len;
}
/* PMM con bitmap: tras heap_init se activa; permite liberar paginas (procesos de usuario). */
#define PMM_MAX_PAGES 262144u            /* hasta 1 GB gestionado */
static uint32_t pmm_bm[PMM_MAX_PAGES/32];
static uint32_t pmm_base, pmm_pages, pmm_hint, pmm_ready;
static void pmm_init(void){
    pmm_base=(pmm_next+0xFFF)&~0xFFFu;
    uint32_t n=(pmm_end>pmm_base)?(pmm_end-pmm_base)>>12:0;
    pmm_pages=n>PMM_MAX_PAGES?PMM_MAX_PAGES:n; pmm_hint=0; pages_alloc=0; pmm_ready=1;
}
static void *page_alloc(void){
    if(!pmm_ready){
        if(pmm_next+4096>pmm_end) return 0;
        void *p=(void*)pmm_next; pmm_next+=4096; pages_alloc++; return p;
    }
    for(uint32_t i=pmm_hint;i<pmm_pages;i++){
        if((i&31)==0 && pmm_bm[i>>5]==0xFFFFFFFFu){ i+=31; continue; }
        if(!(pmm_bm[i>>5]&(1u<<(i&31)))){
            pmm_bm[i>>5]|=1u<<(i&31); pmm_hint=i+1; pages_alloc++;
            return (void*)(pmm_base+(i<<12));
        }
    }
    return 0;
}
static void page_free(void *p){
    uint32_t a=(uint32_t)p;
    if(!pmm_ready || a<pmm_base || (a&0xFFF)) return;
    uint32_t i=(a-pmm_base)>>12;
    if(i>=pmm_pages || !(pmm_bm[i>>5]&(1u<<(i&31)))) return;
    pmm_bm[i>>5]&=~(1u<<(i&31)); pages_alloc--;
    if(i<pmm_hint) pmm_hint=i;
}
static void *page_alloc_z(void){ void *p=page_alloc(); if(p) memset(p,0,4096); return p; }

/* ---------- Memoria virtual (paginación i386) ----------
 * Mapa identidad de los 4 GB con páginas de 4 MB (PSE). vmm_map/vmm_unmap parten una
 * página de 4 MB en tabla de 4 KB cuando hace falta granularidad fina. */
#define PG_P  1u
#define PG_RW 2u
#define PG_PS 0x80u
static uint32_t page_dir[1024] __attribute__((aligned(4096)));
/* Framebuffer en "write-combining" (via PAT): escribir pixeles sin cache es muy lento; con WC es varias veces mas rapido. */
static void fb_wc(uint32_t addr,uint32_t bytes){
    uint32_t a,b,c,d; __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1));
    if(!(d&(1u<<16)) || addr<0x80000000u) return;                         /* sin PAT, o ya esta cacheado */
    uint32_t i0=addr>>22, i1=(addr+bytes-1)>>22, nm=net_mmio_addr();
    if(nm && (nm>>22)>=i0 && (nm>>22)<=i1) return;                         /* la tarjeta de red comparte pagina: no tocar */
    __asm__ volatile("wbinvd":::"memory");
    __asm__ volatile("wrmsr"::"c"(0x277),"a"(0x00070106u),"d"(0x00070406u):"memory");   /* PAT1 = WC */
    for(uint32_t i=i0;i<=i1 && i<1024;i++) page_dir[i]=(page_dir[i]&~0x10u)|0x08u;      /* PWT sin PCD => indice PAT 1 */
    uint32_t cr3; __asm__ volatile("mov %%cr3,%0":"=r"(cr3)); __asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");
}


static void vmm_init(void){
    for(uint32_t i=0;i<1024;i++) page_dir[i]=(i<<22)|PG_P|PG_RW|PG_PS|(i>=512?0x18u:0);   /* >=2 GB: PWT|PCD (sin cache) para MMIO de tarjetas PCI y framebuffer */
    uint32_t cr4; __asm__ volatile("mov %%cr4,%0":"=r"(cr4));
    cr4|=0x10; __asm__ volatile("mov %0,%%cr4"::"r"(cr4):"memory");
    __asm__ volatile("mov %0,%%cr3"::"r"((uint32_t)page_dir):"memory");
    uint32_t cr0; __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    cr0|=0x80000000u; __asm__ volatile("mov %0,%%cr0"::"r"(cr0):"memory");
}
static uint32_t *vmm_table(uint32_t virt){
    uint32_t idx=virt>>22, e=page_dir[idx];
    if(e&PG_PS){
        uint32_t *pt=page_alloc(); if(!pt) return 0;
        uint32_t base=e&0xFFC00000u;
        for(uint32_t j=0;j<1024;j++) pt[j]=(base+(j<<12))|PG_P|PG_RW|((e&0x18u)?0x18u:0);
        page_dir[idx]=(uint32_t)pt|PG_P|PG_RW;
        __asm__ volatile("mov %0,%%cr3"::"r"((uint32_t)page_dir):"memory");
        return pt;
    }
    if(!(e&PG_P)){
        uint32_t *pt=page_alloc(); if(!pt) return 0;
        memset(pt,0,4096); page_dir[idx]=(uint32_t)pt|PG_P|PG_RW; return pt;
    }
    return (uint32_t*)(e&~0xFFFu);
}
static int vmm_map(uint32_t virt, uint32_t phys, uint32_t flags){
    uint32_t *pt=vmm_table(virt); if(!pt) return -1;
    pt[(virt>>12)&0x3FF]=(phys&~0xFFFu)|flags|PG_P;
    __asm__ volatile("invlpg (%0)"::"r"(virt):"memory");
    return 0;
}
static int vmm_unmap(uint32_t virt){
    uint32_t *pt=vmm_table(virt); if(!pt) return -1;
    pt[(virt>>12)&0x3FF]=0;
    __asm__ volatile("invlpg (%0)"::"r"(virt):"memory");
    return 0;
}
static uint32_t vmm_phys(uint32_t virt){   /* 0 = no mapeada */
    uint32_t e=page_dir[virt>>22];
    if(!(e&PG_P)) return 0;
    if(e&PG_PS) return (e&0xFFC00000u)|(virt&0x3FFFFFu);
    uint32_t p=((uint32_t*)(e&~0xFFFu))[(virt>>12)&0x3FF];
    if(!(p&PG_P)) return 0;
    return (p&~0xFFFu)|(virt&0xFFFu);
}

/* ---------- Heap del kernel: kmalloc / kfree (primer ajuste + fusión) ---------- */
struct hblk { uint32_t size, used; struct hblk *next, *prev; };
#define HDR ((uint32_t)sizeof(struct hblk))
static struct hblk *heap_head; static uint32_t heap_total;

static void heap_init(uint32_t want){
    uint32_t avail=pmm_end-pmm_next;
    if(want>avail/2) want=avail/2;
    want&=~0xFFFu;
    heap_head=(struct hblk*)pmm_next; pmm_next+=want; heap_total=want;
    heap_head->size=want-HDR; heap_head->used=0; heap_head->next=heap_head->prev=0;
}
void *kmalloc(size_t n){
    if(!n) return 0;
    n=(n+15)&~15u;
    uint32_t f=irq_save(); void *r=0;
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
    struct hblk *b=(struct hblk*)((uint8_t*)p-HDR);
    uint32_t f=irq_save();
    if(b->used){
        b->used=0;
        if(b->next && !b->next->used){ b->size+=HDR+b->next->size; b->next=b->next->next; if(b->next) b->next->prev=b; }
        if(b->prev && !b->prev->used){ struct hblk *pv=b->prev; pv->size+=HDR+b->size; pv->next=b->next; if(b->next) b->next->prev=pv; }
    }
    irq_restore(f);
}
static void heap_stats(uint32_t *used, uint32_t *freeb, uint32_t *largest){
    uint32_t u=0,fr=0,l=0; uint32_t f=irq_save();
    for(struct hblk *b=heap_head;b;b=b->next){
        if(b->used) u+=b->size; else { fr+=b->size; if(b->size>l) l=b->size; }
    }
    irq_restore(f); *used=u; *freeb=fr; *largest=l;
}

/*HT-BEGIN*/
/* ---------- Procesos de usuario: espacios de direcciones + cargador ELF32 ----------
 * Region de usuario: 0x40000000..0x407FFFFF (8 MB, dos entradas del directorio). Cada proceso tiene su
 * propio page directory: copia del kernel (identidad, solo supervisor) + tablas propias con bit US. */
#define PG_US 4u
#define USER_BASE 0x40000000u
#define USER_TOP  0x40800000u
#define USER_STACK_PAGES 4u
#define USER_STACK_LOW (USER_TOP-0x10000u)
#define HEAP_BASE  0x40200000u        /* brk de cada proceso: 0x40200000..0x406FFFFF */
#define HEAP_LIMIT 0x40700000u
#define ELF_LIMIT  HEAP_BASE          /* el ELF debe caber debajo del heap */
#define USER_IDX0 (USER_BASE>>22)
#define USER_IDX1 ((USER_TOP-1)>>22)

static uint32_t pte_of(const uint32_t *dir, uint32_t va){      /* 0 si no hay tabla de 4 KB */
    uint32_t e=dir[va>>22];
    if(!(e&PG_P) || (e&PG_PS)) return 0;
    return ((const uint32_t*)(e&~0xFFFu))[(va>>12)&0x3FF];
}
static int proc_map(uint32_t *dir, uint32_t va, uint32_t phys, uint32_t flags){
    if(va<USER_BASE || va>=USER_TOP) return -1;
    uint32_t idx=va>>22, e=dir[idx]; uint32_t *pt;
    if(!(e&PG_P)){
        pt=page_alloc_z(); if(!pt) return -1;
        dir[idx]=(uint32_t)pt|PG_P|PG_RW|PG_US;
    } else if(e&PG_PS) return -1;
    else pt=(uint32_t*)(e&~0xFFFu);
    pt[(va>>12)&0x3FF]=(phys&~0xFFFu)|flags|PG_P;
    return 0;
}
static uint32_t *proc_dir_new(void){
    uint32_t *d=page_alloc_z(); if(!d) return 0;
    memcpy(d,page_dir,4096);
    for(uint32_t i=USER_IDX0;i<=USER_IDX1;i++) d[i]=0;
    return d;
}
static void proc_destroy(uint32_t *dir){
    for(uint32_t i=USER_IDX0;i<=USER_IDX1;i++){
        uint32_t e=dir[i];
        if((e&PG_P) && !(e&PG_PS)){
            uint32_t *pt=(uint32_t*)(e&~0xFFFu);
            for(uint32_t j=0;j<1024;j++) if(pt[j]&PG_P) page_free((void*)(pt[j]&~0xFFFu));
            page_free(pt);
        }
    }
    page_free(dir);
}
static int proc_stack(uint32_t *dir){
    for(uint32_t k=1;k<=USER_STACK_PAGES;k++){
        void *f=page_alloc_z(); if(!f) return -1;
        if(proc_map(dir,USER_TOP-4096*k,(uint32_t)f,PG_RW|PG_US)){ page_free(f); return -1; }
    }
    return 0;
}
struct elf_hdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } __attribute__((packed));
struct elf_ph  { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } __attribute__((packed));

static int elf_load(uint32_t *dir, const uint8_t *img, uint32_t size, uint32_t *entry){
    if(size<sizeof(struct elf_hdr)) return -1;
    const struct elf_hdr *h=(const struct elf_hdr*)img;
    if(h->ident[0]!=0x7F||h->ident[1]!='E'||h->ident[2]!='L'||h->ident[3]!='F'||h->ident[4]!=1||h->ident[5]!=1) return -1;
    if(h->type!=2 || h->machine!=3 || h->phentsize!=sizeof(struct elf_ph) || h->phnum==0 || h->phnum>16) return -1;
    if(h->phoff>size || (uint32_t)h->phnum*sizeof(struct elf_ph) > size-h->phoff) return -1;
    for(uint32_t i=0;i<h->phnum;i++){
        const struct elf_ph *p=(const struct elf_ph*)(img+h->phoff+i*sizeof(struct elf_ph));
        if(p->type!=1) continue;                                        /* sólo PT_LOAD */
        if(p->memsz<p->filesz || p->vaddr<USER_BASE || p->vaddr>=ELF_LIMIT || p->memsz>ELF_LIMIT-p->vaddr) return -1;
        if(p->offset>size || p->filesz>size-p->offset) return -1;
        for(uint32_t pg=p->vaddr&~0xFFFu; pg<p->vaddr+p->memsz; pg+=4096){
            if(pte_of(dir,pg)&PG_P) continue;
            void *f=page_alloc_z(); if(!f) return -1;
            if(proc_map(dir,pg,(uint32_t)f,PG_RW|PG_US)){ page_free(f); return -1; }
        }
        for(uint32_t done=0; done<p->filesz; ){
            uint32_t va=p->vaddr+done, pte=pte_of(dir,va);
            uint32_t n=4096-(va&0xFFF); if(n>p->filesz-done) n=p->filesz-done;
            memcpy((void*)((pte&~0xFFFu)+(va&0xFFF)), img+p->offset+done, n);
            done+=n;
        }
    }
    if(h->entry<USER_BASE || h->entry>=ELF_LIMIT) return -1;
    *entry=h->entry; return 0;
}

static void proc_unmap(uint32_t *dir, uint32_t va){          /* libera el marco y limpia la entrada */
    uint32_t e=dir[va>>22]; if(!(e&PG_P) || (e&PG_PS)) return;
    uint32_t *pt=(uint32_t*)(e&~0xFFFu), pte=pt[(va>>12)&0x3FF];
    if(pte&PG_P){ page_free((void*)(pte&~0xFFFu)); pt[(va>>12)&0x3FF]=0; }
}
/* brk: mueve el fin del heap de usuario; devuelve el brk resultante (el anterior si falla) */
static uint32_t proc_brk(uint32_t *dir, uint32_t cur, uint32_t want){
    if(want<HEAP_BASE || want>HEAP_LIMIT) return cur;
    uint32_t oc=(cur+0xFFFu)&~0xFFFu, nw=(want+0xFFFu)&~0xFFFu;
    if(nw>oc){
        for(uint32_t pg=oc; pg<nw; pg+=4096){
            void *f=page_alloc_z();
            if(!f || proc_map(dir,pg,(uint32_t)f,PG_RW|PG_US)){
                if(f) page_free(f);
                for(uint32_t q=oc; q<pg; q+=4096) proc_unmap(dir,q);   /* deshacer */
                return cur;
            }
        }
    } else for(uint32_t pg=nw; pg<oc; pg+=4096) proc_unmap(dir,pg);
    return want;
}
/* fork: copia profunda del espacio de usuario (tablas + marcos). 0 si no hay memoria (sin fugas). */
static uint32_t *proc_clone(const uint32_t *src){
    uint32_t *d=proc_dir_new(); if(!d) return 0;
    for(uint32_t i=USER_IDX0;i<=USER_IDX1;i++){
        uint32_t e=src[i]; if(!(e&PG_P) || (e&PG_PS)) continue;
        const uint32_t *spt=(const uint32_t*)(e&~0xFFFu);
        uint32_t *dpt=page_alloc_z(); if(!dpt){ proc_destroy(d); return 0; }
        d[i]=(uint32_t)dpt|PG_P|PG_RW|PG_US;
        for(uint32_t j=0;j<1024;j++) if(spt[j]&PG_P){
            void *f=page_alloc(); if(!f){ proc_destroy(d); return 0; }
            memcpy(f,(void*)(spt[j]&~0xFFFu),4096);
            dpt[j]=(uint32_t)f|(spt[j]&0xFFFu);
        }
    }
    return d;
}
/*HT-END*/

/* ---------- Hilos de kernel + planificador round-robin expropiativo ----------
 * Cada tick del PIT (10 ms) el ISR llama a schedule(), que guarda la pila de la tarea actual
 * y devuelve la de la siguiente; isr_common cambia ESP y el iret "retoma" esa tarea. */
#define STACK_SZ 8192
enum { T_RUN, T_SLEEP, T_DEAD };
struct task { uint32_t id; struct regs *sp; uint8_t *stack; int state; uint32_t wake, runs; char name[16]; struct task *next;
              uint32_t cr3, parent, brk_cur; int user, exit_code, killed, collected; };
static uint32_t cur_cr3;
static struct task task0 = { .id=0, .state=T_RUN, .name="shell", .next=&task0 };
static struct task *volatile current=&task0;
static uint32_t next_id=1;
static volatile int sched_on;

static struct regs *schedule(struct regs *r){
    if(!sched_on) return r;
    current->sp=r;
    struct task *t=current->next;
    for(;;){                                   /* task0 (shell) siempre es ejecutable: termina */
        if(t->state==T_SLEEP && (int32_t)(ticks-t->wake)>=0) t->state=T_RUN;
        if(t->state==T_RUN) break;
        t=t->next;
    }
    t->runs++; current=t;
    struct regs *ret=t->sp;
    if(t->user) tss.esp0=(uint32_t)t->stack+STACK_SZ;            /* pila de kernel para las interrupciones de ring 3 */
    if(t->cr3!=cur_cr3){ cur_cr3=t->cr3; __asm__ volatile("mov %0,%%cr3"::"r"(cur_cr3):"memory"); }
    return ret;
}
static void thread_exit(void){
    current->state=T_DEAD;
    for(;;) __asm__ volatile("sti; hlt");
}
static struct task *thread_create(const char *name, void (*fn)(void)){
    struct task *t=kmalloc(sizeof *t); uint8_t *stk=kmalloc(STACK_SZ);
    if(!t||!stk){ kfree(t); kfree(stk); return 0; }
    memset(t,0,sizeof *t); t->stack=stk; t->cr3=(uint32_t)page_dir;
    for(int i=0;i<15&&name[i];i++) t->name[i]=name[i];
    uint32_t top=(uint32_t)stk+STACK_SZ;
    struct regs *f=(struct regs*)(top-sizeof(struct regs));
    memset(f,0,sizeof *f);
    f->eip=(uint32_t)fn; f->cs=0x08; f->eflags=0x202;     /* IF=1 */
    f->useresp=(uint32_t)thread_exit;   /* tras el iret (sin cambio de privilegio) ESP apunta aqui: es la direccion de retorno de fn() */
    t->sp=f; t->state=T_RUN;
    uint32_t fl=irq_save();
    t->id=next_id++; t->next=task0.next; task0.next=t;
    irq_restore(fl);
    return t;
}
static void thread_sleep(uint32_t n){
    if(current==&task0){ uint32_t e=ticks+n; while((int32_t)(ticks-e)<0) __asm__ volatile("hlt"); return; }
    current->wake=ticks+n; current->state=T_SLEEP;
    while(current->state==T_SLEEP) __asm__ volatile("sti; hlt");
}
static int task_alive(uint32_t id){
    struct task *t=&task0;
    do{ if(t->id==id) return t->state!=T_DEAD; t=t->next; }while(t!=&task0);
    return 0;
}
static void reap(void){                  /* libera pilas de hilos terminados (sólo desde el shell) */
    uint32_t fl=irq_save();
    struct task *p=&task0;
    while(p->next!=&task0){
        struct task *t=p->next;
        if(t->state==T_DEAD && t!=current && (t->parent==0 || t->collected || !task_alive(t->parent))){ p->next=t->next; if(t->user) proc_destroy((uint32_t*)t->cr3); kfree(t->stack); kfree(t); }
        else p=t;
    }
    irq_restore(fl);
}


/* ---------- Procesos de usuario (ring 3), syscalls y programas incrustados ---------- */
static volatile uint32_t ubytes;
static struct task *proc_create(const char *name, const uint8_t *img, uint32_t size){
    uint32_t *dir=proc_dir_new(); if(!dir) return 0;
    uint32_t entry;
    if(elf_load(dir,img,size,&entry) || proc_stack(dir)){ proc_destroy(dir); return 0; }
    struct task *t=kmalloc(sizeof *t); uint8_t *stk=kmalloc(STACK_SZ);
    if(!t||!stk){ kfree(t); kfree(stk); proc_destroy(dir); return 0; }
    memset(t,0,sizeof *t); t->stack=stk; t->user=1; t->cr3=(uint32_t)dir; t->brk_cur=HEAP_BASE;
    for(int i=0;i<15&&name[i];i++) t->name[i]=name[i];
    uint32_t top=(uint32_t)stk+STACK_SZ;
    struct regs *f=(struct regs*)(top-sizeof(struct regs));
    memset(f,0,sizeof *f);
    f->eip=entry; f->cs=0x1B; f->ss=0x23; f->useresp=USER_TOP-16; f->eflags=0x202;
    t->sp=f; t->state=T_RUN;
    uint32_t fl=irq_save();
    t->id=next_id++; t->next=task0.next; task0.next=t;
    irq_restore(fl);
    return t;
}
extern const uint8_t blob_utest_start[], blob_utest_end[], blob_crash_start[], blob_crash_end[], blob_spin_start[], blob_spin_end[],
                     blob_ush_start[], blob_ush_end[], blob_forktest_start[], blob_forktest_end[];
static const struct prog { const char *name; const uint8_t *s, *e; } progs[]={
    {"utest",blob_utest_start,blob_utest_end}, {"crash",blob_crash_start,blob_crash_end}, {"spin",blob_spin_start,blob_spin_end},
    {"ush",blob_ush_start,blob_ush_end}, {"forktest",blob_forktest_start,blob_forktest_end} };
#define NPROGS ((int)(sizeof progs/sizeof progs[0]))
static const struct prog *find_prog(const char *name){
    for(int i=0;i<NPROGS;i++) if(streq(name,progs[i].name)) return &progs[i];
    return 0;
}
static struct task *proc_spawn(const char *name){
    const struct prog *p=find_prog(name);
    return p?proc_create(p->name,p->s,(uint32_t)(p->e-p->s)):0;
}

static int user_ok(uint32_t va, uint32_t len){          /* el rango debe estar en region de usuario y mapeado con US */
    if(!len) return 1;
    if(va<USER_BASE || va>=USER_TOP || len>USER_TOP-va) return 0;
    const uint32_t *dir=(const uint32_t*)current->cr3;
    for(uint32_t p=va&~0xFFFu; p<va+len; p+=4096)
        if((pte_of(dir,p)&(PG_P|PG_US))!=(PG_P|PG_US)) return 0;
    return 1;
}
#define E_BADF 9
#define E_AGAIN 11
#define E_FAULT 14
#define E_NOSYS 38
/* ABI: eax=numero, ebx/ecx/edx=args, retorno en eax (negativo = -errno). Numeros iguales a Linux i386. */
static struct regs *syscall_dispatch(struct regs *r){
    switch(r->eax){
    case 1:   /* exit */
        current->exit_code=(int)r->ebx; current->state=T_DEAD; return schedule(r);
    case 3: { /* read(fd=0): una tecla si hay, si no -EAGAIN */
        if(r->ebx!=0){ r->eax=(uint32_t)-E_BADF; break; }
        if(!r->edx){ r->eax=0; break; }
        if(!user_ok(r->ecx,1)){ r->eax=(uint32_t)-E_FAULT; break; }
        int c=getc_nb();
        if(c<0){ r->eip-=2; current->wake=ticks+1; current->state=T_SLEEP; return schedule(r); }   /* bloquea: reintenta la syscall */
        *(char*)r->ecx=(char)c; r->eax=1; break; }
    case 4: { /* write(fd 1|2) */
        if(r->ebx!=1 && r->ebx!=2){ r->eax=(uint32_t)-E_BADF; break; }
        uint32_t n=r->edx; if(n>4096) n=4096;
        if(!user_ok(r->ecx,n)){ r->eax=(uint32_t)-E_FAULT; break; }
        const char *b=(const char*)r->ecx;
        for(uint32_t i=0;i<n;i++) putc(b[i]);
        ubytes+=n; r->eax=n; break; }
    case 20: r->eax=current->id; break;                   /* getpid */
    case 158: r->eax=0; return schedule(r);               /* sched_yield */
    case 162: { /* nanosleep(const struct timespec32 *req, rem) */
        if(!user_ok(r->ebx,8)){ r->eax=(uint32_t)-E_FAULT; break; }
        const int32_t *ts=(const int32_t*)r->ebx;
        int32_t sec=ts[0], ns=ts[1];
        if(sec<0||ns<0||ns>=1000000000){ r->eax=(uint32_t)-22; break; }
        if(sec>100000) sec=100000;
        uint32_t n=(uint32_t)sec*100+(uint32_t)ns/10000000u;
        r->eax=0;
        if(!n) return schedule(r);
        current->wake=ticks+n; current->state=T_SLEEP; return schedule(r); }
    case 2: { /* fork: copia del espacio de usuario; el hijo ve eax=0 */
        uint32_t *nd=proc_clone((const uint32_t*)current->cr3);
        if(!nd){ r->eax=(uint32_t)-12; break; }
        struct task *t=kmalloc(sizeof *t); uint8_t *stk=kmalloc(STACK_SZ);
        if(!t||!stk){ kfree(t); kfree(stk); proc_destroy(nd); r->eax=(uint32_t)-12; break; }
        memset(t,0,sizeof *t); t->stack=stk; t->user=1; t->cr3=(uint32_t)nd;
        t->parent=current->id; t->brk_cur=current->brk_cur;
        for(int i=0;i<15;i++) t->name[i]=current->name[i];
        struct regs *f=(struct regs*)((uint32_t)stk+STACK_SZ-sizeof(struct regs));
        memcpy(f,r,sizeof *f); f->eax=0;
        t->sp=f; t->state=T_RUN; t->id=next_id++; t->next=task0.next; task0.next=t;
        r->eax=t->id; break; }
    case 7: { /* waitpid(pid|-1, *status, opts): bloquea hasta que un hijo termine */
        int want=(int)r->ebx, kids=0; struct task *t=&task0, *dead=0;
        if(r->ecx && !user_ok(r->ecx,4)){ r->eax=(uint32_t)-E_FAULT; break; }
        do{ if(t->parent==current->id && !t->collected && (want==-1||(int)t->id==want)){
                kids++; if(t->state==T_DEAD && !dead) dead=t; }
            t=t->next; }while(t!=&task0);
        if(dead){
            if(r->ecx) *(uint32_t*)r->ecx = dead->killed ? 11u : (((uint32_t)dead->exit_code&0xFFu)<<8);
            dead->collected=1; r->eax=dead->id; break;
        }
        if(!kids){ r->eax=(uint32_t)-10; break; }                 /* ECHILD */
        r->eip-=2; current->wake=ticks+1; current->state=T_SLEEP; return schedule(r); }
    case 11: { /* exec(nombre): reemplaza el espacio de usuario por un programa incrustado */
        char nm[32]; int i=0;
        for(;i<31;i++){ if(!user_ok(r->ebx+i,1)) break; nm[i]=*(const char*)(r->ebx+i); if(!nm[i]) break; }
        if(i==0 || i>=31 || nm[i]){ r->eax=(uint32_t)-E_FAULT; break; }
        const struct prog *p=find_prog(nm);
        if(!p){ r->eax=(uint32_t)-2; break; }                     /* ENOENT */
        uint32_t *nd=proc_dir_new(), entry;
        if(!nd){ r->eax=(uint32_t)-12; break; }
        if(elf_load(nd,p->s,(uint32_t)(p->e-p->s),&entry) || proc_stack(nd)){ proc_destroy(nd); r->eax=(uint32_t)-8; break; }
        uint32_t *old=(uint32_t*)current->cr3;
        current->cr3=cur_cr3=(uint32_t)nd; __asm__ volatile("mov %0,%%cr3"::"r"(cur_cr3):"memory");
        proc_destroy(old);
        current->brk_cur=HEAP_BASE;
        for(int k=0;k<15;k++) current->name[k]=(k<i)?nm[k]:0;
        r->edi=r->esi=r->ebp=r->ebx=r->edx=r->ecx=r->eax=0;
        r->eip=entry; r->useresp=USER_TOP-16; r->eflags=0x202;
        return r; }
    case 45: { /* brk */
        uint32_t cur=current->brk_cur;
        if(r->ebx){ current->brk_cur=proc_brk((uint32_t*)current->cr3,cur,r->ebx);
                    __asm__ volatile("mov %0,%%cr3"::"r"(cur_cr3):"memory"); }   /* recarga CR3: invalida el TLB */
        r->eax=current->brk_cur; break; }
    case 64: { /* getppid */
        r->eax=current->parent; break; }
    case 500: r->eax=ticks; break;                        /* extension propia: ticks del timer */
    default: r->eax=(uint32_t)-E_NOSYS;
    }
    return r;
}
static struct regs *user_fault(struct regs *r, uint32_t cr2){
    kprintf("[kng] proceso %u (%s) terminado: excepcion %u (%s) eip=0x%x",current->id,current->name,r->int_no,
            r->int_no<15?exc_names[r->int_no]:"reservada",r->eip);
    if(r->int_no==14) kprintf(" cr2=0x%x err=0x%x",cr2,r->err);
    kprintf("\n");
    current->killed=1; current->exit_code=-1; current->state=T_DEAD;
    return schedule(r);
}
static void cmd_sh(void){
    struct task *t=proc_spawn("ush");
    if(!t){ kprintf("no se pudo iniciar ush\n"); return; }
    while(t->state!=T_DEAD) __asm__ volatile("hlt");
    kprintf("[kng] ush termino (codigo %d)\n",t->exit_code);
}
static void cmd_run(const char *name){
    struct task *t=proc_spawn(name);
    if(!t){ kprintf("no se pudo iniciar '%s' (usa 'progs')\n",name); return; }
    uint32_t e=ticks+1000;
    while(t->state!=T_DEAD && (int32_t)(ticks-e)<0) __asm__ volatile("hlt");
    if(t->state==T_DEAD) kprintf("[kng] '%s' termino: codigo %d%s\n",name,t->exit_code,t->killed?" (matado por excepcion)":"");
    else kprintf("[kng] '%s' sigue ejecutandose (id %u). Usa 'kill %u'.\n",name,t->id,t->id);
}

static volatile uint32_t busy_n, st_a, st_b;
static void w_count(void){ for(int i=1;i<=5;i++){ kprintf("[%s] paso %d/5\n",current->name,i); thread_sleep(100); } }
static void w_busy(void){ for(;;) busy_n++; }
static void w_a(void){ for(int i=0;i<10;i++){ st_a++; thread_sleep(5); } }
static void w_b(void){ for(int i=0;i<10;i++){ st_b++; thread_sleep(5); } }

/* ---------- Autopruebas (make test las ejecuta en QEMU) ---------- */
static int st_fail;
static void check(const char *n, int ok){ kprintf("[%s] %s\n",ok?"PASS":"FAIL",n); if(!ok) st_fail++; }
static void selftest(void){
    st_fail=0;
    uint32_t u0,f0,l0,u1,f1,l1; heap_stats(&u0,&f0,&l0);
    { uint32_t n0=pages_alloc; void *p1=page_alloc(),*p2=page_alloc();
      check("pmm: dos paginas distintas y alineadas", p1&&p2&&p1!=p2&&!((uint32_t)p1&0xFFF)&&!((uint32_t)p2&0xFFF));
      page_free(p1); void *p3=page_alloc();
      check("pmm: reutiliza la pagina liberada", p3==p1);
      page_free(p3); page_free(p2); page_free(p2);          /* el doble free se ignora */
      check("pmm: contador vuelve al inicial", pages_alloc==n0); }
    void *a=kmalloc(100),*b=kmalloc(200),*c=kmalloc(300);
    check("heap: tres bloques distintos", a&&b&&c&&a!=b&&b!=c);
    memset(a,0xAA,100); memset(b,0xBB,200); memset(c,0xCC,300);
    check("heap: sin solapamiento", ((uint8_t*)a)[99]==0xAA && ((uint8_t*)b)[0]==0xBB && ((uint8_t*)c)[0]==0xCC);
    kfree(b); void *d=kmalloc(150);
    check("heap: reutiliza el hueco liberado", d==b);
    kfree(a); kfree(c); kfree(d);
    heap_stats(&u1,&f1,&l1);
    check("heap: fusion de bloques libres", u1==u0 && l1==l0);

    uint32_t va=0xD0000000u; void *pg=page_alloc();
    check("vmm: pagina fisica obtenida", pg!=0);
    if(pg){
        check("vmm: map", vmm_map(va,(uint32_t)pg,PG_RW)==0 && vmm_phys(va)==(uint32_t)pg);
        *(volatile uint32_t*)va=0xC0FFEE;
        check("vmm: escritura virtual visible en fisica", *(volatile uint32_t*)pg==0xC0FFEE);
        vmm_unmap(va);
        check("vmm: unmap", vmm_phys(va)==0);
        page_free(pg);
    }

    st_a=st_b=0; busy_n=0;
    struct task *ta=thread_create("t-a",w_a),*tb=thread_create("t-b",w_b),*tc=thread_create("busy",w_busy);
    check("sched: hilos creados", ta&&tb&&tc);
    uint32_t e=ticks+30; while((int32_t)(ticks-e)<0) ;   /* el shell gira sin hlt: sólo el timer puede quitarle la CPU */
    check("sched: expropiacion (busy corrio mientras el shell giraba)", busy_n>0);
    e=ticks+300; while(!(st_a==10&&st_b==10) && (int32_t)(ticks-e)<0) __asm__ volatile("hlt");
    check("sched: hilos con sleep completan 10 pasos", st_a==10&&st_b==10);
    if(tc) tc->state=T_DEAD;
    e=ticks+20; while((int32_t)(ticks-e)<0) __asm__ volatile("hlt");
    reap(); heap_stats(&u1,&f1,&l1);
    check("sched: hilos liberados sin fugas", u1==u0);

    /* ---- procesos de usuario (ring 3) ---- */
    { uint32_t pu=pages_alloc, hu,fu,lu; heap_stats(&hu,&fu,&lu);
      ubytes=0;
      struct task *ut=proc_spawn("utest");
      check("user: proceso utest creado", ut!=0);
      if(ut){
          e=ticks+500; while(ut->state!=T_DEAD && (int32_t)(ticks-e)<0) __asm__ volatile("hlt");
          check("user: utest en ring 3 termino con codigo 42", ut->state==T_DEAD && ut->exit_code==42 && !ut->killed);
          check("user: write() desde ring 3 produjo salida", ubytes>0);
      }
      struct task *cr=proc_spawn("crash");
      check("user: proceso crash creado", cr!=0);
      if(cr){
          e=ticks+300; while(cr->state!=T_DEAD && (int32_t)(ticks-e)<0) __asm__ volatile("hlt");
          check("user: escritura a memoria del kernel mata solo al proceso", cr->state==T_DEAD && cr->killed);
      }
      struct task *ft=proc_spawn("forktest");
      check("user: proceso forktest creado", ft!=0);
      if(ft){
          e=ticks+800; while(ft->state!=T_DEAD && (int32_t)(ticks-e)<0) __asm__ volatile("hlt");
          check("user: fork + exec + waitpid + brk correctos (forktest)", ft->state==T_DEAD && ft->exit_code==0 && !ft->killed);
      }
      struct task *sp=proc_spawn("spin");
      check("user: proceso spin creado", sp!=0);
      if(sp){
          e=ticks+50; while((int32_t)(ticks-e)<0) ;          /* el shell gira: spin (ring 3) tiene que ser expropiado y corrido */
          check("user: planificador alterna con proceso ring 3", sp->runs>=3);
          sp->state=T_DEAD;
      }
      e=ticks+20; while((int32_t)(ticks-e)<0) __asm__ volatile("hlt");
      reap(); heap_stats(&u1,&f1,&l1);
      check("user: sin fugas de heap ni de paginas fisicas", u1==hu && pages_alloc==pu);
    }
    if(st_fail) kprintf("SELFTEST FAIL (%d)\n",st_fail); else kprintf("SELFTEST OK\n");
}

/* ---------- Shell ---------- */
static void cmd_info(void){
    kprintf("kng-kernel %s (%s, Multiboot)\n", KNG_VERSION, KNG_ARCH);
    kprintf("Parte de Konigreich OS. PMM, paginacion, heap, hilos, procesos ring 3, ELF, syscalls, red.\n");
}
static void run(char *l){
    while(*l==' ') l++;
    if(!*l) return;
    if(streq(l,"help")) kprintf("help info mem heap ps spawn [busy] kill <id> progs run <prog> sh selftest fault uptime alloc clear echo <txt> reboot halt | red ping <host> dns <host> wget <url>\n");
    else if(streq(l,"info")) cmd_info();
    else if(streq(l,"mem")) kprintf("RAM usable: %u KB | PMM: %u paginas usadas de %u (%u KB libres)\n",
            ram_usable_kb,pages_alloc,pmm_pages,(pmm_pages-pages_alloc)*4);
    else if(streq(l,"heap")){ uint32_t u,f,m; heap_stats(&u,&f,&m);
        kprintf("heap: total %u KB | usado %u B | libre %u B | bloque libre mayor %u B\n",heap_total/1024,u,f,m); }
    else if(streq(l,"ps")){ static const char *sn[]={"RUN","SLEEP","DEAD"}; struct task *t=&task0;
        kprintf("ID  NOMBRE  ESTADO  VECES  MODO\n");
        do{ kprintf("%u   %s  %s   %u   %s\n",t->id,t->name,sn[t->state],t->runs,t->user?"ring3":"kernel"); t=t->next; }while(t!=&task0); }
    else if(streq(l,"spawn")){ if(!thread_create("contador",w_count)) kprintf("sin memoria\n"); }
    else if(streq(l,"spawn busy")){ if(!thread_create("busy",w_busy)) kprintf("sin memoria\n"); }
    else if(startswith(l,"kill ")){ int id=atoi_(l+5); struct task *t=&task0; int ok=0;
        if(id>0) do{ if((int)t->id==id){ t->state=T_DEAD; ok=1; } t=t->next; }while(t!=&task0);
        kprintf(ok?"hilo %d terminado\n":"no existe (o es el shell): %d\n",id); }
    else if(streq(l,"progs")){ for(int i=0;i<NPROGS;i++) kprintf("%s (%u bytes)\n",progs[i].name,(uint32_t)(progs[i].e-progs[i].s)); }
    else if(startswith(l,"run ")) cmd_run(l+4);
    else if(streq(l,"sh")) cmd_sh();
    else if(streq(l,"selftest")) selftest();
    else if(streq(l,"fault")){ vmm_unmap(0xD0000000u); kprintf("tocando pagina no mapeada...\n"); *(volatile uint32_t*)0xD0000000u=1; }
    else if(streq(l,"uptime")) kprintf("%u.%u s\n",ticks/100,(ticks%100)/10);
    else if(streq(l,"alloc")){ void*p=page_alloc(); if(p) kprintf("pagina 4K en 0x%x\n",(uint32_t)p); else kprintf("sin memoria\n"); }
    else if(streq(l,"clear")) clear();
    else if(startswith(l,"echo ")) kprintf("%s\n",l+5);
    else if(streq(l,"reboot")){ outb(0x64,0xFE); for(;;) __asm__ volatile("hlt"); }
    else if(streq(l,"halt")){ kprintf("Sistema detenido.\n"); for(;;) __asm__ volatile("cli;hlt"); }
    else kprintf("comando desconocido: %s (prueba 'help')\n", l);
}

/* ---------- API para la interfaz grafica (kng.h) ---------- */
uint32_t kticks(void){ return ticks; }
int kgetc(void){ return getc_nb(); }
void kshell_exec(char *l){ if(net_cmd(l)) return; run(l); }
uint32_t kram_kb(void){ return ram_usable_kb; }
static inline void outw(uint16_t p,uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
void kreboot(void){ outb(0x64,0xFE); for(;;) __asm__ volatile("hlt"); }
void khalt(void){
    kprintf("Apagando...\n");
    outw(0x604,0x2000); outw(0xB004,0x2000); outw(0x4004,0x3400);   /* apagado por ACPI en QEMU, Bochs y VirtualBox */
    for(;;) __asm__ volatile("cli;hlt");
}
static uint8_t cmos(uint8_t r){ outb(0x70,r); return inb(0x71); }
int krtc(int *h,int *m){
    int t=0; while((cmos(0x0A)&0x80) && ++t<10000);
    uint8_t b=cmos(0x0B), hh=cmos(4), mm=cmos(2); int pm=hh&0x80; hh&=0x7F;
    if(!(b&4)){ hh=(uint8_t)((hh&0x0F)+(hh>>4)*10); mm=(uint8_t)((mm&0x0F)+(mm>>4)*10); }
    if(!(b&2)){ hh%=12; if(pm) hh+=12; }
    *h=hh%24; *m=mm%60; return 0;
}

void kmain(uint32_t magic, struct mb_info *mi){
    serial_init(); clear();
    uint32_t fb_addr=0,fb_pitch=0,fb_w=0,fb_h=0,fb_ok=0;                  /* se copia ya: la estructura Multiboot puede ser pisada despues */
    if(magic==0x2BADB002 && (mi->flags&(1u<<12)) && mi->fb_type==1 && mi->fb_bpp==32 && mi->fb_hi==0 && mi->fb_w>=640 && mi->fb_h>=480){
        fb_addr=mi->fb_lo; fb_pitch=mi->fb_pitch; fb_w=mi->fb_w; fb_h=mi->fb_h; fb_ok=1; }
    kprintf("== kng-kernel %s :: Konigreich OS ==\n", KNG_VERSION);
    if(magic!=0x2BADB002){ kprintf("Arranque no Multiboot (magic=0x%x). Detenido.\n",magic); for(;;) __asm__ volatile("hlt"); }
    gdt_init(); idt_init(); pic_init(); pit_init(); irq_handlers[1]=kb_irq;
    mouse_init();
    mem_init(mi);
    vmm_init(); heap_init(64u<<20); pmm_init(); task0.cr3=cur_cr3=(uint32_t)page_dir; sched_on=1;
    __asm__ volatile("sti");
    kprintf("[ok] GDT, IDT, PIC, PIT(100Hz), teclado y raton PS/2\n");
    if(net_init()==0){ char nb[90]; net_info(nb,sizeof nb); kprintf("[ok] %s\n",nb); } else kprintf("[i] sin tarjeta de red compatible (e1000 / RTL8139)\n");
    kprintf("[ok] RAM utilizable: %u KB\n", ram_usable_kb);
    kprintf("[ok] paginacion, heap %u KB, PMM %u paginas, hilos, ring 3 (TSS), syscalls int 0x80\n", heap_total/1024, pmm_pages);
    if(fb_ok){
        kprintf("[ok] framebuffer %ux%u 32 bpp en 0x%x. Iniciando escritorio...\n",fb_w,fb_h,fb_addr);
        fb_wc(fb_addr,fb_pitch*fb_h);
        if(gui_init((uint32_t*)fb_addr,(int)fb_w,(int)fb_h,(int)fb_pitch)==0){
            gui_mode=1;
            for(;;){
                int c; while((c=getc_nb())>=0) gui_key(c);
                struct mev e; while(mouse_pop(&e)) gui_mouse_event(e.dx,e.dy,e.b);
                net_poll(); gui_frame(); reap();
                __asm__ volatile("hlt");                   /* despierta con el timer (100 Hz) o cualquier IRQ */
            }
        }
        kprintf("[!] no se pudo iniciar la interfaz grafica (memoria). Modo texto.\n");
    } else kprintf("[i] sin framebuffer (arranque en modo texto).\n");
    kprintf("Escribe 'help'.\n");
    char line[128]; int n=0;
    kprintf("kng> ");
    for(;;){
        int c=getc_nb();
        if(c<0){ net_poll(); reap(); __asm__ volatile("hlt"); continue; }
        if(c=='\n'){ putc('\n'); line[n]=0; run(line); n=0; kprintf("kng> "); }
        else if(c=='\b'){ if(n>0){ n--; putc('\b'); } }
        else if(c>=32 && n<127){ line[n++]=(char)c; putc((char)c); }
    }
}
