#!/usr/bin/env bash
# Genera out/KonigreichOS-kng.iso (arranque BIOS con GRUB) con los kernels de 32 y de 64 bits. Sin Debian, sin Linux.
set -e
cd "$(dirname "$0")/.."
faltan=""
for c in gcc make grub-mkrescue xorriso mformat objcopy python3; do command -v "$c" >/dev/null || faltan="$faltan $c"; done
if [ -n "$faltan" ] || [ ! -d /usr/lib/grub/i386-pc ]; then
  echo "Faltan herramientas:$faltan"
  echo "Instala con:  sudo apt update && sudo apt install -y build-essential gcc-multilib grub-pc-bin grub-efi-amd64-bin xorriso mtools python3"
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
if [ "$KNG_DEBIAN" = 1 ] && [ ! -f build/debian.cpio.gz ]; then bash tools/mkdebian.sh; fi
if [ -f build/debian.cpio.gz ]; then
  cp build/debian.cpio.gz build/iso/boot/debian.cpio.gz
  python3 tools/mkrootfs.py --debian build/iso/boot/kng-extra.cpio
  DEBIAN=1
else DEBIAN=0; echo "[aviso] sin Debian (para incluirlo:  KNG_DEBIAN=1 bash tools/build-iso.sh )."; fi
python3 tools/check-mb.py build/iso/boot/*.elf
cat > build/iso/boot/grub/grub.cfg <<'CFG'
set timeout=6
set default=1
insmod all_video
insmod gzio
menuentry "Königreich OS - 32 bits (grafico)" {
    set gfxpayload=1024x768x32,1280x1024x32,1366x768x32,1280x720x32,1920x1080x32,800x600x32,1024x768x24,1280x1024x24,800x600x24
    multiboot /boot/kng-kernel.elf
    boot
}
menuentry "Königreich OS - 64 bits (grafico)" {
    set gfxpayload=1024x768x32,1280x1024x32,1366x768x32,1280x720x32,1920x1080x32,800x600x32,1024x768x24,1280x1024x24,800x600x24
    multiboot /boot/kng-kernel64.elf
    module /boot/rootfs.cpio
    boot
}
menuentry "Königreich OS - 32 bits (modo texto)" {
    set gfxpayload=text
    multiboot /boot/kng-kernel-text.elf
    boot
}
menuentry "Königreich OS - 64 bits (modo texto)" {
    set gfxpayload=text
    multiboot /boot/kng-kernel64-text.elf
    module /boot/rootfs.cpio
    boot
}
CFG
[ "$ROOTFS" = 1 ] || sed -i '/rootfs.cpio/d' build/iso/boot/grub/grub.cfg
if [ "$DEBIAN" = 1 ]; then      # con Debian: sus archivos primero y los programas de kng encima (sin la libc del PC, que no es la de Debian)
  cat > build/iso/boot/grub/grub.cfg <<'CFG'
set timeout=6
set default=0
insmod all_video
insmod gzio
menuentry "Königreich OS - 64 bits con Debian (grafico)" {
    set gfxpayload=1024x768x32,1280x1024x32,1366x768x32,1280x720x32,1920x1080x32,800x600x32,1024x768x24,1280x1024x24,800x600x24
    multiboot /boot/kng-kernel64.elf
    module /boot/debian.cpio.gz
    module /boot/kng-extra.cpio
    boot
}
menuentry "Königreich OS - 64 bits con Debian (modo texto)" {
    set gfxpayload=text
    multiboot /boot/kng-kernel64-text.elf
    module /boot/debian.cpio.gz
    module /boot/kng-extra.cpio
    boot
}
menuentry "Königreich OS - 64 bits (grafico, sin Debian)" {
    set gfxpayload=1024x768x32,1280x1024x32,1366x768x32,1280x720x32,1920x1080x32,800x600x32,1024x768x24,1280x1024x24,800x600x24
    multiboot /boot/kng-kernel64.elf
    module /boot/rootfs.cpio
    boot
}
menuentry "Königreich OS - 32 bits (grafico)" {
    set gfxpayload=1024x768x32,1280x1024x32,1366x768x32,1280x720x32,1920x1080x32,800x600x32,1024x768x24,1280x1024x24,800x600x24
    multiboot /boot/kng-kernel.elf
    boot
}
CFG
fi
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
