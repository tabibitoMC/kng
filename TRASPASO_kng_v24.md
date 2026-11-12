# Traspaso kng v24 (kernel 64 bits + Debian real + pantalla X)

Estado honesto. "VERIFICADO" = lo ejecuto GitHub Actions arrancando kng en QEMU (los logs se guardan en cada ejecucion). "SIN PROBAR" = el codigo existe pero nunca se vio funcionar.

## Verificado en QEMU (CI)
- Kernel 64 bits: mapa de 16 GB en identidad, heap en varias regiones de RAM, arranque con 6 GB y uso de memoria por encima de 4 GB (`probar-ram`).
- Programas Linux sin modificar: sin libc, glibc estatica PIE/no-PIE, dinamicos (ld.so + libc), fork/exec/hilos/futex, pipes, senales, epoll/poll/select, sockets TCP/UDP, eventfd/timerfd, mmap (privado, compartido, de archivo), `/proc` y `/dev` basicos.
- Archivos: mtime reales y `utimensat`, enlaces simbolicos, **enlaces duros reales** (st_nlink, mismo inodo, contenido compartido), `sendfile` y `copy_file_range`. `memtest` 70 pruebas.
- Red: e1000, DNS (cola de 8 paquetes mientras llega ARP, FIONREAD), TCP; `nettest` 42/42.
- **Debian 12 real** sobre kng: `apt` 2.6.1 y `dpkg` 1.21.23 funcionan de punta a punta. Comandos en espanol: `actualizar`, `instalar`, `quitar`, `mejorar`, `buscar`, `mostrar`, `paquetes`, `limpiar`, `ayuda-programas`.
- **Servidor X real (Xvfb)** corriendo, con clientes X reales (`xlogo`, `xev`, `xwininfo`) y **entrada de teclado/raton**: `kng-xinput` lee `/dev/input/event0` y la entrega por XTEST; `xev` recibe MotionNotify, ButtonPress/Release y KeyPress/Release con la tecla correcta.
- **LibreOffice 7.4.7 real (Writer + Calc)**: se instala con apt y `soffice --headless --convert-to pdf` convierte un .txt a un PDF valido (`%PDF-1.6`).

- **Modo grafico probado en CI** (ISO con GRUB, opcion de arranque `terminal`, `diag.yml` con `grafico=si`): `instalar brave` completo en 6 min 16 s con 8 GB. Causa del cuelgue de 30+ min que se veia en la VM: el bucle del escritorio dormia (`hlt`, un tic de 10 ms) tras cada bloqueo de proceso aunque hubiera procesos listos; con tuberias de 4 KB un binario de 335 MB generaba 80 000 bloqueos. Arreglado (no dormir si hay procesos listos; tuberias de 64 KB). `clone3` implementado (ya no sale el aviso 435).
- Repositorios de Brave, Chrome y Opera y la descarga de OpenOffice (SourceForge): URLs y llaves verificadas (responden 200).

## Pendiente / SIN PROBAR
- **Navegador (Chromium/Brave/Chrome/Opera)**: ver "Estado del navegador" mas abajo.
- LibreOffice con ventana (modo X) y Apache OpenOffice (la URL de SourceForge no esta verificada).
- Chrome, Opera y OpenOffice: los repositorios responden, pero la instalacion completa de esos tres no se ha ejecutado aun en kng (Brave, Chromium y LibreOffice si).
- Ventana "Pantalla X" del escritorio grafico, ventana "Programas" y `abrir`: el CI solo arranca en modo texto, **nunca se vio en una VM con pantalla**.
- Persistencia: lo instalado vive en RAM y se pierde al reiniciar (hace falta disco o cache en KNGFS).
- Permisos y propietarios de archivos son simulados (todo root, 0755).
- fork sin copia-al-escribir (copia completa); `vfork` se emula como fork.
- Sin inotify, signalfd ni pty (`/dev/ptmx`); `/proc/self/maps` truncado a 8 KB.
- Capa de Windows (Wine): no iniciada.
- Pulido visual: pendiente.

## Como probar rapido
- Pruebas automaticas: `.github/workflows/qemu.yml` (cada push).
- Pruebas manuales en CI: workflow *diagnostico* con una lista de `tools/ci/*.txt` (`pantalla.txt`, `navegador.txt`, `oficina.txt`...). Disparo por API: `gh api -X POST repos/tabibitoMC/kng/actions/workflows/diag.yml/dispatches -f ref=main -f 'inputs[lista]=tools/ci/X.txt' -f 'inputs[ram]=6G'`.
- ISO con Debian: workflow *construir-iso* (publica en Releases) o `KNG_DEBIAN=1 bash tools/build-iso.sh` en WSL. VM con 4 GB de RAM o mas, BIOS, red NAT e1000.
- Depurar: `rastro 1` (llamadas fallidas), `procesos` o Ctrl+] (estado de cada proceso, sus descriptores y esperas).
