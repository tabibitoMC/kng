/* zip.c — archivos .zip de verdad, sin bibliotecas: deflate (LZ77 + Huffman fijo) para comprimir,
 * inflate completo (bloques guardados, Huffman fijo y dinamico) para descomprimir. CRC-32 estandar. */
#include "zip.h"
#include "kng.h"

/* ---------- utilidades ---------- */
static uint32_t crc_tab[256]; static int crc_ok;
static uint32_t crc32(const uint8_t *p,int n){
    if(!crc_ok){ for(uint32_t i=0;i<256;i++){ uint32_t c=i; for(int k=0;k<8;k++) c=(c&1)?0xEDB88320u^(c>>1):c>>1; crc_tab[i]=c; } crc_ok=1; }
    uint32_t c=0xFFFFFFFFu; for(int i=0;i<n;i++) c=crc_tab[(c^p[i])&0xFF]^(c>>8); return c^0xFFFFFFFFu;
}
typedef struct { uint8_t *d; int n, cap, bad; } Buf;
static void b_need(Buf *b,int more){
    if(b->bad||b->n+more<=b->cap) return;
    int nc=b->cap?b->cap*2:65536; while(nc<b->n+more) nc*=2;
    uint8_t *x=kmalloc((size_t)nc); if(!x){ b->bad=1; return; }
    if(b->n) memcpy(x,b->d,(size_t)b->n);
    if(b->d) kfree(b->d);
    b->d=x; b->cap=nc;
}
static void b_put(Buf *b,const void *s,int n){ b_need(b,n); if(b->bad) return; memcpy(b->d+b->n,s,(size_t)n); b->n+=n; }
static void b_u16(Buf *b,uint32_t v){ uint8_t t[2]={(uint8_t)v,(uint8_t)(v>>8)}; b_put(b,t,2); }
static void b_u32(Buf *b,uint32_t v){ uint8_t t[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; b_put(b,t,4); }
static uint32_t rd16(const uint8_t *p){ return (uint32_t)p[0]|(uint32_t)p[1]<<8; }
static uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }

