#ifndef FS_H
#define FS_H
#include "kng.h"
#define FS_NAME 96
typedef struct FsNode {
    char name[FS_NAME]; int dir; struct FsNode *parent, *child, *next; char *data; int size; int sys; int dev;
    int cap;          /* bytes reservados en 'data' (>= size+1 si el bloque es nuestro) */
    int ro;           /* 1: 'data' no es nuestro (memoria del rootfs): se copia antes de escribir */
    void *shm;        /* si no es 0: 'data' pertenece a un objeto de memoria compartida (mmap compartido) */
    char *link;       /* si no es 0: enlace simbolico a esta ruta */
    int opens, dead;  /* abierto por programas de usuario; borrado pero aun abierto (se libera al cerrar el ultimo) */
} FsNode;
extern void (*fs_shm_hook)(void *shm);                                  /* libera la referencia al objeto compartido */
int     fs_write_at(FsNode *n, int off, const char *src, int len);     /* escribe en una posicion, creciendo con reserva; 0 ok */
int     fs_resize(FsNode *n, int size);                                 /* cambia el tamano (rellena con ceros); 0 ok */
FsNode *fs_symlink(FsNode *dir, const char *name, const char *target);
FsNode *fs_resolve_nf(FsNode *cwd, const char *path);                   /* como fs_resolve pero no sigue el enlace final */
void    fs_node_ref(FsNode *n);
void    fs_node_unref(FsNode *n);
void    fs_adopt(FsNode *n, const char *data, int len);                 /* el archivo apunta a datos ajenos (solo lectura, sin copiar) */
void    fs_init(void);
FsNode *fs_root(void);
FsNode *fs_find(FsNode *dir, const char *name);
FsNode *fs_mkdir(FsNode *dir, const char *name);                       /* 0 si ya existe o no hay memoria */
FsNode *fs_create(FsNode *dir, const char *name, const char *data, int len);   /* crea o sobrescribe */
int     fs_write(FsNode *n, const char *data, int len);
int     fs_remove(FsNode *n);                                           /* recursivo; 0 ok */
FsNode *fs_resolve(FsNode *cwd, const char *path);                      /* acepta /abs, rel, . y .. */
void    fs_path(FsNode *n, char *out, int max);
int     fs_list(FsNode *dir, FsNode **out, int max);                    /* carpetas primero, luego por nombre */
uint32_t fs_gen(void);                                                  /* sube con cada cambio */
int     fs_rename(FsNode *n, const char *name);
FsNode *fs_copy(FsNode *src, FsNode *dir, const char *name);            /* archivo o carpeta entera */
int     fs_move(FsNode *n, FsNode *dir, const char *name);
void    fs_usage(int *files, int *dirs, int *bytes);
int     fs_serialize(uint8_t *out, int max);                            /* bytes escritos, o -1 si no cabe */
int     fs_deserialize(const uint8_t *in, int len);                     /* 0 ok */
#endif
