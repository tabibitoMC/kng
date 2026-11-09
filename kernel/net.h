/* net.h — red de kng: controladores (e1000, RTL8139) + ARP/IPv4/ICMP/UDP/DHCP/DNS/TCP + cliente HTTP/1.0 */
#ifndef NET_H
#define NET_H
#include "kng.h"
int   net_init(void);                 /* busca tarjeta (PCI) e inicia DHCP; 0 ok, <0 no hay tarjeta compatible */
void  net_poll(void);                 /* hay que llamarla seguido (bucle principal) */
uint32_t net_mmio_addr(void);         /* direccion MMIO de la e1000 (0 si no hay) */
int   net_state(void);                /* 0 sin tarjeta, 1 configurando (DHCP), 2 lista */
void  net_info(char *out, int max);   /* resumen de una linea: tarjeta, IP... */
int   net_cmd(char *line);            /* 'red' 'ping' 'dns' 'wget'; devuelve 1 si el comando era de red */

/* HTTP (sin TLS: https:// no esta soportado). Una peticion a la vez. */
int         http_get(const char *url);   /* 0 iniciada; <0 error inmediato (ver http_error) */
int         http_poll(void);             /* 0 en curso, 1 lista, -1 error */
char       *http_take(int *len);         /* cuerpo (kmalloc, terminado en 0); el que llama lo libera con kfree */
int         http_is_html(void);          /* 1 si el contenido es HTML */
int         http_status(void);
const char *http_error(void);
const char *http_final_url(void);        /* URL final tras redirecciones */
void        http_cancel(void);
#endif
