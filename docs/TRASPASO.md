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

- **Persistencia en disco (probado en CI, `persist.yml`)**: controlador SATA (AHCI) ademas de IDE. Se guarda lo que cambio desde el arranque (incluido lo instalado con apt), sin el antiguo tope de 2 MB, se carga encima del sistema base y se guarda al apagar o reiniciar. Probado: guardar en un arranque y leer en otro, con 64 y 32 bits, en SATA e IDE. Activar: Ajustes > Almacenamiento > "Usar este disco" (BORRA ese disco) o `disco formatear si` en la terminal.
- **Red (probado en CI, `red.yml`)**: Intel e1000, e1000e (82574 probado; I217/I218/I219 agregados sin probar en hardware), RTL8139, virtio, PCnet con DHCP y ping. Nuevo controlador Realtek RTL8111/8168/8169/8101 (QEMU no lo emula: SIN PROBAR). Intel igb (I210/I211/82576): agregado pero FALLA en QEMU.
- **Arranque**: menu con los 4 modos siempre (64/32 bits, grafico/texto) y submenu de resolucion; por defecto la mayor hasta 1920x1080. ISO hibrida BIOS+UEFI (UEFI probado en QEMU/OVMF). Pantallas de 24 bits soportadas.
- **Ajustes con pestanas**: General, Pantalla (cambio de resolucion en caliente en VirtualBox/QEMU), Almacenamiento (barra de colores por categorias y disco), Sistema.

## Pendiente / SIN PROBAR
- **Bug conocido (memoria)**: con 6 GB, si se ejecuta `probar-ram` (comando de diagnostico que reserva y marca TODA la RAM) y despues un programa dinamico, ese programa muere con 'excepcion 6' en `memcpy` de la libc (pagina de codigo distinta a la del archivo). Sin `probar-ram` todo pasa (memtest 77/77). Diagnostico en `.github/workflows/mem6.yml`. Descartado: modulos dentro del heap, red, tiempo, bloques de kmalloc sin poner a cero. Aparecio cuando la imagen del kernel crecio 82 KB.
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

## Sesion 13-nov-2026: pantalla X negra y navegadores
- **Causa de la pantalla negra (arreglada):** `file_shm` (archivo mapeado MAP_SHARED) dejaba `n->cap` del bloque viejo; al crecer el archivo (cache de apt) se escribia fuera del bloque y se corrompia el heap del nucleo -> "Cannot fork", programas X no arrancaban.
- **Mapa de identidad (boot64.S):** la parte alta de las entradas de 2 MB era 0: de 4 a 16 GB se repetian los primeros 4 GB. Corregido.
- **Pseudoterminales** `/dev/ptmx` + `/dev/pts/N` (kernel64/pty.inc): xterm funciona (verificado en CI con captura: xlogo y xterm dibujados en la Pantalla X).
- Pantalla X: solo se redibuja cuando cambia (sin parpadeo del puntero) y muestra aviso mientras esta negra.
- Para Chromium (probado en CI con chromium 156 de Debian 12, 8 GB): SO_PASSCRED/SCM_CREDENTIALS, creat, permisos reales (mkdir/open/chmod/stat), execve de /proc/self/exe, eventfd2/memfd_create con EINVAL ante opciones invalidas, sellos de memfd, F_GETFL con modo real, /proc/self/task (nlink = 2 + hilos), escrituras en /proc aceptadas, EFAULT en paginas PROT_NONE y de solo lectura.
- **Estado de Chromium:** el proceso principal, el de red y los hijos arrancan; el renderizador aun falla mas adelante (rip offset 0x9cb0d6c en chromium 156.0.8078.79). Siguiente paso: `analizar.yml` con esa direccion y la pila (`rastro 1` imprime "[pila N]").
- Herramientas nuevas: `tools/ci/chromium.txt`, `xpantalla.txt`, `xterm.txt`, `memleak.txt`; `ci_drive.py` admite `@captura nombre`; workflow `analizar.yml` + `tools/ci/analizar.py` (desensambla un binario de Debian alrededor de direcciones).
- **CI detenido:** GitHub Actions dejo de ejecutar trabajos (limite de gasto / pagos de la cuenta).

## 14-nov-2026: navegadores y LibreOffice funcionando
- **LibreOffice Writer** abre con ventana en la Pantalla X (CI, 8 GB; ~4 min el primer arranque).
- **Chromium** muestra paginas web (CI: example.com cargada). Claves: zona de mmap de 32 TB (MMAP_BASE 0x600000000000, ld.so en 0x5f0000000000) para el sandbox de V8 de 1 TB, y modo `--single-process --ozone-platform=x11` en los lanzadores de chromium/chrome/brave/opera.
- El primer arranque de Chromium aun se cae (SIGSEGV en una zona de mmap, rip 0x600001751f96; pila en el registro "[pila]"); el lanzador lo vuelve a abrir solo y la segunda vez funciona.
- Pendiente: modo multiproceso de Chromium (se cierra a los ~20 s), fuentes CJK (cuadritos), inotify, ptrace.
- Compilacion fijada a -march=x86-64 / i686 (gcc 15 de Ubuntu 26.04 generaba BMI2/AVX2). CI fijado en ubuntu-24.04 (diag.yml acepta `imagen`).

