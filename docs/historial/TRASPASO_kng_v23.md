# Traspaso kng v23 (kernel 0.7, 64 bits)

## Que funciona (arrancado de verdad en QEMU por GitHub Actions, log en cada commit)
- Programas Linux REALES sin modificar corren en modo usuario (ring 3) sobre el kernel de 64 bits:
  - sin libc (`/bin/hola`), glibc estatica PIE y NO-PIE (direccion baja 0x400000), y programas DINAMICOS (ld.so + libc.so.6 del PC).
  - fork, execve, wait4, pipes, hilos (pthread), mutex/futex, nanosleep, kill, exit_group, varios procesos a la vez con planificador por turnos y apropiacion por timer.
  - archivos: open/read/write/lseek/stat/getdents64/mkdir/unlink/rename sobre el sistema de archivos de kng (lo que no es del rootfs se guarda en KNGFS).
- Comandos nuevos del kernel de 64 bits: `ejecutar <ruta> [args] [&]`, `programas`, `procesos`, `matar <pid>`.
- El rootfs (programas + ld.so + libc) viaja como modulo de arranque (cpio): `tools/mkrootfs.py` lo genera; `tools/build-iso.sh` lo mete al ISO y GRUB lo pasa a los dos kernels de 64 bits. La entrada por defecto de GRUB ahora es "64 bits (grafico)".
- Kernel (bajo 4 MB) y programas de usuario usan CR3 distintas: el kernel no se ve desde el programa.

## Que NO esta (y por que importa para Chrome/Brave/LibreOffice)
- Sockets para programas de usuario (sin eso un navegador no puede conectarse). Es lo siguiente.
- Manejadores de senales del usuario (kill y terminar si funcionan), epoll/poll/select, mmap compartido, memoria compartida, eventfd/timerfd.
- Dispositivos para programas graficos: framebuffer, teclado y raton como archivos; servidor grafico (X11/Wayland). Sin eso no hay ventanas de programas Linux.
- /proc y /dev, usuarios y permisos, ext4 desde disco (el rootfs va en memoria).
- Capa para programas de Windows (Wine necesita todo lo anterior).
- Ejecutar programas desde la terminal grafica bloquea la ventana mientras corren (se repinta, pero las ventanas no responden al raton).
- Kernel de 32 bits: sin cambios (sigue con su propio sistema de procesos).

## Verificacion
CI (.github/workflows/qemu.yml): compila 32 y 64 bits, arranca ambos en QEMU, ejecuta hola, hola-pie, hola-static, hola-dyn y multi-* (fork/pipe/exec/hilos/kill) y comprueba las salidas.
NO verificado en una VM con pantalla (interfaz grafica) ni en VirtualBox/VMware con tus tarjetas de red.
