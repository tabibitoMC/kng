#ifndef HTML_H
#define HTML_H
#include "kng.h"
#define HTML_MAXLINKS 400
typedef struct { int x, y, w, h, href; } HLink;                 /* coordenadas de pagina */
typedef struct {
    HLink links[HTML_MAXLINKS]; int nlinks;
    char hrefs[64][192]; int nhrefs;
    char title[48]; int content_h;
} HPage;
/* Dibuja (o solo mide si draw=0) un subconjunto de HTML: h1 h2 p br b a ul li hr pre title.
 * Area de vista (x,y,w,h) en pantalla; scroll en pixeles. */
void html_render(const char *html, int len, int x, int y, int w, int h, int scroll, uint32_t link_col, int draw, HPage *out);
const char *html_builtin(const char *url, int *len);           /* paginas kng:// integradas; 0 si no existe */
#endif
