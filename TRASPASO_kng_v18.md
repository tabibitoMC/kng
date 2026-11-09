# Traspaso: Königreich OS (kng) v18 — 32 y 64 bits + red

Pega esto al otro Claude junto con `KonigreichOS_kng_v18.zip`.

## Contexto
- Nick (Lima, español, respuestas MUY breves, poco chat). SO propio, SIN Linux/Debian (su ingeniero no quiere kernel Linux). Todo el código es propio; no copiar código GPL de Linux.
- Prioridades que dio: **32 y 64 bits a la vez** (no sustituir uno por otro), **red**. Pospuestos: USB, apps como procesos de usuario, disco/persistencia.
- Nick genera la ISO y prueba él (WSL en Windows). Zips versionados `kng_vN`. Si faltan tokens, parar y entregar lo avanzado. No afirmar que algo funciona sin haberlo probado.

## Estructura
- `kernel/` kernel de 32 bits (Multiboot, ring 3, ELF, fork/exec, syscalls) + código compartido: `gfx.c gui.c fs.c html.c net.c font.h`.
- `kernel64/` kernel de 64 bits: `boot64.S` (32→modo largo, tablas PAE 4 GB con páginas de 2 MB, ≥2 GB sin cache), `kernel64.c` (IDT64, PIC, PIT, PS/2, heap, GUI, red; **sin procesos**), `Makefile` (enlaza ELF64 y `objcopy -O elf32-i386` porque Multiboot 1 solo carga ELF32).
- `kernel/net.c`: PCI, e1000, RTL8139, ARP, IPv4, ICMP, UDP, DHCP (reserva 10.0.2.15), DNS, TCP cliente (una conexión), HTTP/1.0 (redirecciones, chunked, Content-Length). API en `net.h`. **Sin TLS.**
- `kernel/gui.c`: navegador con `http://`, carga asíncrona (`browser_poll` en `gui_frame`), enlaces relativos (`resolve`), barra con selección total; la terminal deja el gancho `kcon_hook` puesto para respuestas de red tardías.
- `kernel/html.c`: subconjunto de HTML (omite script/style/comentarios, entidades, h1–h6, listas, tablas como texto, `[imagen: alt]`).
- `tools/build-iso.sh` (+ `Construir-ISO.cmd`): compila ambos kernels, verifica cabeceras (`check-mb.py`), `grub-mkrescue -d /usr/lib/grub/i386-pc`; menú GRUB con 32/64 gráfico y texto.
- Pruebas: `tools/nettest.py` (pila real contra red simulada en Python, lock-step, ASan/UBSan, 34 comprobaciones), `make -C kernel hosttest`, `make -C kernel hostgui`.

## Estado de pruebas (importante)
- Verificado en host: net.c (nettest), PMM/ELF/clone/brk, GUI (capturas en `docs/`), compilación sin avisos de ambos kernels (-m32 y -m64), cabeceras Multiboot, desensamblado de `boot64.S`.
- **NUNCA ejecutado en QEMU/VM** (el sandbox no tiene QEMU ni red para instalarlo): arranque GRUB, paso a modo largo, framebuffer, ratón PS/2, controladores e1000/RTL8139 sobre la tarjeta virtual real, ring 3 y planificador.
- PRIMER PASO: generar la ISO, arrancar en VM BIOS/Legacy (e1000, NAT) y arreglar lo que falle. Sospechosos: `boot64.S`; validación de framebuffer en `kmain`/`kmain64`; `e_init` (reset/RCTL/TCTL, esperas con `kticks` requieren interrupciones activas: `net_init` se llama tras `sti`); `r_init`; mapeo sin cache ≥2 GB.
- Depurar con `kng-kernel*-text.elf` (salida por VGA y COM1) y serie a archivo en la VM.

## Siguiente (orden sugerido)
1. Verificar arranque (arriba) y red real en VM.
2. Procesos/ring 3 en 64 bits (TSS, syscall/sysret o int 0x80, planificador, ELF64).
3. TLS (https) — grande; o proxy/alternativa. Soporte de más tarjetas (virtio-net, PCnet, vmxnet3).
4. Disco (ATA/AHCI/virtio-blk) + FS persistente; apps como procesos de usuario con API gráfica; USB; teclado es/ñ; UEFI.

## Limitaciones conocidas
Sin https; HTML sin CSS/JS/imágenes; una petición HTTP a la vez; FS en RAM; PS/2 únicamente; teclado US; framebuffer solo si GRUB negocia 32 bpp ≥640x480; en 64 bits la terminal no tiene ps/run/sh.
