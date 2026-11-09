# Traspaso: Königreich OS (kng) v19

Base: v18 (32 y 64 bits + red). Cambios de v19 (todo en kernel/gui.c, gfx.c, html.c, fs.c; ambos kernels comparten la interfaz):
- gfx: cobertura 4x4 con rutas rapidas (rrect/circle), realce suave del alfa del texto.
- Fondo con precision fija y tramado Bayer 4x4 (sin bandas); miniaturas de Ajustes en cache.
- Buscador: `kng://buscar?q=...`; palabras sin punto o con espacios en la barra => busqueda local (paginas del sistema + nombre/contenido de archivos) y enlace web si /Sistema/kng.cfg tiene `buscador=http://host/ruta?q=%s`. https no existe (sin TLS).
- Apps: Notas (A_NOTES), Calculadora (A_CALC, enteros), Snake (A_SNAKE); dock de 8 (NDOCK).
- Verificado en host: `make -C kernel hosttest`, `python3 tools/nettest.py`, `python3 tools/hostgui.py` (afirmaciones de buscador, notas, calculadora, snake). Ambos kernels compilan sin warnings; check-mb.py OK.
- NUNCA se ha arrancado en una VM desde este entorno (no hay QEMU/grub-mkrescue). Pendiente: procesos en 64 bits, TLS, disco, USB, teclado es, UEFI.
