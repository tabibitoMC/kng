/* gui_estilo.inc — estilos visuales del escritorio (Frutiger Aero, Windows 11, Mac): fondos, marcos de ventana,
 * barra superior, dock / barra de tareas, animaciones (abrir, cerrar, minimizar, maximizar) y el inicio de sesion.
 * Se incluye desde gui.c (comparte sus variables estaticas). Sin coma flotante. */

/* ---------------- seno entero: periodo 256, resultado -1024..1024 ---------------- */
static const short SIN64[65]={0,25,50,75,100,125,150,175,200,224,249,273,297,321,345,369,392,415,438,460,483,505,526,548,569,590,610,630,650,669,688,706,
    724,742,759,775,792,807,822,837,851,865,878,891,903,915,926,936,946,955,964,972,980,987,993,999,1004,1009,1013,1016,1019,1021,1023,1024,1024};
static int isin(int a){ a&=255; int i=a&63; switch(a>>6){ case 0: return SIN64[i]; case 1: return SIN64[64-i]; case 2: return -SIN64[i]; default: return -SIN64[64-i]; } }
static int icos(int a){ return isin(a+64); }
static int iabs(int v){ return v<0?-v:v; }
static int ease_out(int t){ if(t<=0) return 0; if(t>=256) return 256; int u=256-t; return 256-(u*u/256)*u/256; }   /* 0..256 */
static int ease_in(int t){ if(t<=0) return 0; if(t>=256) return 256; return (t*t/256)*t/256; }

/* ---------------- fondos de los temas (3 = Aero, 4 = Windows 11, 5 = Mac) ---------------- */
static const short BUB[12][3]={{120,190,62},{236,336,34},{338,146,22},{612,262,50},{702,430,28},{884,300,72},{944,566,30},{424,520,40},{80,610,26},{524,118,18},{784,650,22},{300,716,36}};
static const uint32_t PET_A[4]={0x1e40af,0x1d4ed8,0x2563eb,0x3b82f6}, PET_B[4]={0x60a5fa,0x7cc0ff,0x93c5fd,0xbfdbfe};
static const uint32_t WAVE[4]={0x4c1d95,0x9d2f86,0xdc5a6e,0xf3a35a};
static uint32_t wall_px_tema(int v,int x,int y,int W,int H){
    int nx=x*1024/W, ny=y*1024/H; Fx f;
    if(v==3){                                                     /* Frutiger Aero: cielo, sol, colinas, estela de luz y burbujas */
        f=fx_of(0x0a55bd); fx_mix(&f,0x9fe6ff,k_min(4096,ny*4));
        fx_glow(&f,nx,ny,800,70,560,0xffffff,200);
        { int yc=640+isin(nx/4+40)*120/1024, d=iabs(ny-yc); if(d<70){ int k=(70-d)*(70-d)*4096/4900; fx_mix(&f,0xffffff,k*90/255); } }
        { int yc=700+isin(nx/4+60)*100/1024, d=iabs(ny-yc); if(d<22){ int k=(22-d)*4096/22; fx_mix(&f,0xe8fbff,k*70/255); } }
        { int yc=560+isin(nx/3+10)*90/1024, d=iabs(ny-yc); if(d<40){ int k=(40-d)*(40-d)*4096/1600; fx_mix(&f,0x7dffd9,k*55/255); } }   /* aurora verde agua */
        { static const short SP[9][2]={{640,120},{120,420},{900,180},{330,600},{560,380},{760,520},{210,90},{960,420},{450,240}};      /* destellos */
          for(int i=0;i<9;i++){ int ddx=iabs(nx-SP[i][0]), ddy=iabs(ny-SP[i][1]);
              if(ddx<14&&ddy<2) fx_mix(&f,0xffffff,(14-ddx)*200); else if(ddy<14&&ddx<2) fx_mix(&f,0xffffff,(14-ddy)*200); else if(ddx<4&&ddy<4) fx_mix(&f,0xffffff,3000); } }
        fx_glow(&f,nx,ny,160,1200,640,0x67d14a,245); fx_glow(&f,nx,ny,780,1260,760,0x24a148,245); fx_glow(&f,nx,ny,480,1300,520,0x9be86a,160);
        for(int i=0;i<12;i++){ int px=BUB[i][0]*W/1024, py=BUB[i][1]*H/1024, R=BUB[i][2]*H/1024; int dx=x-px, dy=y-py, d2=dx*dx+dy*dy, R2=R*R;
            if(R<2||d2>=R2) continue;
            int t=d2*256/R2, a=t>180?30+(t-180)*2:16; if(t>245) a=a*(256-t)/11;     /* borde mas brillante, suavizado */
            fx_mix(&f,0xffffff,a*16);
            if(t>200){ uint32_t rc=dx>0?(dy>0?0xff9ad5:0xfff3a0):(dy>0?0x9affc8:0xa0e8ff); fx_mix(&f,rc,(t-200)*40); }   /* borde tornasolado */
            int hx=dx+R*2/5, hy=dy+R*2/5, hr=R/3, h2=hx*hx+hy*hy, hr2=hr*hr; if(hr>0&&h2<hr2) fx_mix(&f,0xffffff,(hr2-h2)*2900/hr2); }
    } else if(v==4){                                              /* Windows 11: "flor" azul sobre fondo oscuro */
        f=fx_of(0x081430); fx_mix(&f,0x0d2a66,k_min(4096,ny*4)); fx_glow(&f,nx,ny,512,600,760,0x1d4ed8,210);
        int cx=W/2, cy=H*66/100, dx=x-cx, dy=cy-y, L=H*44/100, A=L/2, B=H*9/100;
        static const signed char ORD[7]={-3,3,-2,2,-1,1,0};
        int A2=k_max(1,A*A>>10), B2=k_max(1,B*B>>10);
        for(int o=0;o<7;o++){ int k=ORD[o], ang=64+k*15, c=icos(ang), s=isin(ang);
            int u=(dx*c+dy*s)/1024-A*11/10, w2=(-dx*s+dy*c)/1024; int q=u*u/A2+w2*w2/B2;
            if(q>=1024) continue;
            int along=k_max(0,k_min(255,(u+A)*255/(2*A))); int sh=iabs(k);
            uint32_t pc=mix(PET_A[3-sh],PET_B[3-sh],along);
            int st=(1024-q)*10; if(st>4096) st=4096; fx_mix(&f,pc,st*(235-sh*25)/255);
            if(q>820&&q<1024) fx_mix(&f,0xdbeafe,(q<920?(q-820):(1024-q))*8); }
    } else {                                                      /* Mac: ondas de colores (estilo Sonoma) */
        f=fx_of(0x120a26); fx_mix(&f,0x24104a,k_min(4096,ny*4)); fx_glow(&f,nx,ny,820,160,620,0x6d28d9,150);
        for(int k=0;k<4;k++){
            int base=430+k*130, amp=70-k*10, ang=x*256*(k+2)/(W*3)+k*47;
            int yk16=(base*H/1024)*16+(isin(ang)*amp/1024)*H*16/1024;
            int cov=y*16+16-yk16; if(cov<=0) continue; if(cov>16) cov=16;
            int dd=y-yk16/16; uint32_t c=WAVE[k];
            c=mix(c,0x000000,k_min(150,dd*400/H));
            if(dd<12) c=mix(c,0xffffff,(12-dd)*9);
            fx_mix(&f,c,cov*256); }
    }
    int d=BAYER[(y&3)*4+(x&3)]*16+8, o[3];
    for(int i=0;i<3;i++){ o[i]=(f.c[i]+d)>>8; if(o[i]>255) o[i]=255; if(o[i]<0) o[i]=0; }
    return ((uint32_t)o[0]<<16)|((uint32_t)o[1]<<8)|(uint32_t)o[2];
}

