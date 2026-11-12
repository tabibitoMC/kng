/* zip.h — archivos .zip reales (compatibles con Windows, Linux y macOS): comprimir con deflate y descomprimir (deflate o sin comprimir). */
#ifndef ZIP_H
#define ZIP_H
#include "fs.h"
/* Comprime 'src' (archivo o carpeta entera) en dir/name. Devuelve el nodo del .zip o 0 si falla (sin memoria). */
FsNode *zip_create(FsNode *src, FsNode *dir, const char *name);
/* Extrae el .zip 'zf' dentro de la carpeta 'dest'. Devuelve cuantos archivos extrajo, o -1 si el archivo no es un zip valido. */
int zip_extract(FsNode *zf, FsNode *dest);
#endif