static const uint16_t LBASE[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  LEXT[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t DBASE[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  DEXT[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

/* ---------- deflate (compresor) ---------- */
typedef struct { Buf *o; uint32_t bits; int nb; } BW;
static void bw_put(BW *w,uint32_t v,int n){                       /* bits en orden LSB primero */
    w->bits|=v<<w->nb; w->nb+=n;
    while(w->nb>=8){ uint8_t c=(uint8_t)w->bits; b_put(w->o,&c,1); w->bits>>=8; w->nb-=8; }
}
static void bw_huff(BW *w,uint32_t code,int n){ uint32_t r=0; for(int i=0;i<n;i++) r|=((code>>i)&1)<<(n-1-i); bw_put(w,r,n); }   /* los codigos Huffman van con el bit alto primero */
static void put_lit(BW *w,int v){
    if(v<144) bw_huff(w,0x30+v,8); else if(v<256) bw_huff(w,0x190+(v-144),9);
    else if(v<280) bw_huff(w,v-256,7); else bw_huff(w,0xC0+(v-280),8);
}
static void put_match(BW *w,int len,int dist){
    int i=28; while(LBASE[i]>len) i--;
    put_lit(w,257+i); if(LEXT[i]) bw_put(w,(uint32_t)(len-LBASE[i]),LEXT[i]);
    int j=29; while(DBASE[j]>dist) j--;
    bw_huff(w,(uint32_t)j,5); if(DEXT[j]) bw_put(w,(uint32_t)(dist-DBASE[j]),DEXT[j]);
}
#define HBITS 15
#define HSIZE (1<<HBITS)
#define WSIZE 32768
static int deflate_fixed(const uint8_t *in,int n,Buf *o){
    int *head=kmalloc(HSIZE*sizeof(int)), *prev=kmalloc(WSIZE*sizeof(int));
    if(!head||!prev){ if(head) kfree(head); if(prev) kfree(prev); return -1; }
    for(int i=0;i<HSIZE;i++) head[i]=-1;
    BW w={o,0,0}; bw_put(&w,1,1); bw_put(&w,1,2);                 /* un solo bloque final, Huffman fijo */
    int i=0;
    while(i<n){
        int best=0, bd=0;
        if(i+3<=n){
            uint32_t h=((uint32_t)in[i]<<10^(uint32_t)in[i+1]<<5^in[i+2])&(HSIZE-1);
            int c=head[h], tries=48, lim=i-WSIZE+1;
            while(c>=0&&c>=lim&&tries--){
                if(in[c+best]==in[i+best]){ int l=0, mx=n-i; if(mx>258) mx=258; while(l<mx&&in[c+l]==in[i+l]) l++; if(l>best){ best=l; bd=i-c; if(l==mx) break; } }
                c=prev[c&(WSIZE-1)];
            }
            prev[i&(WSIZE-1)]=head[h]; head[h]=i;
        }
        if(best>=3){
            put_match(&w,best,bd);
            for(int k=1;k<best;k++){ int p=i+k; if(p+3<=n){ uint32_t h=((uint32_t)in[p]<<10^(uint32_t)in[p+1]<<5^in[p+2])&(HSIZE-1); prev[p&(WSIZE-1)]=head[h]; head[h]=p; } }
            i+=best;
        } else { put_lit(&w,in[i]); i++; }
    }
    put_lit(&w,256); if(w.nb) bw_put(&w,0,8-w.nb);
    kfree(head); kfree(prev);
    return o->bad?-1:0;
}

/* ---------- inflate (descompresor) ---------- */
typedef struct { const uint8_t *p; int n, pos; uint32_t bits; int nb, err; } BR;
static int br_get(BR *r,int n){
    while(r->nb<n){ if(r->pos>=r->n){ r->err=1; return 0; } r->bits|=(uint32_t)r->p[r->pos++]<<r->nb; r->nb+=8; }
    int v=(int)(r->bits&((1u<<n)-1)); r->bits>>=n; r->nb-=n; return v;
}
typedef struct { uint16_t cnt[16], sym[320]; } Huff;
static void h_build(Huff *h,const uint8_t *len,int n){
    uint16_t off[16]; memset(h->cnt,0,sizeof h->cnt);
    for(int i=0;i<n;i++) h->cnt[len[i]]++;
    h->cnt[0]=0; off[1]=0; for(int i=1;i<15;i++) off[i+1]=off[i]+h->cnt[i];
    for(int i=0;i<n;i++) if(len[i]) h->sym[off[len[i]]++]=(uint16_t)i;
}
static int h_dec(BR *r,const Huff *h){
    int code=0, first=0, idx=0;
    for(int l=1;l<16;l++){ code|=br_get(r,1); int c=h->cnt[l]; if(code-c<first) return h->sym[idx+(code-first)]; idx+=c; first+=c; first<<=1; code<<=1; if(r->err) return -1; }
    r->err=1; return -1;
}
static int inflate_raw(const uint8_t *in,int n,uint8_t *out,int outmax){
    BR r={in,n,0,0,0,0}; int o=0, last;
    do{
        last=br_get(&r,1); int type=br_get(&r,2);
        if(type==0){
            r.bits=0; r.nb=0; if(r.pos+4>n) return -1;
            int len=(int)rd16(in+r.pos); r.pos+=4; if(r.pos+len>n||o+len>outmax) return -1;
            memcpy(out+o,in+r.pos,(size_t)len); r.pos+=len; o+=len; continue;
        }
        Huff lh, dh; uint8_t ln[320];
        if(type==1){
            for(int i=0;i<288;i++) ln[i]=i<144?8:i<256?9:i<280?7:8;
            h_build(&lh,ln,288); for(int i=0;i<30;i++) ln[i]=5; h_build(&dh,ln,30);
        } else if(type==2){
            static const uint8_t ORD[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            int hl=br_get(&r,5)+257, hd=br_get(&r,5)+1, hc=br_get(&r,4)+4;
            uint8_t cl[19]; memset(cl,0,sizeof cl); for(int i=0;i<hc;i++) cl[ORD[i]]=(uint8_t)br_get(&r,3);
            Huff ch; h_build(&ch,cl,19);
            int k=0; while(k<hl+hd){
                int s=h_dec(&r,&ch); if(s<0) return -1;
                if(s<16) ln[k++]=(uint8_t)s;
                else { int rep, v=0; if(s==16){ if(!k) return -1; v=ln[k-1]; rep=3+br_get(&r,2); } else if(s==17) rep=3+br_get(&r,3); else rep=11+br_get(&r,7);
                       if(k+rep>hl+hd) return -1;
                       while(rep--) ln[k++]=(uint8_t)v; }
            }
            h_build(&lh,ln,hl); h_build(&dh,ln+hl,hd);
        } else return -1;
        for(;;){
            int s=h_dec(&r,&lh); if(s<0||r.err) return -1;
            if(s<256){ if(o>=outmax) return -1; out[o++]=(uint8_t)s; }
            else if(s==256) break;
            else { s-=257; if(s>=29) return -1; int len=LBASE[s]+br_get(&r,LEXT[s]);
                   int ds=h_dec(&r,&dh); if(ds<0||ds>=30) return -1; int dist=DBASE[ds]+br_get(&r,DEXT[ds]);
                   if(dist>o||o+len>outmax) return -1;
                   for(int i=0;i<len;i++,o++) out[o]=out[o-dist]; }
        }
    } while(!last && !r.err);
    return r.err?-1:o;
}

/* ---------- crear el .zip ---------- */
typedef struct { Buf zip, cd; int count; } ZW;
static void z_entry(ZW *z,const char *name,const uint8_t *data,int len,int isdir){
    int nl=k_strlen(name); uint32_t crc=isdir?0:crc32(data,len);
    Buf c={0,0,0,0}; int method=0;
    if(!isdir&&len>64){ if(deflate_fixed(data,len,&c)==0 && c.n<len) method=8; }
    const uint8_t *pd=method?c.d:data; int pl=method?c.n:len;
    uint32_t off=(uint32_t)z->zip.n, dt=(uint32_t)((2026-1980)<<9|1<<5|1), tm=0;
    b_u32(&z->zip,0x04034b50); b_u16(&z->zip,20); b_u16(&z->zip,0x800); b_u16(&z->zip,(uint32_t)method); b_u16(&z->zip,tm); b_u16(&z->zip,dt);
    b_u32(&z->zip,crc); b_u32(&z->zip,(uint32_t)pl); b_u32(&z->zip,(uint32_t)len); b_u16(&z->zip,(uint32_t)nl); b_u16(&z->zip,0);
    b_put(&z->zip,name,nl); if(pl) b_put(&z->zip,pd,pl);
    b_u32(&z->cd,0x02014b50); b_u16(&z->cd,0x031E); b_u16(&z->cd,20); b_u16(&z->cd,0x800); b_u16(&z->cd,(uint32_t)method); b_u16(&z->cd,tm); b_u16(&z->cd,dt);
    b_u32(&z->cd,crc); b_u32(&z->cd,(uint32_t)pl); b_u32(&z->cd,(uint32_t)len); b_u16(&z->cd,(uint32_t)nl); b_u16(&z->cd,0); b_u16(&z->cd,0); b_u16(&z->cd,0); b_u16(&z->cd,0);
    b_u32(&z->cd,isdir?(0x41ED0000u|0x10):0x81A40000u); b_u32(&z->cd,off); b_put(&z->cd,name,nl);
    if(c.d) kfree(c.d);
    z->count++;
}
static void z_add(ZW *z,FsNode *n,const char *prefix){
    char nm[256]; nm[0]=0; int k=0;
    for(const char *s=prefix;*s&&k<200;s++) nm[k++]=*s;
    for(const char *s=n->name;*s&&k<250;s++) nm[k++]=*s;
    if(n->dir){ nm[k++]='/'; nm[k]=0; z_entry(z,nm,0,0,1);
        for(FsNode *c=n->child;c;c=c->next) if(!c->link&&!c->dev) z_add(z,c,nm); }
    else { nm[k]=0; z_entry(z,nm,(const uint8_t*)n->data,n->size,0); }
}
FsNode *zip_create(FsNode *src,FsNode *dir,const char *name){
    if(!src||!dir) return 0;
    ZW z; memset(&z,0,sizeof z);
    z_add(&z,src,"");
    uint32_t cdoff=(uint32_t)z.zip.n;
    if(z.cd.n) b_put(&z.zip,z.cd.d,z.cd.n);
    b_u32(&z.zip,0x06054b50); b_u16(&z.zip,0); b_u16(&z.zip,0); b_u16(&z.zip,(uint32_t)z.count); b_u16(&z.zip,(uint32_t)z.count);
    b_u32(&z.zip,(uint32_t)z.cd.n); b_u32(&z.zip,cdoff); b_u16(&z.zip,0);
    FsNode *r=0;
    if(!z.zip.bad&&!z.cd.bad) r=fs_create(dir,name,(const char*)z.zip.d,z.zip.n);
    if(z.zip.d) kfree(z.zip.d);
    if(z.cd.d) kfree(z.cd.d);
    return r;
}

/* ---------- extraer ---------- */
static FsNode *mkpath(FsNode *base,const char *path,int upto_last){    /* crea las carpetas de 'path'; si upto_last, la ultima parte es un archivo */
    char part[FS_NAME]; FsNode *d=base; const char *s=path;
    for(;;){
        int k=0; while(*s&&*s!='/'&&k<FS_NAME-1) part[k++]=*s++; part[k]=0;
        while(*s&&*s!='/') s++;
        int final=!*s||(!s[1]);
        if(final&&upto_last) return d;
        if(k&&!(part[0]=='.'&&(!part[1]||(part[1]=='.'&&!part[2])))){
            FsNode *e=fs_find(d,part); if(!e) e=fs_mkdir(d,part); if(!e||!e->dir) return 0; d=e; }
        if(!*s) return d;
        s++;
        if(!*s) return d;
    }
}
int zip_extract(FsNode *zf,FsNode *dest){
    if(!zf||zf->dir||!dest||zf->size<22) return -1;
    const uint8_t *p=(const uint8_t*)zf->data; int n=zf->size, e=-1;
    for(int i=n-22;i>=0&&i>=n-22-65535;i--) if(rd32(p+i)==0x06054b50){ e=i; break; }
    if(e<0) return -1;
    int cnt=(int)rd16(p+e+10); uint32_t cdo=rd32(p+e+16); int got=0;
    if(cdo>(uint32_t)n) return -1;
    int q=(int)cdo;
    for(int i=0;i<cnt;i++){
        if(q+46>n||rd32(p+q)!=0x02014b50) return got?got:-1;
        int method=(int)rd16(p+q+10), nl=(int)rd16(p+q+28), xl=(int)rd16(p+q+30), cl=(int)rd16(p+q+32);
        uint32_t crc=rd32(p+q+16), cs=rd32(p+q+20), us=rd32(p+q+24), lo=rd32(p+q+42);
        if(q+46+nl>n) return got?got:-1;
        char name[256]; int k=nl<255?nl:255; memcpy(name,p+q+46,(size_t)k); name[k]=0;
        for(int j=0;j<k;j++) if(name[j]=='\\') name[j]='/';
        q+=46+nl+xl+cl;
        if(lo+30>(uint32_t)n||rd32(p+lo)!=0x04034b50) continue;
        uint32_t ds=lo+30+rd16(p+lo+26)+rd16(p+lo+28);
        if(ds+cs>(uint32_t)n||us>(256u<<20)) continue;
        if(k&&name[k-1]=='/'){ mkpath(dest,name,0); continue; }
        FsNode *d=mkpath(dest,name,1); if(!d) continue;
        const char *leaf=name; for(const char *s=name;*s;s++) if(*s=='/'&&s[1]) leaf=s+1;
        if(!leaf[0]) continue;
        uint8_t *buf=kmalloc(us+1); if(!buf) return got?got:-1;
        int ok=0;
        if(method==0&&cs==us){ memcpy(buf,p+ds,us); ok=1; }
        else if(method==8){ int r=inflate_raw(p+ds,(int)cs,buf,(int)us); ok=(r==(int)us); }
        if(ok&&crc32(buf,(int)us)==crc&&fs_create(d,leaf,(const char*)buf,(int)us)) got++;
        kfree(buf);
    }
    return got;
}