/* ---------------- cambio de tema y ajustes guardados ---------------- */
static int dock_res(void){ return ESTILO==1?48:ESTILO==0?100:86; }               /* alto que se deja libre abajo al maximizar */
static uint32_t *icache[NDOCK]; static int icache_ok;              /* iconos del dock ya dibujados (alfa premultiplicado) */
static void tema_set(int t){ if(t<0||t>=NTEMAS) return; tema=t; acc_i=-1; wall_i=TM->fondo; bar_h=ESTILO==1?0:28; icache_ok=0; }
static void tema_aplicar(int t,int guardar){
    if(t<0||t>=NTEMAS) return;
    tema_set(t);
    for(int i=0;i<nord;i++){ Win *w=&wins[order[i]]; if(w->y<BARH) w->y=BARH; if(w->maxed){ w->y=BARH; w->h=gh-BARH-dock_res(); } }
    build_wall(); dirty=1;
    if(guardar){ char b[4]={(char)('0'+t),0}; cfg_set("tema",b); cfg_set("acento","-1"); b[0]=(char)('0'+wall_i); cfg_set("fondo",b); }
}
static void ajustes_guardados(void){                               /* tema, acento y fondo de /Sistema/kng.cfg */
    char v[8];
    if(cfg_get("tema",v,sizeof v)&&v[0]>='0'&&v[0]<'0'+NTEMAS) tema_set(v[0]-'0');
    if(cfg_get("acento",v,sizeof v)){ int a=v[0]=='-'?-1:v[0]-'0'; if(a>=-1&&a<6) acc_i=a; }
    if(cfg_get("fondo",v,sizeof v)&&v[0]>='0'&&v[0]<'0'+NFONDOS) wall_i=v[0]-'0';
}

