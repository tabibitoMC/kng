# Königreich OS — kernels propios `kng-kernel` (32 y 64 bits)

Dos kernels escritos desde cero (C + ensamblador), sin código de Linux. Comparten la interfaz gráfica (`gfx/gui/fs/html`) y la pila de red (`net.c`).
Arrancan con GRUB (Multiboot 1, BIOS). Referencias: documentación pública (Intel SDM, OSDev, RFC, manuales de Intel/Realtek).

| | 32 bits (`kernel/`) | 64 bits (`kernel64/`) |
|---|---|---|
| Arranque | Multiboot → modo protegido | Multiboot → modo protegido → **modo largo** (`boot64.S`: CPUID, tablas PAE de 4 GB, EFER.LME) |
| Memoria | PMM con bitmap, paginación, heap | Paginación de 4 GB en identidad (páginas de 2 MB), heap |
| Procesos | hilos, **ring 3**, ELF32, fork/exec/wait/brk, syscalls `int 0x80` | **todavía no** (sin planificador ni modo usuario) |
| Interfaz | escritorio, archivos, terminal, navegador, ajustes | **la misma** |
| Red | e1000, RTL8139, DHCP, DNS, TCP, HTTP | **la misma** (`net.c`) |
| Entrada | teclado y ratón PS/2 | igual |

Multiboot 1 solo carga ELF32: el kernel de 64 bits se enlaza como ELF64 y `objcopy -O elf32-i386` cambia el contenedor (el código sigue siendo de 64 bits).

## Pila de red (`net.c`, compartida)
- **PCI** (recorrido de buses), **Intel e1000** (8254x, descriptores heredados, sondeo sin interrupciones) y **Realtek RTL8139**.
- **ARP** (caché + cola de 1 paquete), **IPv4** (sin fragmentación), **ICMP** (responde ping; comando `ping`), **UDP**, **DHCP** (con configuración de reserva 10.0.2.15 si no responde), **DNS** (consulta A, reintentos), **TCP** (cliente de una conexión: SYN con MSS, retransmisión, ACK duplicado ante pérdida, FIN), **HTTP/1.0** (redirecciones, `Content-Length`, `chunked`, `Connection: close`).
- **Sin TLS: `https://` no funciona** (la página lo dice). Una petición a la vez. Límite de 512 KB por página.
- Memoria DMA: arreglos estáticos del kernel; las páginas por encima de 2 GB se mapean sin cache (MMIO de las tarjetas y framebuffer).
- Terminal: `red`, `ping <host>`, `dns <host>`, `wget <url>`. Navegador: `http://…`, o un nombre suelto como `example.com`.

## Interfaz y navegador
Framebuffer lineal 1024x768x32 pedido a GRUB; dibujo por software con antialiasing; ventanas, dock, reloj (RTC), cursor. El navegador entiende un subconjunto de HTML (títulos, párrafos, listas, tablas como texto, enlaces, entidades; **ignora** CSS, JavaScript e imágenes) y resuelve enlaces relativos.

## Pruebas (qué se verificó y qué no)
Verificado en el equipo de desarrollo (sin QEMU):
- `python3 tools/nettest.py`: compila `net.c` real con ASan/UBSan y lo prueba contra una red simulada **independiente** escrita en Python (ARP, DHCP, DNS, ICMP, servidor TCP/HTTP con pérdida de paquetes). Verifica sumas de comprobación y 34 comprobaciones (contenido idéntico byte a byte, redirecciones, chunked, RST, tiempos agotados, https rechazado…).
- `make -C kernel hosttest`: PMM, cargador ELF (13 ELF malformados), clone, brk.
- `make -C kernel hostgui`: la interfaz real dibujada en el host (capturas en `docs/`).
- `python3 tools/check-mb.py`: cabeceras Multiboot de los 4 ELF.
- Ambos kernels compilan sin avisos; el código de arranque de 64 bits se revisó desensamblado.

**NO verificado** (no hay QEMU/VM en el entorno de desarrollo): el arranque real con GRUB, el paso a modo largo, la negociación de video, el ratón PS/2, los controladores de red sobre hardware (virtual) real, ring 3 y el planificador del kernel de 32 bits. Si algo falla al arrancar, el primer sospechoso en 64 bits es `boot64.S` y, en ambos, la validación del framebuffer en `kmain`/`kmain64`.

## Qué falta
Procesos y ring 3 en 64 bits, UEFI, disco y sistema de archivos persistente, TLS (https), USB, SMP/APIC, aplicaciones como procesos de usuario con API gráfica, teclado en español, CSS/imágenes en el navegador.
