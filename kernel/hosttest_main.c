static uint8_t *slurp(const char*f,uint32_t*n){ FILE*fp=fopen(f,"rb"); if(!fp){perror(f);exit(2);} fseek(fp,0,SEEK_END); *n=ftell(fp); rewind(fp); uint8_t*b=malloc(*n); if(fread(b,1,*n,fp)!=*n) exit(2); fclose(fp); return b; }
static int fails;
#define CK(c) do{ if(!(c)){ printf("FALLO: %s (linea %d)\n",#c,__LINE__); fails++; } }while(0)
int main(void){
  void*m=mmap(0,8<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
  if(m==MAP_FAILED){perror("mmap");return 2;}
  pmm_next=((uint32_t)(uintptr_t)m+0xFFF)&~0xFFFu; pmm_end=(uint32_t)(uintptr_t)m+(8<<20);
  pmm_init();
  uint32_t total=pmm_pages;
  /* directorio del kernel simulado: identidad con PS */
  for(uint32_t i=0;i<1024;i++) page_dir[i]=(i<<22)|PG_P|PG_RW|PG_PS;
  /* PMM: agotar y liberar */
  void**all=malloc(sizeof(void*)*total); uint32_t n=0; void*p; while((p=page_alloc())) all[n++]=p;
  CK(n==total); CK(pages_alloc==total);
  for(uint32_t i=0;i<n;i++) page_free(all[i]);
  CK(pages_alloc==0);
  page_free(all[0]); page_free((void*)1); CK(pages_alloc==0);   /* doble free / basura ignorados */
  /* cargar los ELF reales */
  const char*progs[]={"user/utest.elf","user/crash.elf","user/spin.elf"};
  for(int k=0;k<3;k++){
    uint32_t sz; uint8_t*img=slurp(progs[k],&sz);
    uint32_t before=pages_alloc;
    uint32_t*dir=proc_dir_new(); CK(dir!=0);
    CK(dir[0]==page_dir[0] && dir[100]==page_dir[100]);              /* kernel compartido */
    CK(dir[USER_IDX0]==0 && dir[USER_IDX1]==0);
    uint32_t entry=0; CK(elf_load(dir,img,sz,&entry)==0); CK(proc_stack(dir)==0);
    const struct elf_hdr*h=(const struct elf_hdr*)img; CK(entry==h->entry);
    /* el contenido de cada PT_LOAD esta en memoria fisica mapeada con US|RW */
    const struct elf_ph*ph=(const struct elf_ph*)(img+h->phoff);
    for(int i=0;i<h->phnum;i++) if(ph[i].type==1){
      for(uint32_t o=0;o<ph[i].filesz;o++){
        uint32_t va=ph[i].vaddr+o, pte=pte_of(dir,va);
        CK((pte&(PG_P|PG_US|PG_RW))==(PG_P|PG_US|PG_RW));
        if(*(uint8_t*)((pte&~0xFFFu)+(va&0xFFF))!=img[ph[i].offset+o]){ CK(0); break; }
      }
      for(uint32_t o=ph[i].filesz;o<ph[i].memsz;o++){ uint32_t va=ph[i].vaddr+o,pte=pte_of(dir,va); if(*(uint8_t*)((pte&~0xFFFu)+(va&0xFFF))){CK(0);break;} }
    }
    for(uint32_t q=1;q<=USER_STACK_PAGES;q++) CK((pte_of(dir,USER_TOP-4096*q)&(PG_P|PG_US))==(PG_P|PG_US));
    CK(pte_of(dir,USER_TOP)==0 && pte_of(dir,0x1000)==0);            /* fuera de la region: sin mapeo de usuario */
    uint32_t used=pages_alloc-before;
    proc_destroy(dir);
    CK(pages_alloc==before);
    printf("%s: %u bytes, entry=0x%x, %u paginas usadas por el proceso, liberadas OK\n",progs[k],sz,entry,used);
    free(img);
  }
  /* ELF corruptos / maliciosos deben rechazarse sin fugas */
  { uint32_t sz; uint8_t*img=slurp("user/utest.elf",&sz); uint32_t before=pages_alloc; uint32_t e;
    struct { const char*n; int off; uint32_t val; int len; } mut[]={
      {"magic",0,0,1},{"clase64",4,2,1},{"tipo=REL",16,1,2},{"maquina",18,0x3E,2},{"phnum=0",44,0,2},{"phnum=99",44,99,2},
      {"phoff enorme",28,0xFFFFFFF0u,4},{"entry=0",24,0,4}};
    for(unsigned i=0;i<sizeof mut/sizeof*mut;i++){
      uint8_t*c=malloc(sz); memcpy(c,img,sz); memcpy(c+mut[i].off,&mut[i].val,mut[i].len);
      uint32_t*dir=proc_dir_new(); int r=elf_load(dir,c,sz,&e); CK(r!=0); proc_destroy(dir); CK(pages_alloc==before);
      printf("ELF %-13s -> %s\n",mut[i].n,r?"rechazado":"ACEPTADO (mal)"); free(c);
    }
    /* segmento PT_LOAD con vaddr en el kernel / filesz fuera del archivo */
    const struct elf_hdr*h=(const struct elf_hdr*)img; uint32_t phoff=h->phoff;
    struct { const char*n; int off; uint32_t val; } pm[]={ {"vaddr=0x1000",8,0x1000},{"vaddr=0xFFFFF000",8,0xFFFFF000u},{"filesz enorme",16,0x7FFFFFFF},{"offset enorme",4,0xFFFFFFF0u},{"memsz<filesz",20,0} };
    for(unsigned i=0;i<sizeof pm/sizeof*pm;i++){
      uint8_t*c=malloc(sz); memcpy(c,img,sz); memcpy(c+phoff+pm[i].off,&pm[i].val,4);
      uint32_t*dir=proc_dir_new(); int r=elf_load(dir,c,sz,&e); CK(r!=0); proc_destroy(dir); CK(pages_alloc==before);
      printf("PT_LOAD %-17s -> %s\n",pm[i].n,r?"rechazado":"ACEPTADO (mal)"); free(c);
    }
    /* truncado */
    for(uint32_t cut=0;cut<sz;cut+=37){ uint32_t*dir=proc_dir_new(); int r=elf_load(dir,img,cut,&e); (void)r; proc_destroy(dir); CK(pages_alloc==before); }
    puts("truncados: sin fugas ni caidas");
  }
  /* agotamiento de memoria durante la carga: sin fugas */
  { uint32_t sz; uint8_t*img=slurp("user/utest.elf",&sz); uint32_t before=pages_alloc;
    void*hold[16384]; uint32_t nh=0; void*q; while(nh<16384&&(q=page_alloc())) hold[nh++]=q;
    for(uint32_t keep=0;keep<8;keep++){                                 /* liberar 'keep' paginas y probar */
      for(uint32_t i=0;i<keep&&nh;i++) page_free(hold[--nh]);
      uint32_t*dir=proc_dir_new(); uint32_t e=0; int r=-1;
      if(dir){ r=elf_load(dir,img,sz,&e); if(!r) r=proc_stack(dir); proc_destroy(dir); }
      printf("memoria casi agotada (+%u paginas libres) -> %s\n",keep,r?"fallo limpio":"ok");
    }
    while(nh) page_free(hold[--nh]);
    CK(pages_alloc==before);
  }
  /* fork (clone) y brk */
  { uint32_t sz; uint8_t*img=slurp("user/utest.elf",&sz); uint32_t before=pages_alloc, e;
    uint32_t*a=proc_dir_new(); CK(elf_load(a,img,sz,&e)==0 && proc_stack(a)==0);
    uint32_t pa=pte_of(a,USER_BASE)&~0xFFFu; memset((void*)(uintptr_t)pa,0x5A,64);
    uint32_t*b=proc_clone(a); CK(b!=0);
    uint32_t pb=pte_of(b,USER_BASE)&~0xFFFu;
    CK(pb && pb!=pa && memcmp((void*)(uintptr_t)pa,(void*)(uintptr_t)pb,4096)==0);
    memset((void*)(uintptr_t)pb,0x11,64); CK(*(uint8_t*)(uintptr_t)pa==0x5A);       /* memoria aislada */
    CK((pte_of(b,USER_TOP-4096)&(PG_P|PG_US))==(PG_P|PG_US));
    CK(b[0]==page_dir[0]);
    proc_destroy(b); proc_destroy(a); CK(pages_alloc==before);
    puts("clone: copia profunda, aislada y sin fugas");
    /* clone con memoria agotada */
    a=proc_dir_new(); elf_load(a,img,sz,&e); proc_stack(a); uint32_t mid=pages_alloc;
    void*hold[16384]; uint32_t nh=0; void*q; while(nh<16384&&(q=page_alloc())) hold[nh++]=q;
    for(uint32_t keep=0;keep<10;keep++){
      for(uint32_t i=0;i<keep&&nh;i++) page_free(hold[--nh]);
      uint32_t before2=pages_alloc; uint32_t*c=proc_clone(a);
      if(c){ proc_destroy(c); }
      CK(pages_alloc==before2);                                                      /* un fallo no deja fugas */
    }
    while(nh) page_free(hold[--nh]);
    CK(pages_alloc==mid); proc_destroy(a); CK(pages_alloc==before);
    puts("clone con memoria agotada: sin fugas");
    /* brk */
    uint32_t*d=proc_dir_new(); uint32_t p0=pages_alloc, cur=HEAP_BASE;
    cur=proc_brk(d,cur,HEAP_BASE+3*4096+10); CK(cur==HEAP_BASE+3*4096+10);
    for(int i=0;i<4;i++) CK((pte_of(d,HEAP_BASE+4096*i)&(PG_P|PG_US|PG_RW))==(PG_P|PG_US|PG_RW));
    CK(pte_of(d,HEAP_BASE+4096*4)==0);
    uint32_t pg=pte_of(d,HEAP_BASE)&~0xFFFu; CK(*(uint8_t*)(uintptr_t)pg==0);        /* paginas nuevas en cero */
    cur=proc_brk(d,cur,HEAP_BASE+4096); CK(cur==HEAP_BASE+4096);
    CK(pte_of(d,HEAP_BASE)&PG_P); CK(pte_of(d,HEAP_BASE+4096)==0);
    CK(proc_brk(d,cur,HEAP_LIMIT+4096)==cur && proc_brk(d,cur,HEAP_BASE-4096)==cur); /* fuera de limites */
    uint32_t nh2=0; while(nh2<16384&&(q=page_alloc())) hold[nh2++]=q;
    for(int i=0;i<2;i++) page_free(hold[--nh2]);
    uint32_t pc=pages_alloc; CK(proc_brk(d,cur,cur+40*4096)==cur); CK(pages_alloc==pc);   /* fallo: rollback */
    while(nh2) page_free(hold[--nh2]);
    proc_brk(d,cur,HEAP_BASE); proc_destroy(d); CK(pages_alloc==p0-0+0 || pages_alloc==before);
    puts("brk: crece, encoge, respeta limites y hace rollback");
    free(img);
  }
  printf("%s\n",fails?"HOST TEST: FALLOS":"HOST TEST OK"); return fails?1:0; }
