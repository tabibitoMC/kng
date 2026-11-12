#!/usr/bin/env bash
# Genera out/KonigreichOS-kng.iso (arranque BIOS con GRUB) con los kernels de 32 y de 64 bits. Sin Debian, sin Linux.
set -e
cd "$(dirname "$0")/.."
faltan=""
for c in gcc make grub-mkrescue xorriso mformat objcopy python3; do command -v "$c" >/dev/null || faltan="$faltan $c"; done
if [ -n "$faltan" ] || [ ! -d /usr/lib/grub/i386-pc ]; then
  echo "Faltan herramientas:$faltan"
  echo "Instala con:  sudo apt update && sudo apt install -y build-essential gcc-multilib grub-pc-bin grub-efi-amd64-bin xorriso mtools python3 dosfstools fdisk debootstrap cpio gzip"
  exit 1
fi
make -C kernel clean >/dev/null; make -C kernel64 clean >/dev/null
make -C kernel      # kernel de 32 bits
make -C kernel64    # kernel de 64 bits
rm -rf build/iso && mkdir -p build/iso/boot/grub out
cp kernel/kng-kernel.elf kernel/kng-kernel-text.elf kernel64/kng-kernel64.elf kernel64/kng-kernel64-text.elf build/iso/boot/
# rootfs (programas Linux + enlazador dinamico + libc del PC) como modulo de arranque; solo lo usa el kernel de 64 bits
if python3 tools/mkrootfs.py build/iso/boot/rootfs.cpio; then ROOTFS=1; else ROOTFS=0; echo "[aviso] sin rootfs (falta libc estatica: sudo apt install libc6-dev). El ISO igual arranca, sin programas Linux."; fi
# Debian real (opcional): se usa si ya existe build/debian.cpio.gz, o si se pide con  KNG_DEBIAN=1 bash tools/build-iso.sh
# Se reutiliza un Debian ya creado antes (copia guardada en WSL o en otra carpeta de kng en Descargas) para no volver a bajarlo.
CACHE_DEB="$HOME/.cache/kng/debian.cpio.gz"
if [ "$KNG_DEBIAN" = 1 ] && [ ! -f build/debian.cpio.gz ]; then
  mkdir -p build
  if [ -f "$CACHE_DEB" ]; then cp "$CACHE_DEB" build/debian.cpio.gz; echo "[debian] reutilizando el Debian guardado"
  else
    VIEJO=$(ls -t /mnt/c/Users/*/Downloads/*/build/debian.cpio.gz /mnt/c/Users/*/Downloads/*/*/build/debian.cpio.gz /mnt/c/Users/*/Desktop/*/build/debian.cpio.gz /mnt/c/Users/*/Desktop/*/*/build/debian.cpio.gz 2>/dev/null | head -1)
    if [ -n "$VIEJO" ] && gzip -t "$VIEJO" 2>/dev/null; then cp "$VIEJO" build/debian.cpio.gz; echo "[debian] reutilizando $VIEJO"
    else bash tools/mkdebian.sh; fi
  fi
fi
if [ -f build/debian.cpio.gz ] && [ ! -f "$CACHE_DEB" ]; then mkdir -p "$(dirname "$CACHE_DEB")" && cp build/debian.cpio.gz "$CACHE_DEB"; fi
if [ -f build/debian.cpio.gz ]; then
  cp build/debian.cpio.gz build/iso/boot/debian.cpio.gz
  python3 tools/mkrootfs.py --debian build/iso/boot/kng-extra.cpio
  DEBIAN=1