## 26-nov-2026: estilos visuales, animaciones, inicio de sesion, programas de Debian
- **3 estilos** (Ajustes > Pantalla > Estilo visual; se guarda `tema=` en /Sistema/kng.cfg, tambien `acento=` y `fondo=`):
  Frutiger Aero (vidrio azul con brillo, cielo con burbujas, por defecto), Windows 11 (barra de tareas abajo, sin barra
  superior, botones rectangulares, fondo "flor"), Mac (semaforos a la izquierda, barra de menus, dock con lupa, ondas).
  Codigo en `kernel/gui_estilo.inc` (incluido por gui.c). `BARH` ahora es variable (0 en Windows 11).
- Vidrio esmerilado: `blur_rect` (gfx.c) bajo titulos Aero, barra y dock. Iconos del dock en cache con alfa (`icon_cache_build`)
  y escalados con `blit_pm_scaled`. Texto grande: `text_scaled`.
- Animaciones (100 Hz): abrir (escala+opacidad, `draw_window_anim`), cerrar y minimizar (copia "fantasma" `ghost_new`),
  maximizar/restaurar/ajustar (rectangulo visual `win_vis`, el logico cambia al instante), rebote del icono al abrir.
- Ventanas: minimizar (vuelve con el dock), maximizar con boton, cambiar tamano arrastrando bordes/esquina (Navegador,
  Archivos, Notas, Visor, Programas).
- Inicio de sesion opcional: sin usuario entra directo. Ajustes > Cuenta crea usuario+contrasena (`usuario=`, `sal=`,
  `clave=` = SHA-256 iterado 1000 veces con sal). Con usuario: pantalla de bloqueo al encender y "Bloquear ahora";
  con la sesion bloqueada no pasa teclado ni raton a la pantalla X.
- Programas de Linux: ademas de la lista fija, muestra todos los programas con ventana de /usr/share/applications/*.desktop
  (Name[es]/Name, Exec sin %U); clic = `abrir <exec>`.
- kernel64: ptrace/mount -> EPERM, ioprio, rt_tgsigqueueinfo, epoll_pwait2, sched_getattr, io_uring -> ENOSYS sin aviso.
- Simulador: `python3 tools/dev/hostgui.py` prueba temas, minimizar, tamano, cuenta/bloqueo y .desktop (TEMAS/SESION/DEBIAN OK).

## 26-nov-2026 (2): programas de Linux en ventanas propias + Aero mejorado
- Ya no hay "Pantalla X" con matchbox: Xvfb mide lo mismo que la pantalla y kng hace de gestor de ventanas.
  `kng-xinput` (kernel64/user) ahora tambien lista las ventanas X visibles cada 150 ms en `/tmp/.kng-xwin`
  (`id x y w h emergente titulo`), guarda el icono `_NET_WM_ICON` en `/tmp/.kng-xicon-<id>` y obedece ordenes del
  escritorio por /dev/input/event0 (tipo 0x31 parametros, 0x30: 1 mover/tamano, 2 al frente+foco, 3 cerrar, 4 matar).
- `kernel/gui_xwin.inc`: cada ventana X es una ventana de kng (A_XWIN) con su marco, en el dock (icono real), minimizable
  (se manda fuera de la pantalla X), maximizable y con tamano por bordes. Los menus/listas (override-redirect) se pintan
  sin marco y encima. El clic en una ventana X la trae al frente antes de entregarlo. `pantalla ver` muestra todo X.
- `/tmp/.kng-sin-wm` (lo crea el kernel) hace que abrir-x no arranque matchbox.
- Aero: marco grueso de vidrio, resplandor del titulo, botones de gel, menus y widgets de vidrio, reloj de aguja,
  orbe de inicio, reflejo de los iconos en la repisa del dock, burbujas tornasoladas, aurora y destellos.
- Probado: kng-xinput contra Xvfb real (lista, icono, mover, foco, clic, tecla, cerrar); simulador VENTANAS LINUX OK.
- (3) kng-xinput es ademas un gestor de ventanas minimo (SubstructureRedirect en la raiz, _NET_SUPPORTED, ventana de
  comprobacion "kng"): cumple MapRequest/ConfigureRequest y pasa al escritorio por `/tmp/.kng-xreq` las peticiones de
  _NET_WM_MOVERESIZE, _NET_WM_STATE (maximizar), WM_CHANGE_STATE (minimizar) y _NET_ACTIVE_WINDOW. Las ventanas con
  _MOTIF_WM_HINTS sin decoraciones (Chromium, Brave, Chrome, GTK con barra propia) salen SIN la barra de kng: se ven
  como en Windows, con sus pestanas y botones, y esos botones funcionan. Orden 5 = poner/quitar maximizada en _NET_WM_STATE.
- Aviso "Abriendo <programa>..." con ruedita hasta que aparece su ventana (o 2 minutos).
- (4) Apps por estilo: Tema tiene `panel` y `claro` (Aero = apps claras tipo Windows 7; C_PANEL, C_OK, C_WARN; colores
  del HTML por estilo). Archivos con columna "Lugares" (FMS) y seleccion por estilo (`sel_row`). Ajustes con menu
  lateral de 8 secciones (SET_ORD): General, Pantalla, Escritorio (widgets, iconos, animaciones, transparencias, lupa),
  Red (estado, volver a conectar, probar), Almacenamiento, Cuenta, Accesibilidad (cursor grande, reducir movimiento,
  24 h, menos transparencia), Sistema (+ buscar actualizaciones). Se guardan en kng.cfg (anim, transp, lupa, widgets,
  iconos, cursor). Menu de apagado en la barra; apagado ACPI real (kernel64).
