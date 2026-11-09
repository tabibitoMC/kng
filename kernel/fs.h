#ifndef FS_H
#define FS_H
#include "kng.h"
typedef struct FsNode {
    char name[32]; int dir; struct FsNode *parent, *child, *next; char *data; int size; int sys;
} FsNode;
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
