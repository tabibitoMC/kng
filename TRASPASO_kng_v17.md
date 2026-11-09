# Traspaso: Königreich OS (kng) v17 — SO propio, SIN Linux

Pega esto al otro Claude junto con `KonigreichOS_kng_v17.zip`.

## Contexto
- Nick (Lima, habla español, respuestas MUY breves). Su ingeniero no quiere kernel Linux. Desde v17 el proyecto es un SO aparte: nada de Debian/Linux/live-build. Identidad propia.
- Pedido: kernel propio, interfaz gráfica básica parecida (no calco) al escritorio de referencia, gestor de archivos, navegador, ajustes, terminal, arranque propio, gráficos y 32 y 64 bits (prioridad), periféricos reducidos. Se puede sacrificar cosas.
- Honestidad: todo el código es propio (teoría de OSDev/specs); no copiar código GPL de Linux. Fuente DejaVu Sans Mono (licencia libre) citada.
- Nick construye la ISO y prueba él; entregar zips versionados `kng_vN`. Si faltan tokens, parar y entregar.

## Contenido (kernel/, i386, Multiboot 1, kng-kernel 0.5)
- Kernel: GDT/TSS ring 3, IDT, PIC, PIT, teclado PS/2 (flechas), ratón PS/2, RTC, paginación, PMM bitmap, heap 64 MB, planificador expropiativo, ELF32, syscalls `int 0x80` (fork/exec/wait/brk…).
- Interfaz (gfx.c, gui.c, fs.c, html.c, font.h): framebuffer 1024x768x32 pedido a GRUB, escritorio, dock, ventanas, apps Archivos/Terminal/Navegador/Ajustes/Acerca de. FS en RAM.
- Dos ELF: `kng-kernel.elf` (video) y `kng-kernel-text.elf`.
- ISO: `tools/build-iso.sh` (GRUB BIOS, `grub-mkrescue -d /usr/lib/grub/i386-pc`), `Construir-ISO.cmd` (WSL).

## Estado de pruebas
- Verificado en host: compila sin avisos; `make -C kernel hosttest` (PMM, ELF, clone, brk) OK; `hostgui` dibuja el mismo código de la GUI y se revisaron capturas (`docs/`).
- NUNCA ejecutado: arranque real en GRUB/QEMU, negociación de video, ratón, ring 3, scheduler. PRIMER PASO: generar ISO, arrancar en VM BIOS/Legacy, corregir lo que falle. Sospechosos: validación del framebuffer (kmain), `mouse_init`, `isr_common` (boot.S), `schedule()`.

## Siguiente (orden)
1. Verificar arranque (arriba).
2. **x86_64**: modo largo, Multiboot2 o UEFI (GOP), reutilizar GUI (código C portable; quitar asm i386 y rep movsl).
3. Disco (ATA PIO → AHCI/virtio-blk) + FS persistente (FAT32 o propio).
4. Red (virtio-net/e1000, TCP mínimo) → navegador con HTTP.
5. Apps en ring 3 con API gráfica por syscalls.
6. USB HID, teclado es/ñ, drivers de video (Bochs/virtio-gpu) para cambiar resolución.

## Limitaciones conocidas
- Apps dentro del kernel; FS solo RAM; sin red; PS/2 únicamente; teclado US; comandos que bloquean (`sh`) congelan la terminal GUI; zona horaria por defecto UTC-5.
