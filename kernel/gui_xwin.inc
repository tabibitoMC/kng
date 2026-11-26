/* gui_xwin.inc — programas de Linux en ventanas propias de kng ("sin pantalla X").
 * kng-xinput (dentro de Debian) escribe en /tmp/.kng-xwin la lista de ventanas visibles del servidor X:
 *   "kngx 1" y luego una linea por ventana, de abajo hacia arriba:  id x y ancho alto emergente titulo
 * El servidor X mide lo mismo que la pantalla de kng y cada ventana X esta exactamente debajo del contenido de su
 * ventana de kng: lo que se ve se copia del framebuffer de Xvfb en el mismo sitio. Mover, cambiar el tamano,
 * traer al frente o cerrar se le pide a kng-xinput con eventos 0x31/0x30 por /dev/input/event0.
 * Las ventanas "emergentes" de X (menus, listas, avisos) se dibujan sin marco y siempre encima. */

static int xw_grab=-1;                                                /* ventana X que recibe el raton mientras hay un boton pulsado */
static int xw_top_last=-1;
static void xw_send(int code,uint32_t id){ kinput_event(0x30,code,(int)id); }
static void xw_conf(Win *w,int x,int y,int ww,int hh){
    kinput_event(0x31,0,x); kinput_event(0x31,1,y); kinput_event(0x31,2,ww); kinput_event(0x31,3,hh); xw_send(1,w->xid);
    w->xsx=x; w->xsy=y; w->xsw=ww; w->xsh=hh; w->xsent=kticks();
}
static const unsigned char *xw_fb(int *pw,int *ph,int *stride){       /* pixeles de Xvfb (32 bits) */
    FsNode *xf=fs_resolve(fs_root(),"/var/lib/xvfb/Xvfb_screen0");
    const unsigned char *d=xf&&!xf->dir?(const unsigned char*)xf->data:0;
    if(!d||xf->size<100) return 0;
    #define XBE(o) ((uint32_t)d[o]<<24|(uint32_t)d[(o)+1]<<16|(uint32_t)d[(o)+2]<<8|(uint32_t)d[(o)+3])
    uint32_t hs=XBE(0), w=XBE(16), h=XBE(20), bpp=XBE(44), bpl=XBE(48), ncol=XBE(76); uint32_t off=hs+ncol*12;
    #undef XBE
    if(bpp!=32||!w||!h||w>8192||h>8192||off+(uint64_t)bpl*h>(uint64_t)xf->size) return 0;
    *pw=(int)w; *ph=(int)h; *stride=(int)(bpl/4); return d+off;
}
static void xw_icon_load(Win *w){
    if(w->xicon||w->xpop) return;
    char p[48]="/tmp/.kng-xicon-"; { static const char HX[]="0123456789abcdef"; char t[12]; int n=0; uint32_t v=w->xid; do{ t[n++]=HX[v&15]; v>>=4; }while(v); int l=k_strlen(p); while(n) p[l++]=t[--n]; p[l]=0; }
    FsNode *f=fs_resolve(fs_root(),p); if(!f||f->dir||f->size<12||!f->data) return;
    const unsigned char *d=(const unsigned char*)f->data;
    int iw=d[0]|d[1]<<8, ih=d[4]|d[5]<<8; if(iw<1||ih<1||iw>256||ih>256||8+iw*ih*4>f->size) return;
    uint32_t *b=kmalloc((size_t)iw*ih*4); if(!b) return;
    for(int i=0;i<iw*ih;i++){ const unsigned char *s=d+8+i*4; unsigned a=s[3], r=s[2]*a/255, g=s[1]*a/255, bl=s[0]*a/255;
        b[i]=(a<<24)|(r<<16)|(g<<8)|bl; }
    w->xicon=b; w->xiw=iw; w->xih=ih;
}
static uint32_t xw_hash(const Win *w){                                /* muestra del contenido: solo se redibuja si cambia */
    int pw,ph,st; const unsigned char *b=xw_fb(&pw,&ph,&st); if(!b) return 0;
    const uint32_t *px=(const uint32_t*)(const void*)b; uint32_t h=2166136261u;
    int x0=k_max(0,w->xgx), y0=k_max(0,w->xgy), x1=k_min(pw,w->xgx+w->xgw), y1=k_min(ph,w->xgy+w->xgh);
    for(int y=y0;y<y1;y+=3){ const uint32_t *r=px+y*st; for(int x=x0+((y/3)&7);x<x1;x+=9) h=(h^r[x])*16777619u; }
    return h;
}
static void xw_content(const Win *w,int *cx,int *cy,int *cw,int *ch){
    if(w->xpop){ *cx=w->x; *cy=w->y; *cw=w->w; *ch=w->h; } else { *cx=w->x+1; *cy=w->y+TB; *cw=w->w-2; *ch=w->h-TB-1; }
}
static void xw_draw(Win *w){
    int cx,cy,cw,ch; xw_content(w,&cx,&cy,&cw,&ch);
    int pw,ph,st; const unsigned char *b=xw_fb(&pw,&ph,&st);
    if(!b){ fill(cx,cy,cw,ch,0x000000,255); return; }
    int sx=w->xgx, sy=w->xgy, bw=k_min(cw,w->xgw), bh=k_min(ch,w->xgh), dx=cx, dy=cy;
    if(sx<0){ bw+=sx; dx-=sx; sx=0; }
    if(sy<0){ bh+=sy; dy-=sy; sy=0; }
    bw=k_min(bw,pw-sx); bh=k_min(bh,ph-sy);
    if(bw>0&&bh>0) blit32(dx,dy,(const uint32_t*)(const void*)b+sy*st+sx,st,bw,bh);
    if(bw<cw) fill(dx+k_max(0,bw),cy,cw-k_max(0,bw)-(dx-cx),ch,C_BG,255);
    if(bh<ch) fill(cx,dy+k_max(0,bh),cw,ch-k_max(0,bh)-(dy-cy),C_BG,255);
}
static int xw_count_dock(int *idx){                                   /* ventanas X normales, para el dock */
    int n=0; for(int i=0;i<MAXW&&n<XW_EXTRA;i++){ Win *w=&wins[i]; if(w->used&&w->type==A_XWIN&&!w->xpop){ if(idx) idx[n]=i; n++; } }   /* orden fijo: el de apertura */
    return n;
}
/* lee la lista de kng-xinput y crea, mueve o cierra las ventanas de kng que les corresponden */
static void xw_sync(void){
    FsNode *f=fs_resolve(fs_root(),"/tmp/.kng-xwin");
    static const char *lastd; static int lasts; static uint32_t lasth;
    if(!f||f->dir||!f->data||f->size<7){ if(lasts){ lasts=0; for(int i=0;i<MAXW;i++) if(wins[i].used&&wins[i].type==A_XWIN) win_close(i); } return; }
    uint32_t h=2166136261u; for(int i=0;i<f->size;i++) h=(h^(unsigned char)f->data[i])*16777619u;
    if(f->data==lastd&&f->size==lasts&&h==lasth) return;
    lastd=f->data; lasts=f->size; lasth=h;
    int seen[MAXW]; memset(seen,0,sizeof seen);
    const char *p=f->data, *e=f->data+f->size; while(p<e&&*p!='\n') p++; if(p<e) p++;
    while(p<e){
        const char *le=p; while(le<e&&*le!='\n') le++;
        uint32_t id=0; int v[5]={0,0,0,0,0}; const char *q=p;
        while(q<le&&*q!=' '){ char c=*q++; id=id*16+(uint32_t)(c>='a'?c-'a'+10:c>='A'?c-'A'+10:c-'0'); }
        for(int i=0;i<5;i++){ while(q<le&&*q==' ') q++; int neg=0; if(q<le&&*q=='-'){ neg=1; q++; } int n=0; while(q<le&&*q>='0'&&*q<='9') n=n*10+(*q++-'0'); v[i]=neg?-n:n; }
        if(q<le&&*q==' ') q++;
        char title[40]; { int l=(int)(le-q); if(l>39) l=39; memcpy(title,q,(size_t)l); title[l]=0; }
        p=le<e?le+1:le;
        if(!id||v[2]<2||v[3]<2) continue;
        int wi=-1; for(int i=0;i<MAXW;i++) if(wins[i].used&&wins[i].type==A_XWIN&&wins[i].xid==id){ wi=i; break; }
        Win *w;
        if(wi<0){
            open_new=1; w=open_app(A_XWIN); open_new=0; if(!w) continue;
            wi=(int)(w-wins); w->xid=id; w->xpop=v[4];
            w->xgx=v[0]; w->xgy=v[1]; w->xgw=v[2]; w->xgh=v[3];
            if(w->xpop){ w->aopen=0; w->x=v[0]; w->y=v[1]; w->w=v[2]; w->h=v[3]; }
            else {
                set_title(w,title[0]?title:"Programa de Linux");
                int maxw=gw-24, maxh=gh-BARH-dock_res()-TB-14, cw=k_min(v[2],maxw), ch=k_min(v[3],maxh);
                int placed=v[0]>=1&&v[1]>=BARH+TB&&v[0]+cw<=gw&&v[1]+ch<=gh-dock_res();
                if(placed){ w->x=v[0]-1; w->y=v[1]-TB; }
                else { w->x=k_max(0,(gw-cw-2)/2+(nord%4)*24-36); w->y=k_max(BARH,BARH+12+(nord%4)*24); }
                w->w=cw+2; w->h=ch+TB+1;
                xw_conf(w,w->x+1,w->y+TB,cw,ch); xw_icon_load(w);
            }
            dirty=1;
        } else {
            w=&wins[wi];
            if(w->xgx!=v[0]||w->xgy!=v[1]||w->xgw!=v[2]||w->xgh!=v[3]){ dwin(w); w->xgx=v[0]; w->xgy=v[1]; w->xgw=v[2]; w->xgh=v[3]; dwin(w);
                if(w->xpop){ w->x=v[0]; w->y=v[1]; w->w=v[2]; w->h=v[3]; dwin(w); } }
            if(!w->xpop&&title[0]&&k_strcmp(title,w->title)){ set_title(w,title); dwin(w); dmark_bars(); }
            if(!w->xpop) xw_icon_load(w);
        }
        seen[wi]=1;
    }
    for(int i=0;i<MAXW;i++) if(wins[i].used&&wins[i].type==A_XWIN&&!seen[i]){ if(xw_grab==i) xw_grab=-1;
        if(wins[i].xpop) win_close(i); else win_close_anim(i); }
    dmark_dock();
}
/* cada cuadro: pedir a X la posicion/tamano que tiene la ventana de kng; aceptar los cambios que haga el programa */
static void xw_tick(uint32_t t){
    static uint32_t next, nexth;
    if((int)(t-next)>=0){ next=t+8; xw_sync(); }
    int top=win_top();
    if(top!=xw_top_last){ xw_top_last=top; if(top>=0&&wins[top].type==A_XWIN&&!wins[top].xpop) xw_send(2,wins[top].xid); }
    int hash_now=(int)(t-nexth)>=0; if(hash_now) nexth=t+3;
    for(int k=0;k<nord;k++){ int wi=order[k]; Win *w=&wins[wi]; if(w->type!=A_XWIN) continue;
        if(hash_now&&!w->minim){ uint32_t hh=xw_hash(w); if(hh!=w->xhash){ w->xhash=hh; int cx,cy,cw,ch; xw_content(w,&cx,&cy,&cw,&ch); dmark(cx,cy,cw,ch); } }
        if(w->xpop) continue;
        int dx,dy,dw,dh;
        if(w->minim){ dx=-w->w-60; dy=w->y+TB; dw=w->w-2; dh=w->h-TB-1; }        /* minimizada: fuera de la pantalla de X */
        else { dx=w->x+1; dy=w->y+TB; dw=w->w-2; dh=w->h-TB-1; }
        int busy=(drag==wi||rsz==wi);
        if(dx!=w->xsx||dy!=w->xsy||dw!=w->xsw||dh!=w->xsh){ if(!busy||(int)(t-w->xsent)>=4) xw_conf(w,dx,dy,dw,dh); }
        else if(!busy&&!w->minim&&(int)(t-w->xsent)>60&&(w->xgx!=w->xsx||w->xgy!=w->xsy||w->xgw!=w->xsw||w->xgh!=w->xsh)){    /* el programa se movio o cambio solo */
            dwin(w); w->x=w->xgx-1; w->y=w->xgy-TB; w->w=w->xgw+2; w->h=w->xgh+TB+1; w->xsx=w->xgx; w->xsy=w->xgy; w->xsw=w->xgw; w->xsh=w->xgh; dwin(w); }
    }
}
/* raton sobre ventanas X: devuelve la ventana (o -1) y la posicion dentro de la pantalla X */
static int xw_at(int px,int py,int *xx,int *xy){
    for(int k=nord-1;k>=0;k--){ Win *w=&wins[order[k]]; if(w->type!=A_XWIN||!w->xpop) continue;
        if(in_rect(px,py,w->x,w->y,w->w,w->h)){ *xx=w->xgx+px-w->x; *xy=w->xgy+py-w->y; return order[k]; } }
    for(int k=nord-1;k>=0;k--){ Win *w=&wins[order[k]]; if(w->minim||w->xpop) continue;
        if(!in_rect(px,py,w->x-5,w->y,w->w+10,w->h+5)) continue;
        if(w->type!=A_XWIN) return -1;
        int cx,cy,cw,ch; xw_content(w,&cx,&cy,&cw,&ch);
        if(!in_rect(px,py,cx,cy,cw,ch)) return -1;
        *xx=w->xgx+px-cx; *xy=w->xgy+py-cy; return order[k]; }
    return -1;
}
static int xw_lx=-1, xw_ly=-1;
/* 1 si el raton se le entrego a un programa de Linux (el escritorio no hace nada mas con ese clic) */
static int xw_mouse(int b,int omb){
    int xx=0, xy=0, wi;
    if(xw_grab>=0&&wins[xw_grab].used){ Win *w=&wins[xw_grab]; int cx,cy,cw,ch; xw_content(w,&cx,&cy,&cw,&ch); wi=xw_grab; xx=w->xgx+mx-cx; xy=w->xgy+my-cy; }
    else { if(drag>=0||rsz>=0||ddrag>=0||mu_open) return 0; wi=xw_at(mx,my,&xx,&xy); }
    if(wi<0) return 0;
    Win *w=&wins[wi];
    if(((b&~omb)&7)&&!w->xpop&&win_top()!=wi){ focus(wi); xw_top_last=wi; xw_send(2,w->xid); }     /* al frente antes del clic */
    if(xx!=xw_lx||xy!=xw_ly){ xw_lx=xx; xw_ly=xy; kinput_event(3,0,xx); kinput_event(3,1,xy); kinput_event(0,0,0); }
    static const int bit[3]={1,2,4}, code[3]={0x110,0x111,0x112};
    for(int k=0;k<3;k++) if((b^omb)&bit[k]){ kinput_event(1,code[k],(b&bit[k])?1:0); kinput_event(0,0,0); }
    if(b&7){ if(xw_grab<0) xw_grab=wi; } else xw_grab=-1;
    return ((b^omb)&7)!=0;
}
