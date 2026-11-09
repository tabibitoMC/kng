/* sock.h — sockets para programas de usuario (AF_INET TCP/UDP, AF_UNIX y bucle local). Los valores de retorno negativos son -errno. */
#ifndef SOCK_H
#define SOCK_H
#include <stdint.h>

#define SK_STREAM 1
#define SK_DGRAM  2
#define SK_AF_UNIX 1
#define SK_AF_INET 2

#define SK_POLLIN    0x001
#define SK_POLLOUT   0x004
#define SK_POLLERR   0x008
#define SK_POLLHUP   0x010
#define SK_POLLRDHUP 0x2000

void sock_init(void);
int  sock_create(int family,int type);                                   /* indice de socket o -errno */
int  sock_bind(int s,uint32_t ip,uint16_t port,const char *upath,int plen);
int  sock_listen(int s,int backlog);
int  sock_connect(int s,uint32_t ip,uint16_t port,const char *upath,int plen);   /* 0 hecho, -115 en curso (esperar sock_connecting), otro <0 error */
int  sock_connecting(int s);
int  sock_accept(int s,uint32_t *ip,uint16_t *port);                      /* indice nuevo o -11 si no hay nadie esperando */
int  sock_send(int s,const void *d,int n,uint32_t ip,uint16_t port,const char *upath,int plen,int have_dst);
int  sock_recv(int s,void *d,int n,int peek,uint32_t *ip,uint16_t *port);
int  sock_events(int s);
int  sock_shutdown(int s,int how);
void sock_ref(int s);
void sock_unref(int s);
int  sock_take_err(int s);
int  sock_names(int s,int peer,uint32_t *ip,uint16_t *port,char *upath,int *plen);   /* familia (1/2) o -errno */
int  sock_pair(int type,int *a,int *b);
void sock_set_reuse(int s,int v);
int  sock_peer(int s);
int  sock_type(int s);
int  sock_family(int s);
#endif
