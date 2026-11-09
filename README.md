# Königreich OS (kng) — sistema operativo propio, 32 y 64 bits

Sistema operativo **independiente**: dos kernels propios (`kng-kernel` de 32 y de 64 bits), interfaz gráfica propia, gestor de archivos,
terminal, navegador con red, ajustes. **No es Linux, no usa Debian y no incluye código de Linux.**

## Qué trae (v21)
- **Red en más VMs**: además de e1000 y RTL8139, ahora AMD PCnet (VirtualBox por omisión) y virtio-net. Si no reconoce tu tarjeta, el error muestra su identificador PCI.
- **Archivos mejorados + disco**: `cp`, `mv`, `ren`, `df`; si la VM tiene un disco IDE, `disco formatear si` activa el guardado automático (KNGFS) y los archivos sobreviven al reiniciar. Nunca escribe en un disco sin la firma KNGFS salvo esa orden.
- **Monitor del sistema** y motores de búsqueda Brave/Google/Bing/DuckDuckGo en kng-proxy.
- Mapa de módulos: `kernel/MODULOS.md`.
- **Rendimiento (v21)**: repintado solo de la zona que cambia, sombras baratas, ventanas tapadas no se dibujan, framebuffer en *write-combining* (PAT) en vez de sin caché.
- **Web real vía `kng-proxy`**: `Proxy-kng.cmd` (o `python tools/kng-proxy.py`) en tu PC; kng busca en DuckDuckGo y abre https:// a través de él (el cifrado lo hace el proxy, kng sigue sin TLS). VM en NAT: el PC es 10.0.2.2:8080.
- **Escritorio modular**: iconos y widgets (reloj, sistema) arrastrables; doble clic abre; ventanas se maximizan con doble clic en el título y se ajustan arrastrando a los bordes.
- **Kernel de 32 bits** (`kernel/`): procesos en ring 3, ELF, fork/exec/wait, memoria virtual.
- **Kernel de 64 bits** (`kernel64/`): modo largo, misma interfaz y misma red. (Sin procesos todavía.)
- **Escritorio gráfico** 1024x768: fondo con corona, dock, ventanas arrastrables, reloj.
- **Archivos**, **Terminal**, **Ajustes** y **Navegador** (con **buscador**: palabras sueltas buscan en páginas y archivos; opcionalmente en la web por http con `buscador=http://.../?q=%s` en `/Sistema/kng.cfg`).
- **Apps simples**: **Notas** (guarda en /Documentos; los .txt se abren aquí), **Calculadora**, **Snake**.
- Interfaz pulida: fondo con tramado (sin bandas), bordes suavizados 4x4, dock de 8 apps, indicador de red.
- **Red**: tarjetas Intel e1000 y Realtek RTL8139, DHCP, DNS, TCP, HTTP. Navegador con `http://`; terminal con `red`, `ping`, `dns`, `wget`.

Capturas de la interfaz: carpeta `docs/` (generadas con el mismo código de la interfaz).

## Construir la ISO (Windows + WSL)
1. Una sola vez, en WSL: `sudo apt update && sudo apt install -y build-essential gcc-multilib grub-pc-bin xorriso mtools python3`
2. Doble clic en `Construir-ISO.cmd` (o `wsl bash tools/build-iso.sh`). Resultado: `out/KonigreichOS-kng.iso`.

## Probarla
- Máquina virtual **BIOS/Legacy** (no UEFI), 256 MB de RAM o más, **ratón PS/2**, tarjeta de red **Intel e1000** (o RTL8139) en modo **NAT**.
  - VirtualBox: Red → Adaptador 1 → NAT, tipo "Intel PRO/1000 MT Desktop". VMware: e1000. QEMU: `-device e1000,netdev=n0 -netdev user,id=n0`.
- Menú de GRUB: 32 bits (gráfico), **64 bits (gráfico)**, y las dos versiones en modo texto (útiles si el modo gráfico no arranca).
- En el navegador: escribe `example.com` o `http://neverssl.com`. En la terminal: `red`.

## Limitaciones actuales (sin rodeos)
- **https:// no funciona** (no hay TLS): muchos sitios modernos no cargarán; sirven los que ofrecen http.
- El navegador ignora CSS, JavaScript e imágenes: es un visor de HTML simple.
- El kernel de 64 bits **aún no tiene procesos/modo usuario** (el de 32 bits sí).
- Las aplicaciones corren **dentro del kernel**; los archivos viven en RAM y se pierden al reiniciar (sin controlador de disco).
- Solo PS/2 (sin USB), teclado mapa US, BIOS (no UEFI).
- **Nunca se arrancó en una máquina virtual al generar este paquete**: el código compila, la red y la interfaz se verificaron en el equipo de desarrollo contra simuladores, pero el arranque real con GRUB (sobre todo el paso a 64 bits y la red sobre la tarjeta virtual) está sin probar.

## Pruebas
- `gcc tools/kbdtest.c -o /tmp/kbdtest && /tmp/kbdtest` — teclado: US/Latam/Espana, Mayus, AltGr, tildes.
- `python3 tools/nettest.py` — pila de red real contra una red simulada (sin QEMU).
- `make -C kernel hosttest` — memoria, ELF, fork, brk. `make -C kernel hostgui` — interfaz dibujada en el host.
- `make -C kernel test` / `make -C kernel64 run` — con QEMU instalado.

Autor: Nick Jose Tolentino Rojas · Fuente: DejaVu Sans Mono (licencia libre Bitstream Vera/DejaVu).


## Programas Linux REALES (kernel de 64 bits)
kng 64 bits ejecuta programas de Linux sin modificar (ELF, glibc, ld.so) con su propia capa de compatibilidad (sin código de Linux): procesos, hilos, señales,
sockets, epoll, mmap/memoria compartida, tty, /proc y /dev. Encima corre un **Debian 12 real** (apt/dpkg, curl, busybox...).

**ISO con Debian** (necesita WSL con sudo): `KNG_DEBIAN=1 bash tools/build-iso.sh` (o el workflow *construir-iso* en GitHub Actions, que lo publica en Releases).
Máquina virtual con **4 GB de RAM o más** (el sistema vive en memoria).

Comandos en español (en la terminal de kng, también dentro de la ventana gráfica):
`actualizar` · `instalar <nombre>` · `quitar <nombre>` · `mejorar` · `buscar <palabra>` · `mostrar <nombre>` · `paquetes` · `limpiar` · `ayuda-programas`
Nombres especiales de `instalar`: `brave`, `chrome`, `opera`, `chromium`, `firefox`, `libreoffice`, `openoffice` (el resto es cualquier paquete de Debian).
Programas con ventana: `pantalla` (servidor X real) y `abrir <programa>` (p. ej. `abrir brave`), o la ventana **Programas** del dock.
Diagnóstico: `rastro 1` muestra las llamadas del sistema que fallan. Estado real y pendientes: ver `TRASPASO_kng_v24.md`.
