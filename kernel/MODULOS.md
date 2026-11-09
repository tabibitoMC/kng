# Módulos de Königreich OS (qué hace cada archivo)

| Módulo | Archivo | Qué hace |
|---|---|---|
| Arranque y kernel | `kernel/boot.S`, `kernel.c` (32 bits) · `kernel64/boot64.S`, `kernel64.c` (64 bits) | Multiboot, memoria, interrupciones, teclado/ratón PS/2, procesos (solo 32 bits) |
| Gráficos 2D | `gfx.c` | Pixeles, formas suaves, texto, recorte por zona sucia |
| Escritorio y apps | `gui.c` | Ventanas, dock, iconos y widgets arrastrables, Archivos, Terminal, Navegador, Notas, Calculadora, Snake, Monitor, Ajustes |
| Páginas web | `html.c` | Motor mínimo de HTML y páginas internas `kng://` |
| Archivos | `fs.c` | Árbol de archivos: crear, borrar, renombrar, copiar, mover, serializar |
| Disco | `disk.c` | Disco ATA (PIO) y formato KNGFS con guardado automático. Solo escribe en discos con firma KNGFS o tras `disco formatear si` |
| Red | `net.c`, `net_pcnet_virtio.h` | Tarjetas e1000, RTL8139, AMD PCnet, virtio-net; ARP, IP, ICMP, UDP, DHCP, DNS, TCP, HTTP |
| Puente web | `tools/kng-proxy.py` | Corre en el PC: https y buscadores (DuckDuckGo, Brave, Google, Bing) para el navegador de kng |

## kbd.c / kbd.h (v22) — teclado
Traduce bytes PS/2 (set 1) a caracteres Latin-1. Distribuciones: US, Latinoamerica, Espana (`teclado us|latam|es` en la terminal o boton en Ajustes; se guarda en `/Sistema/kng.cfg`). Soporta Mayus, AltGr (@ \ | ~ { } ^), n con tilde, ¿ ¡, tilde y dieresis como tecla muerta. Lo comparten el kernel de 32 y el de 64 bits. Prueba en el PC: `gcc tools/kbdtest.c -o /tmp/kbdtest && /tmp/kbdtest`.

## gui_panic (v22)
Si el kernel sufre una excepcion con la interfaz grafica activa, se dibuja una pantalla de "Error fatal" (excepcion, EIP/RIP, codigo, CR2) en vez de dejar la pantalla congelada. Los mismos datos salen por el puerto serie.