else DEBIAN=0; echo "[aviso] sin Debian (para incluirlo:  KNG_DEBIAN=1 bash tools/build-iso.sh )."; fi
python3 tools/check-mb.py build/iso/boot/*.elf
# Menu de arranque: los 4 modos (64/32 bits, grafico/texto) siempre; con Debian, ademas las entradas con Debian.
# Resolucion: primero 1920x1080 y, si la tarjeta no la tiene, la siguiente de la lista. En UEFI se usa la del firmware.
VIDEO='if [ "$grub_platform" = efi ]; then set gfxpayload=keep; elif [ -n "$kng_res" ]; then set gfxpayload=$kng_res; else set gfxpayload=1920x1080x32,1600x900x32,1366x768x32,1280x1024x32,1280x720x32,1024x768x32,800x600x32,1920x1080x24,1366x768x24,1280x1024x24,1024x768x24,800x600x24; fi'
TEXTO='set gfxpayload=text'
R64=""; [ "$ROOTFS" = 1 ] && R64="    module /boot/rootfs.cpio"
DEB="    module /boot/debian.cpio.gz
    module /boot/kng-extra.cpio"
entrada(){ # titulo kernel modulos video
  printf 'menuentry "%s" {\n    %s\n    multiboot /boot/%s\n%s\n    boot\n}\n' "$1" "$4" "$2" "$3"; }
{
  echo 'set timeout=8'
  echo 'set default=0'
  echo 'insmod all_video'
  echo 'insmod gzio'
  if [ "$DEBIAN" = 1 ]; then
    entrada "Königreich OS - 64 bits con Debian (grafico)" kng-kernel64.elf "$DEB" "$VIDEO"
    entrada "Königreich OS - 64 bits con Debian (modo texto)" kng-kernel64-text.elf "$DEB" "$TEXTO"
  fi
  entrada "Königreich OS - 64 bits (grafico)" kng-kernel64.elf "$R64" "$VIDEO"
  entrada "Königreich OS - 32 bits (grafico)" kng-kernel.elf "" "$VIDEO"
  entrada "Königreich OS - 64 bits (modo texto)" kng-kernel64-text.elf "$R64" "$TEXTO"
  entrada "Königreich OS - 32 bits (modo texto)" kng-kernel-text.elf "" "$TEXTO"
  echo 'submenu "Resolucion de pantalla (elige una y vuelve al menu)" {'
  for r in 1920x1080x32 1600x900x32 1366x768x32 1280x1024x32 1280x720x32 1024x768x32 800x600x32; do
    printf '  menuentry "%s" { set kng_res=%s; export kng_res; configfile /boot/grub/grub.cfg }\n' "$r" "$r"
  done
  echo '}'
  echo 'menuentry "Apagar" { halt }'
} > build/iso/boot/grub/grub.cfg
# Instalador: imagen de disco arrancable (MBR + GRUB BIOS/UEFI + FAT32) que kng copia al disco duro de la PC.
if [ "$KNG_SIN_INSTALADOR" != 1 ] && bash tools/mkdisk.sh; then
  DEBIAN=$DEBIAN python3 - <<'PY'
import os
p='build/iso/boot/grub/grub.cfg'; s=open(p).read()
e=("menuentry \"INSTALAR Königreich OS en el disco duro de esta PC\" {\n"
   "    if [ \"$grub_platform\" = efi ]; then set gfxpayload=keep; else set gfxpayload=1024x768x32,1280x1024x32,1366x768x32,800x600x32,1024x768x24,800x600x24; fi\n"
   "    echo \"Cargando el instalador (puede tardar 1 o 2 minutos)...\"\n"
   "    multiboot /boot/kng-kernel64.elf instalador\n"
   + ("    module /boot/debian.cpio.gz\n    module /boot/kng-extra.cpio\n" if os.environ.get("DEBIAN")=="1" else "") +
   "    module /boot/disco.img.gz\n"
   "    boot\n}\n")
i=s.index("menuentry"); s=s[:i]+e+s[i:]
open(p,'w').write(s)
PY
  echo "[iso] con instalador en el disco duro"
else echo "[aviso] ISO sin instalador (falta sudo/losetup/dosfstools o KNG_SIN_INSTALADOR=1)"; rm -f build/iso/boot/disco.img.gz; fi
# ISO hibrida: arranca con BIOS/Legacy y, si esta instalado grub-efi-amd64-bin, tambien en PCs solo UEFI.
# (En UEFI sin "CSM/Legacy" algunos PC no emulan teclado/raton PS/2: si no responden, activa Legacy/CSM en la BIOS.)
if [ -d /usr/lib/grub/x86_64-efi ]; then
  grub-mkrescue -o out/KonigreichOS-kng.iso build/iso 2>&1 | tail -3
else
  echo "[aviso] sin grub-efi-amd64-bin: el ISO solo arrancara en modo BIOS/Legacy (sudo apt install grub-efi-amd64-bin)"
  grub-mkrescue -d /usr/lib/grub/i386-pc -o out/KonigreichOS-kng.iso build/iso 2>&1 | tail -3
fi
( cd out && sha256sum KonigreichOS-kng.iso > KonigreichOS-kng.iso.sha256 )
ls -la out/KonigreichOS-kng.iso
echo "Listo: out/KonigreichOS-kng.iso"
echo "PC real o VM: mejor en BIOS/Legacy (CSM). RAM: 256 MB o mas (con Debian: 4 GB o mas). Red: solo tarjetas Intel e1000/RTL8139 (en PC real puede no haber red)."
echo "USB booteable: Rufus en modo \"imagen DD\" o balenaEtcher."