/* ---------------- iconos del dock en memoria (para escalarlos: lupa del Mac, barra de Windows) ---------------- */
static void draw_icon(int i,int x,int y);
static void icon_cache_build(void){
    if(icache_ok) return;
    int S=TILE; uint32_t *save=kmalloc((size_t)S*S*4), *b0=kmalloc((size_t)S*S*4);
    if(!save||!b0){ if(save) kfree(save); if(b0) kfree(b0); return; }
    int X0,Y0,X1,Y1; clip_get(&X0,&Y0,&X1,&Y1); clip_base(0,0,gw,gh);
    for(int j=0;j<S;j++) memcpy(save+j*S,bb+j*gw,(size_t)S*4);
    for(int i=0;i<NDOCK;i++){
        if(!icache[i]) icache[i]=kmalloc((size_t)S*S*4);
        if(!icache[i]) continue;
        fill(0,0,S,S,0x000000,255); draw_icon(i,0,0); for(int j=0;j<S;j++) memcpy(b0+j*S,bb+j*gw,(size_t)S*4);
        fill(0,0,S,S,0xffffff,255); draw_icon(i,0,0);
        for(int j=0;j<S;j++) for(int k=0;k<S;k++){ uint32_t b=b0[j*S+k], w=bb[j*gw+k];
            int a=255-((int)((w>>16)&255)-(int)((b>>16)&255)+(int)((w>>8)&255)-(int)((b>>8)&255)+(int)(w&255)-(int)(b&255))/3;
            a=k_max(0,k_min(255,a));
            int r=(int)((b>>16)&255), g=(int)((b>>8)&255), bl=(int)(b&255); if(r>a) r=a; if(g>a) g=a; if(bl>a) bl=a;
            icache[i][j*S+k]=((uint32_t)a<<24)|((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)bl; }
    }
    for(int j=0;j<S;j++) memcpy(bb+j*gw,save+j*S,(size_t)S*4);
    kfree(save); kfree(b0);
    clip_base(X0,Y0,X1-X0,Y1-Y0); icache_ok=1;
}
static void icon_at(int i,int x,int y,int s,int a){
    if(icache_ok&&icache[i]) blit_pm_scaled(x,y,s,s,icache[i],TILE,TILE,a);
    else if(s==TILE) draw_icon(i,x,y);
}

/* ---------------- dock (Aero y Mac) o barra de tareas (Windows 11) ---------------- */
/* Los primeros NDOCK iconos son los programas de kng; despues van los programas de Linux abiertos (gui_xwin.inc). */
#define XW_EXTRA 8
static int xw_count_dock(int *idx);
typedef struct { int x,y,s; } DSlot;
static DSlot dsl[NDOCK+XW_EXTRA]; static int dbx,dby,dbw,dbh, dock_n=NDOCK, dock_xw[XW_EXTRA];
static int bnc_i=-1; static uint32_t bnc_t;                        /* icono que rebota al abrir un programa */
static int dock_count(void){ dock_n=NDOCK+xw_count_dock(dock_xw); return dock_n; }
static int dock_bh(void){ return ESTILO==0?TILE+30:TILE+16; }        /* Aero: repisa mas alta para el reflejo */
static int dock_itop(void){ return ESTILO==0?6:8; }
static int dock_near(void){
    int n=dock_n, bw0=n*TILE+(n-1)*14+28, bx0=(gw-bw0)/2, by0=gh-dock_bh()-10;
    return mx>=bx0-20&&mx<bx0+bw0+20&&my>=by0-34&&my<gh;
}
static void dock_layout(void){
    int n=dock_count();
    if(ESTILO==1){ int S=36, G=10, tot=n*S+(n-1)*G+(n>NDOCK?14:0), x0=(gw-tot)/2;
        dbx=0; dby=gh-48; dbw=gw; dbh=48;
        for(int i=0;i<n;i++){ dsl[i].x=x0+i*(S+G)+(i>=NDOCK?14:0); dsl[i].y=dby+6; dsl[i].s=S; }
        return; }
    int bh=dock_bh(), by=gh-bh-10, s[NDOCK+XW_EXTRA], tot=0, near=dock_near()&&!mu_open&&drag<0&&!bloqueado;
    int bw0=n*TILE+(n-1)*14+28, bx0=(gw-bw0)/2, it=dock_itop();
    int gmax=ESTILO==2?30:12, rad=ESTILO==2?150:110;
    for(int i=0;i<n;i++){ int c=bx0+14+i*(TILE+14)+TILE/2, g=0;
        if(near){ int d=iabs(mx-c); if(d<rad) g=gmax*(1024+icos(d*128/rad))/2048; }
        s[i]=TILE+g; tot+=s[i]; }
    int bw=tot+(n-1)*14+28, bx=(gw-bw)/2, x=bx+14;
    if(near){                                                     /* el punto bajo el raton se queda quieto al ampliar */
        int rel=mx-(bx0+7), j=rel/(TILE+14), f=rel%(TILE+14);
        if(rel<0){ j=0; f=0; }
        if(j>=n){ j=n-1; f=TILE+14; }
        int mp=0; for(int i=0;i<j;i++) mp+=s[i]+14; mp+=f*(s[j]+14)/(TILE+14);
        x=mx-mp+7; bx=x-14; }
    uint32_t t=kticks();
    for(int i=0;i<n;i++){ int up=0;
        if(i==bnc_i&&t-bnc_t<70){ int e=(int)(t-bnc_t); up=iabs(isin(e*256/35))*16/1024*(70-e)/70; }
        dsl[i].x=x; dsl[i].s=s[i]; dsl[i].y=by+it+TILE-s[i]-up; x+=s[i]+14; }
    dbx=bx; dby=by; dbw=bw; dbh=bh;
}
__attribute__((unused)) static void dock_base_center(int i,int *x,int *y){                 /* centro del icono sin ampliar (pruebas automaticas) */
    if(ESTILO==1){ dock_layout(); *x=dsl[i].x+dsl[i].s/2; *y=dsl[i].y+dsl[i].s/2; return; }
    int n=dock_count(), bw0=n*TILE+(n-1)*14+28, bx0=(gw-bw0)/2, by0=gh-dock_bh()-10; *x=bx0+14+i*(TILE+14)+TILE/2; *y=by0+dock_itop()+TILE/2;
}
static void dock_geom(int *bx,int *by,int *bw,int *bh){ dock_layout(); *bx=dbx; *by=dby; *bw=dbw; *bh=dbh; }
static void dmark_dock(void){ if(ESTILO==1) dmark(0,gh-90,gw,90); else dmark(0,gh-200,gw,200); }
static void dmark_bars(void){ dmark(0,0,gw,BARH); dmark_dock(); }
static int dock_hit(int px,int py){
    dock_layout();
    if(ESTILO==1){ if(py<dby) return -1;
        for(int i=0;i<dock_n;i++) if(px>=dsl[i].x-5&&px<dsl[i].x+dsl[i].s+5) return i;
        return -2; }
    for(int i=0;i<dock_n;i++) if(in_rect(px,py,dsl[i].x-7,dsl[i].y-4,dsl[i].s+14,dby+dbh-dsl[i].y+4)) return i;
    if(in_rect(px,py,dbx,dby,dbw,dbh)) return -2;                        /* sobre el dock, pero entre iconos */
    return -1;
}
static const char *dock_label(int i){ return i<NDOCK?DOCK_LBL[i]:wins[dock_xw[i-NDOCK]].title; }
static int dock_running(int i,int *act){
    int top=win_top(); *act=0;
    if(i>=NDOCK){ *act=dock_xw[i-NDOCK]==top; return 1; }
    int run=0; for(int k=0;k<nord;k++) if(wins[order[k]].type==DOCK_APP[i]){ run=1; if(order[k]==top) *act=1; }
    return run;
}
static void xicon_draw(const Win *w,int x,int y,int S,int a){      /* icono de un programa de Linux (el suyo o una letra) */
    if(w->xicon){ int m=S/12; blit_pm_scaled(x+m,y+m,S-2*m,S-2*m,w->xicon,w->xiw,w->xih,a); return; }
    uint32_t h=5381; for(const char *q=w->title;*q;q++) h=h*33u+(unsigned char)*q;
    static const uint32_t PAL[6]={0x2f80ed,0x27ae60,0xeb5757,0xf2994a,0x9b51e0,0x00a3a3};
    uint32_t c=PAL[h%6]; int m=S/10;
    rrect_grad(x+m,y+m,S-2*m,S-2*m,S/5,mix(c,0xffffff,100),c,a);
    char in[2]={w->title[0]?w->title[0]:'L',0}; if(in[0]>='a'&&in[0]<='z') in[0]=(char)(in[0]-32);
    int th=S/2; text_scaled(x+(S-th/2)/2,y+(S-th)/2,in,0xffffff,th);
}
static void dock_icon(int i,int x,int y,int S,int a){ if(i<NDOCK) icon_at(i,x,y,S,a); else xicon_draw(&wins[dock_xw[i-NDOCK]],x,y,S,a); }
static void dock_tip(int i,int cx,int ly){
    char lb[40]; k_strcpy(lb,dock_label(i),sizeof lb); int lw=text_w(lb)+16, lx=k_max(4,k_min(gw-lw-4,cx-lw/2));
    if(ESTILO==2){ rrect(lx,ly,lw,24,6,0x2a2a2c,235); rrect(lx,ly,lw,24,6,0xffffff,20); }
    else if(ESTILO==1){ rrect(lx,ly,lw,26,6,0x2b2b2b,240); rrect(lx,ly,lw,26,6,0xffffff,14); ly+=1; }
    else { rrect(lx,ly,lw,24,8,0xffffff,170); rrect_grad(lx+1,ly+1,lw-2,22,7,0x5fb0f0,0x165aa8,240); clip_set(lx,ly,lw,12); rrect(lx+1,ly+1,lw-2,22,7,0xffffff,60); clip_reset(); text(lx+9,ly+5,lb,0x08294d); }
    text(lx+8,ly+4,lb,0xffffff);
}
static void clock_str(char *t){
    int h=clk_h; t[0]=0;
    if(!show24){ int am=h>=12; h=h%12; if(!h) h=12; cat_int(t,16,h); cat(t,16,":"); if(clk_m<10) cat(t,16,"0"); cat_int(t,16,clk_m); cat(t,16,am?" PM":" AM"); }
    else { if(h<10) cat(t,16,"0"); cat_int(t,16,h); cat(t,16,":"); if(clk_m<10) cat(t,16,"0"); cat_int(t,16,clk_m); }
}
static void tray_draw(int rx,int cy,uint32_t col){                 /* red, teclado y ajustes, de derecha a izquierda desde rx */
    circle(rx-14,cy,7,col,255); circle(rx-14,cy,4,0x05060c,200); fill(rx-15,cy-10,2,6,col,255);
    { const char *lc=layout_code(kbd_layout()); char up[4]={(char)(lc[0]-32),(char)(lc[1]?lc[1]-32:0),(char)(lc[1]&&lc[2]?lc[2]-32:0),0}; text(rx-40-text_w(up),cy-8,up,col); }
    { int st=net_state(), nx=rx-100; for(int b=0;b<4;b++) fill(nx+b*5,cy+6-(b+1)*3,3,(b+1)*3,st>=2?0x7ee787:col,st>=2?255:(b==0?200:70)); }
}
static void draw_topbar(void){
    if(!BARH) return;
    char t[16]; clock_str(t);
    blur_rect(0,0,gw,BARH,6);
    if(ESTILO==0){                                                /* Aero: vidrio azul con brillo */
        for(int j=0;j<BARH;j++) fill(0,j,gw,1,mix(0x9bd8ff,0x1f6fc4,j*255/BARH),165);
        fill(0,0,gw,BARH/2,0xffffff,38); fill(0,0,gw,1,0xffffff,120); fill(0,BARH-1,gw,1,0x0b3a6e,170);
        crown(12,6,22,0xffe08a,ACCENT,255); text(41,7,"Königreich OS",0x0b2e55); text(40,6,"Königreich OS",0xffffff);
        text_bold((gw-text_w(t))/2+1,7,t,0x0b2e55); text_bold((gw-text_w(t))/2,6,t,0xffffff);
        tray_draw(gw-12,BARH/2,0xeaf6ff);
    } else {                                                      /* Mac: barra de menus translucida, nombre del programa activo */
        fill(0,0,gw,BARH,0x1c1c1e,175); fill(0,BARH-1,gw,1,0x000000,90);
        crown(12,6,22,0xe9e9e9,0xe9e9e9,235);
        int top=win_top(); const char *ap=top>=0?wins[top].title:"Finder de kng";
        char ab[28]; k_strcpy(ab,ap,sizeof ab);
        int ex=text_bold(44,6,ab,0xffffff);
        static const char *MENU[4]={"Archivo","Edicion","Ver","Ventana"};
        for(int i=0;i<4;i++) ex=text(ex+20,6,MENU[i],0xd8d8d8);
        int cw=text_w(t); text(gw-140-cw,6,t,0xffffff);
        tray_draw(gw-12,BARH/2,0xd8d8d8);
    }
}
static void draw_dock(void){
    icon_cache_build(); dock_layout();
    if(ESTILO==1){                                                /* barra de tareas de Windows 11 */
        blur_rect(0,dby,gw,dbh,8); fill(0,dby,gw,dbh,0x1b1b1f,200); fill(0,dby,gw,1,0xffffff,26);
        for(int i=0;i<dock_n;i++){ int x=dsl[i].x, y=dsl[i].y, S=dsl[i].s, act, run=dock_running(i,&act);
            if(hover==i||act) rrect(x-5,y-4,S+10,S+8,6,0xffffff,act?26:18);
            dock_icon(i,x,y,S,255);
            if(run) rrect(x+S/2-(act?9:3),dby+dbh-5,act?18:6,3,1,act?ACCENT:0x9a9a9a,255); }
        if(dock_n>NDOCK) fill(dsl[NDOCK].x-12,dby+12,1,dbh-24,0xffffff,40);
        if(hover>=0&&hover<dock_n) dock_tip(hover,dsl[hover].x+dsl[hover].s/2,dby-34);
        char t[16]; clock_str(t); int tw=text_w(t); text(gw-14-tw,dby+16,t,0xffffff);
        tray_draw(gw-24-tw,dby+dbh/2,0xd0d0d0);
        return; }
    blur_rect(dbx+4,dby,dbw-8,dbh,6);
    if(ESTILO==0){                                                /* Aero: repisa de vidrio con brillo y reflejo de los iconos */
        rrect(dbx,dby,dbw,dbh,18,0xffffff,110);
        rrect_grad(dbx+1,dby+1,dbw-2,dbh-2,17,0x7cc6f5,0x1c5ea8,135);
        clip_set(dbx,dby,dbw,dbh*2/5); rrect(dbx+1,dby+1,dbw-2,dbh-2,17,0xffffff,58); clip_reset();
        fill(dbx+18,dby+1,dbw-36,1,0xffffff,190);
        rrect(dbx+10,dby+dbh-20,dbw-20,12,6,0x9ff0ff,38);                     /* luz de la repisa */
        clip_set(dbx+2,dby+6+TILE+1,dbw-4,dbh-TILE-8);
        for(int i=0;i<dock_n;i++){ int x=dsl[i].x, S=dsl[i].s, ry=dby+6+TILE+1;           /* reflejo: el icono al reves, desvaneciendose */
            if(i<NDOCK&&icache_ok&&icache[i]) blit_pm_reflect(x,ry,S,S,icache[i],TILE,TILE,90);
            else if(i>=NDOCK&&wins[dock_xw[i-NDOCK]].xicon){ const Win *xw=&wins[dock_xw[i-NDOCK]]; blit_pm_reflect(x+S/12,ry,S-S/6,S-S/6,xw->xicon,xw->xiw,xw->xih,90); } }
        clip_reset();
    } else { rrect(dbx,dby,dbw,dbh,18,0xffffff,46); rrect(dbx+1,dby+1,dbw-2,dbh-2,17,0x262628,150); }
    if(dock_n>NDOCK){ int sx=dsl[NDOCK].x-8; fill(sx,dby+10,1,dbh-20,ESTILO==0?0xffffff:0xffffff,ESTILO==0?120:50); }
    for(int i=0;i<dock_n;i++){ int x=dsl[i].x, y=dsl[i].y, S=dsl[i].s, act, run=dock_running(i,&act);
        if(ESTILO==0&&(hover==i||act)){ rrect(x-5,y-5,S+10,S+10,16,0xffffff,hover==i?70:34); if(hover==i) circle(x+S/2,dby+dbh-6,S/3,0xbff4ff,40); }
        dock_icon(i,x,y,S,255);
        if(run){ if(ESTILO==2) circle(x+S/2,dby+dbh-4,2,0xdddddd,255); else { rrect(x+S/2-10,dby+dbh-6,20,4,2,0xffffff,act?250:150); circle(x+S/2,dby+dbh-4,6,0xbff4ff,act?70:30); } }
    }
    if(hover>=0&&hover<dock_n) dock_tip(hover,dsl[hover].x+dsl[hover].s/2,dsl[hover].y-32);
}

/* ---------------- paneles de vidrio (widgets, menus) y reloj de aguja ---------------- */
static void glass_panel(int x,int y,int w,int h,int r){
    int X0,Y0,X1,Y1; clip_get(&X0,&Y0,&X1,&Y1);
    blur_rect(x,y,w,h,6);
    if(ESTILO==0){
        rrect(x,y,w,h,r,0xffffff,150); rrect_grad(x+1,y+1,w-2,h-2,r-1,0x5fb0ee,0x123f80,190);
        clip_set(x,y,w,h*2/5); rrect(x+1,y+1,w-2,h-2,r-1,0xffffff,62); clip_set(X0,Y0,X1-X0,Y1-Y0);
        fill(x+r,y+1,w-2*r,1,0xffffff,200); rrect(x+8,y+h-14,w-16,8,4,0xbff4ff,40);
    } else if(ESTILO==1){ rrect(x,y,w,h,r,0xffffff,22); rrect(x+1,y+1,w-2,h-2,r-1,0x202024,205); }
    else { rrect(x,y,w,h,r,0xffffff,34); rrect(x+1,y+1,w-2,h-2,r-1,0x1c1c1e,175); }
}
static void aero_clock(int cx,int cy,int R){
    circle(cx,cy+2,R+2,0x000000,50);
    circle(cx,cy,R+1,0x0b3a6e,255); circle(cx,cy,R,0xd8eefc,255);
    for(int k=0;k<6;k++) circle(cx-k,cy-k,R-4-k*3,mix(0xffffff,0xb7dcf5,k*40),255);   /* cara con degradado */
    for(int i=0;i<12;i++){ int a=i*256/12, c=icos(a), s=isin(a), l=i%3?4:8;
        line(cx+s*(R-3)/1024,cy-c*(R-3)/1024,cx+s*(R-3-l)/1024,cy-c*(R-3-l)/1024,i%3?1:2,0x173a63,255); }
    int ah=(clk_h%12)*256/12+clk_m*256/720, am=clk_m*256/60;
    line(cx,cy,cx+isin(ah)*(R*5/10)/1024,cy-icos(ah)*(R*5/10)/1024,3,0x10243e,255);
    line(cx,cy,cx+isin(am)*(R*8/10)/1024,cy-icos(am)*(R*8/10)/1024,2,0x10243e,255);
    circle(cx,cy,3,0xd23a20,255);
    int X0,Y0,X1,Y1; clip_get(&X0,&Y0,&X1,&Y1);                    /* brillo de cristal en la mitad de arriba */
    clip_set(cx-R,cy-R,2*R,R*9/10); circle(cx,cy-R/6,R*9/10,0xffffff,70); clip_set(X0,Y0,X1-X0,Y1-Y0);
}

/* ---------------- marco de las ventanas segun el tema ---------------- */
static int ctl_hw=-1, ctl_hk=-1;                                   /* boton de ventana bajo el raton (ventana, boton) */
static void ctl_geom(const Win *w,int k,int *x,int *y,int *cw,int *ch){   /* k: 0 cerrar, 1 maximizar, 2 minimizar */
    if(ESTILO==2){ int pos=k==0?0:(k==2?1:2); *cw=*ch=13; *x=w->x+13+pos*20; *y=w->y+(TB-13)/2; return; }
    if(ESTILO==1){ int pos=k; *cw=46; *ch=TB-1; *x=w->x+w->w-1-46*(pos+1); *y=w->y+1; return; }
    *ch=19; *y=w->y+5;
    if(k==0){ *cw=42; *x=w->x+w->w-8-42; } else { *cw=26; *x=w->x+w->w-8-42-26*k; }
}
static int ctl_hit(const Win *w,int px,int py){
    for(int k=0;k<3;k++){ int x,y,cw,ch; ctl_geom(w,k,&x,&y,&cw,&ch);
        if(ESTILO==2){ if(in_rect(px,py,x-3,y-4,cw+7,ch+8)) return k; }
        else if(in_rect(px,py,x,y,cw,ch)) return k; }
    return -1;
}
static void ctl_draw(Win *w,int k,int f){
    int x,y,cw,ch; ctl_geom(w,k,&x,&y,&cw,&ch);
    int wi=(int)(w-wins), hv=ctl_hw==wi&&ctl_hk==k, grp=ctl_hw==wi, dis=k==1&&!win_resizable(w);
    if(ESTILO==2){
        static const uint32_t MC[3]={0xff5f57,0x28c840,0xfebc2e};
        uint32_t c=(f||grp)&&!dis?MC[k]:0x58585a; int cx=x+6, cy=y+6;
        circle(cx,cy,6,mix(c,0x000000,70),255); circle(cx,cy,5,c,255);
        if(grp&&!dis){ uint32_t g=0x4a1410; if(k==0){ line(cx-2,cy-2,cx+2,cy+2,1,g,220); line(cx+2,cy-2,cx-2,cy+2,1,g,220); }
            else if(k==2) fill(cx-3,cy,7,1,g,220); else { fill(cx-3,cy,7,1,g,220); fill(cx,cy-3,1,7,g,220); } }
        return; }
    if(ESTILO==1){
        if(hv&&!dis) fill(x,y,cw,ch,k==0?0xc42b1c:0xffffff,k==0?255:22);
        uint32_t g=dis?0x5a5a5a:(f||hv?0xffffff:0x9a9a9a); int cx=x+cw/2, cy=y+ch/2;
        if(k==0){ line(cx-5,cy-5,cx+5,cy+5,1,g,255); line(cx+5,cy-5,cx-5,cy+5,1,g,255); }
        else if(k==1){ if(w->maxed){ fill(cx-3,cy-5,8,1,g,255); fill(cx+4,cy-5,1,8,g,255); }
            int o=w->maxed?2:0, s=w->maxed?8:10; fill(cx-5,cy-5+o,s,1,g,255); fill(cx-5,cy+4,s,1,g,255); fill(cx-5,cy-5+o,1,10-o,g,255); fill(cx-5+s-1,cy-5+o,1,10-o,g,255); }
        else fill(cx-5,cy,10,1,g,255);
        return; }
    uint32_t c0,c1;                                               /* Aero: botones de vidrio con brillo */
    if(k==0){ c0=hv?0xffa08c:0xe8735f; c1=hv?0xe0401e:0xb02814; } else { c0=hv?0xe4f6ff:0xbfe2f8; c1=hv?0x58a8ec:0x3b7fc0; }
    if(dis){ c0=0xb8c8d6; c1=0x7d93a8; }
    else if(!f&&!hv){ c0=mix(c0,0xa9bccc,160); c1=mix(c1,0x7590aa,160); }
    rrect(x,y,cw,ch,4,0x0b2e55,210); rrect_grad(x+1,y+1,cw-2,ch-2,3,c0,c1,255);
    clip_set(x,y,cw,ch/2); rrect(x+1,y+1,cw-2,ch-2,3,0xffffff,70); clip_reset();
    int cx=x+cw/2, cy=y+ch/2; uint32_t g=0xffffff;
    if(k==0){ line(cx-4,cy-4+1,cx+4,cy+4+1,2,0x5a1006,120); line(cx+4,cy-4+1,cx-4,cy+4+1,2,0x5a1006,120); line(cx-4,cy-4,cx+4,cy+4,2,g,255); line(cx+4,cy-4,cx-4,cy+4,2,g,255); }
    else if(k==1){ fill(cx-5,cy-4,10,3,g,240); fill(cx-5,cy-4,1,8,g,240); fill(cx+4,cy-4,1,8,g,240); fill(cx-5,cy+3,10,1,g,240); }
    else fill(cx-5,cy+2,10,3,g,240);
}
static void chrome_draw(Win *w,int f){
    int x=w->x, y=w->y, W=w->w, H=w->h, r=TM->rad, e=ESTILO;
    { int X0=x-10,Y0=y-6,X1=x+W+10,Y1=y+H+16;                      /* sombra suave, solo fuera de la ventana */
      int n=f?4:3, al=e==2?(f?24:15):(f?19:12);
      for(int bnd=0;bnd<4;bnd++){
          if(bnd==0) clip_set(X0,Y0,X1-X0,y-Y0); else if(bnd==1) clip_set(X0,y+H,X1-X0,Y1-(y+H));
          else if(bnd==2) clip_set(X0,y,x-X0,H); else clip_set(x+W,y,X1-(x+W),H);
          for(int i=n;i>=1;i--) rrect(x-i*2,y-i*2+5,W+4*i,H+4*i,r+i*2,0x000000,al);
          if(e==0&&f) rrect(x-2,y-2,W+4,H+4,r+2,0xbfe6ff,40);          /* Aero: halo de luz */
      }
      clip_reset(); }
    if(e==0){ int F=5;                                             /* Aero: marco grueso de vidrio alrededor de la ventana */
        for(int bnd=0;bnd<4;bnd++){
            if(bnd==0) clip_set(x-F,y-F,W+2*F,F); else if(bnd==1) clip_set(x-F,y+H,W+2*F,F);
            else if(bnd==2) clip_set(x-F,y,F,H); else clip_set(x+W,y,F,H);
            blur_rect(x-F,y-F,W+2*F,H+2*F,4);
            rrect_grad(x-F,y-F,W+2*F,H+2*F,r+F,f?0xc6ebff:0xb4cde0,f?0x2f7fd0:0x7393b4,f?175:125);
            rrect(x-F,y-F,W+2*F,H+2*F,r+F,0xffffff,f?40:20); }
        clip_reset(); }
    uint32_t bord=f?(e==0?0x1b5a98:e==1?mix(0x3a3a3a,ACCENT,50):0x5c5c5e):TM->border;
    clip_set(x,y+TB,W,H-TB); rrect(x,y,W,H,r,bord,255);
    if(e==0) rrect_grad(x+1,y+1,W-2,H-2,r-1,0x1d3a60,0x0e1b31,255); else rrect(x+1,y+1,W-2,H-2,r-1,C_BG,255);      /* Aero: fondo con degradado */
    clip_reset();
    clip_set(x,y,W,TB);
    int ta=f?TM->ta:TM->ta-40;
    if(e==0){
        blur_rect(x,y,W,TB,5);                                     /* vidrio esmerilado: lo de atras se ve borroso */
        rrect_grad(x,y,W,TB+2*r,r,f?0xb4e2ff:0xa6c8de,f?0x2a7cd0:0x5d88ad,ta);
        clip_set(x,y,W,TB/2); rrect(x+1,y+1,W-2,TB,r-1,0xffffff,f?72:40); clip_set(x,y,W,TB);
        fill(x+r,y+1,W-2*r,1,0xffffff,160); fill(x,y+r,1,TB-r,bord,200); fill(x+W-1,y+r,1,TB-r,bord,200);
    } else if(e==1){
        rrect(x,y,W,TB+2*r,r,bord,255); { uint32_t c=f?0x2b2b2e:0x1f1f21; rrect_grad(x+1,y+1,W-2,TB+2*r,r-1,c,c,ta); }
    } else {
        rrect(x,y,W,TB+2*r,r,bord,255); rrect_grad(x+1,y+1,W-2,TB+2*r,r-1,f?0x404042:0x2e2e30,f?0x2c2c2e:0x262628,ta);
        fill(x+r,y+1,W-2*r,1,0xffffff,f?34:12);
    }
    clip_reset();
    fill(x+1,y+TB-1,W-2,1,e==0?0x0b2e55:0x000000,e==0?150:110);
    for(int k=0;k<3;k++) ctl_draw(w,k,f);
}
static void chrome_title(Win *w,int f){
    int x=w->x, y=w->y, W=w->w, e=ESTILO, tx, lim;
    if(e==2){ int tw=text_w(w->title)+(w->type==A_ABOUT?26:0); tx=x+(W-tw)/2; if(tx<x+78) tx=x+78; lim=x+W-10; }
    else { int bx,by,bw,bh; ctl_geom(w,2,&bx,&by,&bw,&bh); tx=x+14; lim=bx-6; }
    if(w->type==A_ABOUT){ crown(tx-2,y+9,18,0xe9c46a,ACCENT,255); tx+=24; }
    else if(w->type==A_XWIN&&w->xicon&&e!=2){ blit_pm_scaled(tx-2,y+(TB-18)/2,18,18,w->xicon,w->xiw,w->xih,255); tx+=22; }
    clip_set(x+1,y+1,lim-x-1,TB-1);
    if(e==0){ int tw=text_w(w->title)+1; rrect(tx-8,y+6,tw+16,18,9,0xffffff,f?80:40); blur_rect(tx-12,y+2,tw+24,TB-4,4);   /* resplandor detras del titulo */
        text_bold(tx+1,y+8,w->title,0x08294d); text_bold(tx,y+7,w->title,f?0xffffff:0xe0ecf6); }
    else if(e==1) text(tx,y+7,w->title,f?0xffffff:0x9a9a9a);
    else text_bold(tx,y+7,w->title,f?0xe6e6e6:0x8a8a8c);
    clip_reset();
    if(win_resizable(w)&&!w->maxed){ int gx=x+W-4, gy=y+w->h-4;          /* agarradera para cambiar el tamano */
        for(int i=0;i<3;i++) for(int j=0;j<=i;j++) fill(gx-(i-j)*4-2,gy-j*4-2,2,2,C_DIM,150); }
}

/* ---------------- animaciones ---------------- */
#define A_ABRIR 16
#define A_CERRAR 14
#define A_MINI 26
#define A_RECT 14
typedef struct { int on; uint32_t t0; int dur, mode; uint32_t *buf; int bw,bh, x0,y0,w0,h0, x1,y1,w1,h1, a0,a1; } Ghost;
#define NGH 4
static Ghost ghs[NGH];
static void ghost_cur(const Ghost *g,int *x,int *y,int *w,int *h,int *a){
    int t=(int)(kticks()-g->t0)*256/g->dur; int e=g->mode?ease_in(t):ease_out(t);
    *x=g->x0+(g->x1-g->x0)*e/256; *y=g->y0+(g->y1-g->y0)*e/256; *w=g->w0+(g->w1-g->w0)*e/256; *h=g->h0+(g->h1-g->h0)*e/256; *a=g->a0+(g->a1-g->a0)*e/256;
}
static void ghost_mark(const Ghost *g){
    int ax=k_min(g->x0,g->x1), ay=k_min(g->y0,g->y1), bx=k_max(g->x0+g->w0,g->x1+g->w1), by=k_max(g->y0+g->h0,g->y1+g->h1);
    dmark(ax-2,ay-2,bx-ax+4,by-ay+4);
}
/* copia lo que hay en pantalla en (x,y,w,h) y lo anima hasta (x1,y1,w1,h1) con opacidad a0 -> a1 */
static void ghost_new(int x,int y,int w,int h,int x1,int y1,int w1,int h1,int a0,int a1,int dur,int mode){
    if(x<0){ w+=x; x=0; } if(y<0){ h+=y; y=0; } if(x+w>gw) w=gw-x; if(y+h>gh) h=gh-y;
    if(w<8||h<8) return;
    int s=-1; for(int i=0;i<NGH;i++) if(!ghs[i].on){ s=i; break; }
    if(s<0) return;
    Ghost *g=&ghs[s]; g->buf=kmalloc((size_t)w*h*4); if(!g->buf) return;
    for(int j=0;j<h;j++) memcpy(g->buf+j*w,bb+(y+j)*gw+x,(size_t)w*4);
    g->bw=w; g->bh=h; g->x0=x; g->y0=y; g->w0=w; g->h0=h; g->x1=x1; g->y1=y1; g->w1=k_max(1,w1); g->h1=k_max(1,h1);
    g->a0=a0; g->a1=a1; g->dur=dur; g->mode=mode; g->t0=kticks(); g->on=1; ghost_mark(g);
}
static void ghosts_draw(void){
    int X0,Y0,X1,Y1; clip_get(&X0,&Y0,&X1,&Y1);
    for(int i=0;i<NGH;i++){ Ghost *g=&ghs[i]; if(!g->on) continue;
        int x,y,w,h,a; ghost_cur(g,&x,&y,&w,&h,&a); if(a<=0||w<=0||h<=0) continue; if(a>255) a=255;
        int i0=k_max(x,X0), i1=k_min(x+w,X1), j0=k_max(y,Y0), j1=k_min(y+h,Y1);
        for(int j=j0;j<j1;j++){ int sy=(j-y)*g->bh/h; uint32_t *d=bb+j*gw; const uint32_t *s=g->buf+sy*g->bw;
            for(int q=i0;q<i1;q++) d[q]=mix(d[q],s[(q-x)*g->bw/w],a); } }
}
static int anim_tick(void){                                        /* avanza las animaciones; 1 si hay alguna activa */
    uint32_t t=kticks(); int act=0;
    for(int i=0;i<NGH;i++){ Ghost *g=&ghs[i]; if(!g->on) continue; ghost_mark(g); act=1;
        if((int)(t-g->t0)>=g->dur){ g->on=0; kfree(g->buf); g->buf=0; } }
    for(int k=0;k<nord;k++){ Win *w=&wins[order[k]];
        if(w->aopen){ dwin(w); act=1; if((int)(t-w->at0)>=A_ABRIR) w->aopen=0; }
        if(w->vanim){ dwin(w); dmark(w->vx0-14,w->vy0-10,w->vw0+28,w->vh0+32); act=1; if((int)(t-w->vt0)>=A_RECT) w->vanim=0; } }
    if(bnc_i>=0){ dmark_dock(); act=1; if(t-bnc_t>=70) bnc_i=-1; }
    return act;
}
static void win_vis(const Win *w,int *x,int *y,int *ww,int *hh){   /* rectangulo que se ve (durante maximizar/restaurar) */
    *x=w->x; *y=w->y; *ww=w->w; *hh=w->h; if(!w->vanim) return;
    int e=ease_out((int)(kticks()-w->vt0)*256/A_RECT);
    *x=w->vx0+(w->x-w->vx0)*e/256; *y=w->vy0+(w->y-w->vy0)*e/256; *ww=w->vw0+(w->w-w->vw0)*e/256; *hh=w->vh0+(w->h-w->vh0)*e/256;
}
static void win_anim_from(Win *w){ int x,y,ww,hh; win_vis(w,&x,&y,&ww,&hh); w->vx0=x; w->vy0=y; w->vw0=ww; w->vh0=hh; w->vt0=kticks(); w->vanim=1; }
static void draw_window(int wi,int focused);
static void draw_window_anim(int wi,int focused){                 /* aparecer: de 92% a 100% de tamano y de transparente a opaca */
    Win *w=&wins[wi]; int t=(int)(kticks()-w->at0)*256/A_ABRIR;
    int X0,Y0,X1,Y1; clip_get(&X0,&Y0,&X1,&Y1);
    int bx=k_max(w->x-14,X0), by=k_max(w->y-10,Y0), ex=k_min(w->x+w->w+14,X1), ey=k_min(w->y+w->h+22,Y1);
    if(ex<=bx||ey<=by) return;
    int bw=ex-bx, bh=ey-by; uint32_t *un=kmalloc((size_t)bw*bh*4), *ov=un?kmalloc((size_t)bw*bh*4):0;
    if(!ov){ if(un) kfree(un); draw_window(wi,focused); return; }
    for(int j=0;j<bh;j++) memcpy(un+j*bw,bb+(by+j)*gw+bx,(size_t)bw*4);
    draw_window(wi,focused);
    for(int j=0;j<bh;j++) memcpy(ov+j*bw,bb+(by+j)*gw+bx,(size_t)bw*4);
    int e=ease_out(t), a=e*255/256, sc=236+20*e/256, cx=w->x+w->w/2, cy=w->y+w->h/2;
    int wx0=k_max(w->x,bx), wy0=k_max(w->y,by), wx1=k_min(w->x+w->w,ex), wy1=k_min(w->y+w->h,ey);
    for(int j=0;j<bh;j++){ int sy=cy+((by+j)-cy)*256/sc; uint32_t *d=bb+(by+j)*gw+bx; const uint32_t *u=un+j*bw;
        int rowin=sy>=wy0&&sy<wy1; const uint32_t *o=ov+(sy-by)*bw;
        for(int i=0;i<bw;i++){ uint32_t c=u[i];
            if(rowin){ int sx=cx+((bx+i)-cx)*256/sc; if(sx>=wx0&&sx<wx1) c=mix(c,o[sx-bx],a); }
            d[i]=c; } }
    kfree(un); kfree(ov);
}
static void win_close(int wi);
static void win_close_anim(int wi){                                /* cerrar: se encoge y se desvanece */
    Win *w=&wins[wi]; int dx=w->w*4/100, dy=w->h*4/100;
    ghost_new(w->x,w->y,w->w,w->h,w->x+dx,w->y+dy,w->w-2*dx,w->h-2*dy,255,0,A_CERRAR,0);
    win_close(wi);
}
static void win_minimize(int wi){                                  /* minimizar: vuela hacia su icono del dock */
    Win *w=&wins[wi]; int tx=gw/2, ty=gh-30, ts=40;
    dock_layout(); for(int i=0;i<dock_n;i++) if(i<NDOCK?DOCK_APP[i]==w->type:dock_xw[i-NDOCK]==wi){ tx=dsl[i].x+dsl[i].s/2; ty=dsl[i].y+dsl[i].s/2; ts=dsl[i].s; }
    int th=ts*w->h/k_max(1,w->w); if(th<8) th=8;
    ghost_new(w->x,w->y,w->w,w->h,tx-ts/2,ty-th/2,ts,th,255,40,A_MINI,1);
    w->minim=1; w->aopen=0;
    int k=0; while(k<nord&&order[k]!=wi) k++;
    for(;k>0;k--) order[k]=order[k-1];
    order[0]=wi; dirty=1;
}

/* ---------------- tarjetas para elegir el tema (Ajustes > Pantalla) ---------------- */
static void tema_card(int i,int x,int y,int sel){
    const Tema *T=&TEMAS[i]; int W=180, H=104;
    if(sel) rrect(x-3,y-3,W+6,H+6,10,ACCENT,255);
    rrect(x,y,W,H,8,0x000000,255);
    { uint32_t *th=get_thumb(T->fondo); if(th) for(int yy=1;yy<H-1;yy++) for(int xx=1;xx<W-1;xx++) px_blend(x+xx,y+yy,th[(yy*THH/H)*THW+xx*THW/W],255); }
    int wx=x+28, wy=y+18, ww=124, wh=62, e=T->estilo;
    if(e!=1){ fill(x+1,y+1,W-2,7,e==0?0x7cc4ff:0x1c1c1e,e==0?170:190); }
    rrect(wx,wy,ww,wh,e==2?6:4,T->bg,255);
    clip_set(wx,wy,ww,12);
    if(e==0){ rrect_grad(wx,wy,ww,24,4,0xb4e2ff,0x2a7cd0,200); clip_set(wx,wy,ww,6); rrect(wx,wy,ww,24,4,0xffffff,70); }
    else rrect(wx,wy,ww,24,e==2?6:4,e==1?0x2b2b2e:0x3a3a3c,255);
    clip_reset();
    if(e==2){ circle(wx+8,wy+6,3,0xff5f57,255); circle(wx+17,wy+6,3,0xfebc2e,255); circle(wx+26,wy+6,3,0x28c840,255); }
    else if(e==1){ fill(wx+ww-12,wy+3,7,6,0xc42b1c,255); fill(wx+ww-24,wy+5,6,1,0xffffff,200); }
    else { rrect(wx+ww-20,wy+2,16,8,2,0xd0442a,255); rrect(wx+ww-31,wy+2,10,8,2,0x8cc4ee,255); }
    fill(wx+8,wy+20,60,4,T->txt,120); fill(wx+8,wy+30,90,4,T->dim,110); fill(wx+8,wy+40,74,4,T->dim,110);
    if(e==1){ fill(x+1,y+H-14,W-2,13,0x1b1b1f,220); for(int k=0;k<5;k++) rrect(x+W/2-34+k*14,y+H-11,9,8,2,k==0?0x60a5fa:0xd0d0d0,230); }
    else { int dw=e==0?84:92; rrect(x+(W-dw)/2,y+H-17,dw,13,4,e==0?0x2c86d8:0x262628,e==0?170:190);
        for(int k=0;k<6;k++) rrect(x+(W-dw)/2+5+k*14,y+H-15,9,9,3,k==0?0xe9c46a:k==1?0xf97316:k==2?0xf5b83d:0x9aa6c8,255); }
    text(x+(W-text_w(T->nombre))/2,y+H+8,T->nombre,sel?0xffffff:C_TXT);
}

/* ---------------- usuario y contrasena (opcional) ---------------- */
static const uint32_t SHK[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,
    0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,
    0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha_blk(uint32_t *h,const unsigned char *p){
    uint32_t w[64]; for(int i=0;i<16;i++) w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for(int i=16;i<64;i++){ uint32_t s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3), s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],k=h[7];
    for(int i=0;i<64;i++){ uint32_t t1=k+(ROR(e,6)^ROR(e,11)^ROR(e,25))+((e&f)^(~e&g))+SHK[i]+w[i], t2=(ROR(a,2)^ROR(a,13)^ROR(a,22))+((a&b)^(a&c)^(b&c));
        k=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2; }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=k;
}
static void sha256_hex(const char *m,char *out){                   /* out: 65 bytes */
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    int n=k_strlen(m), i=0; unsigned char b[64];
    for(;i+64<=n;i+=64) sha_blk(h,(const unsigned char*)m+i);
    int r=n-i; memcpy(b,m+i,(size_t)r); b[r++]=0x80;
    if(r>56){ while(r<64) b[r++]=0; sha_blk(h,b); r=0; }
    while(r<56) b[r++]=0;
    uint32_t bits=(uint32_t)n*8; b[56]=b[57]=b[58]=0; b[59]=(unsigned char)(n>>29); b[60]=(unsigned char)(bits>>24); b[61]=(unsigned char)(bits>>16); b[62]=(unsigned char)(bits>>8); b[63]=(unsigned char)bits;
    sha_blk(h,b);
    static const char HX[]="0123456789abcdef"; for(int j=0;j<32;j++){ unsigned v=(h[j/4]>>(24-(j%4)*8))&255; out[j*2]=HX[v>>4]; out[j*2+1]=HX[v&15]; } out[64]=0;
}
static char cu_user[28];                                           /* usuario configurado ("" = sin usuario: entra directo) */
static void clave_hash(const char *sal,const char *pw,char *out){
    char m[200]=""; cat(m,sizeof m,"kng:"); cat(m,sizeof m,sal); cat(m,sizeof m,":"); cat(m,sizeof m,pw);
    sha256_hex(m,out);
    for(int i=0;i<999;i++){ char m2[140]=""; cat(m2,sizeof m2,out); cat(m2,sizeof m2,sal); sha256_hex(m2,out); }   /* mas lento de adivinar */
}
static int clave_ok(const char *pw){
    char sal[40], h[72], c[72]; if(!cfg_get("sal",sal,sizeof sal)||!cfg_get("clave",h,sizeof h)) return 0;
    clave_hash(sal,pw,c); return !k_strcmp(c,h);
}
static unsigned rnd_mix;
static void cuenta_crear(const char *user,const char *pw){
    static const char HX[]="0123456789abcdef"; char sal[17];
    unsigned v=rnd_mix^(unsigned)kticks()*2654435761u^(unsigned)(mx*7919+my*104729);
    for(int i=0;i<16;i++){ v=v*1103515245u+12345u+(unsigned)kticks(); sal[i]=HX[(v>>16)&15]; } sal[16]=0;
    char h[72]; clave_hash(sal,pw,h);
    cfg_set("usuario",user); cfg_set("sal",sal); cfg_set("clave",h); k_strcpy(cu_user,user,sizeof cu_user);
    (void)kngfs_save();
}
static void cuenta_quitar(void){ cfg_set("usuario",""); cfg_set("sal",""); cfg_set("clave",""); cu_user[0]=0; (void)kngfs_save(); }

/* campos de texto de Ajustes > Cuenta y de la pantalla de bloqueo */
typedef struct { char s[40]; int n; } Campo;
static Campo cf[4]; static int cf_sel=-1; static char cf_msg[64]; static uint32_t cf_msgc;
static Campo lk_pw; static int lk_err; static uint32_t lk_shake;
static void campo_key(Campo *c,int k,int ascii_only){
    if(k=='\b'){ if(c->n>0){ c->n-=u8_last(c->s,c->n); c->s[c->n]=0; } return; }
    if(k<32||k==127||k>=0x400) return;
    if(ascii_only&&!((k>='a'&&k<='z')||(k>='A'&&k<='Z')||(k>='0'&&k<='9')||k=='_'||k=='-'||k=='.')) return;
    char e[2]; int en=u8_enc(k,e); if(!en||c->n+en>=(int)sizeof c->s-1) return;
    for(int i=0;i<en;i++){ c->s[c->n++]=e[i]; } c->s[c->n]=0;
}
static void campo_draw(int x,int y,int w,const Campo *c,int sel,int oculto,const char *ph){
    rrect(x,y,w,30,7,sel?ACCENT:0xffffff,sel?255:40); rrect(x+1,y+1,w-2,28,6,0x0b0c14,235);
    char b[44]; if(oculto){ int n=0; for(int i=0;i<c->n&&n<40;i++) if(((unsigned char)c->s[i]&0xC0)!=0x80) b[n++]='*'; b[n]=0; } else k_strcpy(b,c->s,sizeof b);
    int ex;
    if(!b[0]&&!sel) ex=text(x+10,y+7,ph,0x6b7088); else ex=text(x+10,y+7,b,0xffffff);
    if(sel&&((kticks()/50)&1)==0) fill(b[0]?ex+1:x+10,y+7,2,16,0xffffff,220);
}
static void cuenta_draw(int cx,int oy,int cw){
    (void)cw;
    text_bold(cx+20,oy+12,"Inicio de sesion",C_TXT);
    if(!cu_user[0]){
        text(cx+20,oy+38,"Ahora Konigreich OS entra directo al encender, sin pedir nada.",C_DIM);
        text(cx+20,oy+58,"Crea un usuario para que pida contrasena al encender.",C_DIM);
        text(cx+20,oy+92,"Usuario",C_TXT); campo_draw(cx+20,oy+112,280,&cf[0],cf_sel==0,0,"nombre (letras y numeros)");
        text(cx+20,oy+154,"Contrasena",C_TXT); campo_draw(cx+20,oy+174,280,&cf[1],cf_sel==1,1,"contrasena");
        text(cx+20,oy+216,"Repite la contrasena",C_TXT); campo_draw(cx+20,oy+236,280,&cf[2],cf_sel==2,1,"otra vez");
        button(cx+20,oy+284,170,30,"Crear usuario",1);
    } else {
        circle(cx+52,oy+78,28,ACCENT,255); { char in[3]={cu_user[0]>='a'&&cu_user[0]<='z'?(char)(cu_user[0]-32):cu_user[0],0}; text_big(cx+46,oy+66,in,0xffffff); }
        text_big(cx+96,oy+56,cu_user,0xffffff); text(cx+96,oy+86,"Pide contrasena al encender y al bloquear.",C_DIM);
        button(cx+20,oy+124,170,30,"Bloquear ahora",1);
        text(cx+20,oy+174,"Para cambiar o quitar la contrasena escribe la actual:",C_DIM);
        campo_draw(cx+20,oy+196,280,&cf[0],cf_sel==0,1,"contrasena actual");
        text(cx+20,oy+236,"Nueva contrasena (vacia = no cambiar):",C_DIM);
        campo_draw(cx+20,oy+256,280,&cf[1],cf_sel==1,1,"nueva contrasena");
        button(cx+20,oy+300,170,30,"Cambiar contrasena",0); button(cx+200,oy+300,200,30,"Quitar usuario",0);
    }
    if(cf_msg[0]) text(cx+20,oy+344,cf_msg,cf_msgc);
}
static void cuenta_msg(const char *m,uint32_t c){ k_strcpy(cf_msg,m,sizeof cf_msg); cf_msgc=c; dirty=1; }
static void cuenta_limpiar(void){ memset(cf,0,sizeof cf); cf_sel=-1; }
static void bloquear(void);
static void cuenta_accion(int b){                                   /* b: 0 crear/cambiar, 1 quitar, 2 bloquear */
    if(!cu_user[0]){
        if(cf[0].n<1){ cuenta_msg("Escribe un nombre de usuario.",0xffb86b); return; }
        if(cf[1].n<1){ cuenta_msg("Escribe una contrasena.",0xffb86b); return; }
        if(k_strcmp(cf[1].s,cf[2].s)){ cuenta_msg("Las contrasenas no coinciden.",0xff7b72); return; }
        cuenta_crear(cf[0].s,cf[1].s); cuenta_limpiar(); cuenta_msg("Listo: se pedira la contrasena al encender.",0x7ee787); return; }
    if(b==2){ bloquear(); return; }
    if(!clave_ok(cf[0].s)){ cuenta_msg("La contrasena actual no es correcta.",0xff7b72); return; }
    if(b==1){ cuenta_quitar(); cuenta_limpiar(); cuenta_msg("Usuario quitado: el sistema entra directo.",0x7ee787); return; }
    if(cf[1].n<1){ cuenta_msg("Escribe la nueva contrasena.",0xffb86b); return; }
    { char u[28]; k_strcpy(u,cu_user,sizeof u); cuenta_crear(u,cf[1].s); }
    cuenta_limpiar(); cuenta_msg("Contrasena cambiada.",0x7ee787);
}
static void cuenta_click(int lx,int y){
    cf_msg[0]=0;
    if(!cu_user[0]){
        for(int i=0;i<3;i++) if(in_rect(lx,y,20,112+i*62,280,30)){ cf_sel=i; dirty=1; return; }
        if(in_rect(lx,y,20,284,170,30)) cuenta_accion(0);
    } else {
        if(in_rect(lx,y,20,124,170,30)){ cuenta_accion(2); return; }
        if(in_rect(lx,y,20,196,280,30)){ cf_sel=0; dirty=1; return; }
        if(in_rect(lx,y,20,256,280,30)){ cf_sel=1; dirty=1; return; }
        if(in_rect(lx,y,20,300,170,30)) cuenta_accion(0);
        else if(in_rect(lx,y,200,300,200,30)) cuenta_accion(1);
    }
    dirty=1;
}
static int cuenta_key(int c){                                       /* 1 si la tecla fue para un campo */
    if(cf_sel<0) return 0;
    if(c=='\t'){ cf_sel=(cf_sel+1)%(cu_user[0]?2:3); dirty=1; return 1; }
    if(c=='\n'){ if(!cu_user[0]&&cf_sel<2){ cf_sel++; dirty=1; } else cuenta_accion(0); return 1; }
    if(c==27){ cf_sel=-1; dirty=1; return 1; }
    campo_key(&cf[cf_sel],c,!cu_user[0]&&cf_sel==0); dirty=1; return 1;
}

/* ---------------- pantalla de bloqueo / inicio de sesion ---------------- */
static void bloquear(void){ if(!cu_user[0]) return; bloqueado=1; memset(&lk_pw,0,sizeof lk_pw); lk_err=0; mu_open=0; drag=-1; dirty=1; }
static void lock_geom(int *fx,int *fy){ *fx=gw/2-150; *fy=gh*58/100; }
static void lock_draw(void){
    fill(0,0,gw,gh,0x000000,ESTILO==0?70:110);
    char t[16]; clock_str(t);
    int H=gh/7; if(H>120) H=120; if(H<48) H=48;
    text_scaled((gw-text_scaled_w(t,H))/2+2,gh*14/100+3,t,0x000000,H); text_scaled((gw-text_scaled_w(t,H))/2,gh*14/100,t,0xffffff,H);
    text((gw-text_w("Königreich OS"))/2,gh*14/100+H+10,"Königreich OS",0xdde3f0);
    int fx,fy; lock_geom(&fx,&fy);
    int ay=fy-130; circle(gw/2,ay+44,48,0xffffff,60); circle(gw/2,ay+44,44,ACCENT,255);
    { char in[3]={cu_user[0]>='a'&&cu_user[0]<='z'?(char)(cu_user[0]-32):cu_user[0],0}; text_scaled(gw/2-14,ay+44-28,in,0xffffff,56); }
    text_big(gw/2-text_w_big(cu_user)/2,ay+102,cu_user,0xffffff);
    int sh=0; { int e=(int)(kticks()-lk_shake); if(lk_shake&&e<40) sh=isin(e*256/10)*10/1024*(40-e)/40; }
    int x=fx+sh;
    rrect(x,fy,300,38,19,0xffffff,ESTILO==1?0:70); rrect(x+1,fy+1,298,36,18,0x10131f,ESTILO==1?220:150);
    if(ESTILO==1) fill(x+12,fy+36,276,2,ACCENT,255);
    char b[44]; int n=0; for(int i=0;i<lk_pw.n&&n<30;i++) if(((unsigned char)lk_pw.s[i]&0xC0)!=0x80) b[n++]='*'; b[n]=0;
    int ex=n?text(x+18,fy+11,b,0xffffff):text(x+18,fy+11,"Contrasena",0x8a90a8);
    if(((kticks()/50)&1)==0) fill(n?ex+1:x+18,fy+11,2,16,0xffffff,220);
    circle(x+300+26,fy+19,17,ACCENT,255); line(x+318,fy+19,x+333,fy+19,2,0xffffff,255); line(x+327,fy+13,x+333,fy+19,2,0xffffff,255); line(x+327,fy+25,x+333,fy+19,2,0xffffff,255);
    if(lk_err) text(gw/2-text_w("Contrasena incorrecta")/2,fy+52,"Contrasena incorrecta",0xff8a80);
    else text(gw/2-text_w("Escribe tu contrasena y pulsa Intro")/2,fy+52,"Escribe tu contrasena y pulsa Intro",0xc8cde0);
}
static void lock_try(void){
    if(clave_ok(lk_pw.s)){
        ghost_new(0,0,gw,gh,-gw/40,-gh/40,gw+gw/20,gh+gh/20,255,0,22,0);
        bloqueado=0; memset(&lk_pw,0,sizeof lk_pw); lk_err=0; dirty=1;
    } else { lk_err=1; lk_shake=kticks(); if(!lk_shake) lk_shake=1; memset(&lk_pw,0,sizeof lk_pw); dirty=1; }
}
static void lock_key(int c){
    if(c=='\n'){ lock_try(); return; }
    if(c==27){ memset(&lk_pw,0,sizeof lk_pw); dirty=1; return; }
    campo_key(&lk_pw,c,0); lk_err=0; dirty=1;
}
static void lock_press(void){ int fx,fy; lock_geom(&fx,&fy); int dx=mx-(fx+326), dy=my-(fy+19); if(dx*dx+dy*dy<=19*19) lock_try(); }
