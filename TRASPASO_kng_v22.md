# Traspaso kng v22 (kernel 0.7)
Hecho: teclado propio con distribuciones US/Latam/Espana (kbd.c, compartido 32/64 bits); tildes y ñ en terminal, Notas y barra del navegador (texto UTF-8 interno, borrado por caracter); selector de teclado en Ajustes + comando `teclado` + persistencia en /Sistema/kng.cfg; pantalla de error fatal visible (gui_panic) en ambos kernels; el buffer de teclado ahora es unsigned (antes los caracteres >127 se rompian).
Revisado: syscalls del kernel de 32 bits ya validan punteros de usuario (user_ok), no hizo falta cambiar.
Verificado en el PC: kbdtest, hosttest, nettest, hostgui (incluye capturas 19-21), check-mb, compilacion 32 y 64 bits sin avisos.
NO verificado: nada se arranco en VM por mi parte; los scancodes reales dependen de tu VM (si una tecla sale mal, dime cual).
Pendiente real (no se toco): procesos en 64 bits, apps como programas separados (hoy corren en modo kernel), TLS propio, USB, UEFI, syscalls con numeracion propia.
