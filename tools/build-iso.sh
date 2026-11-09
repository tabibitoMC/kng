#!/usr/bin/env bash
# Genera out/KonigreichOS-kng.iso (arranque BIOS con GRUB) con los kernels de 32 y de 64 bits. Sin Debian, sin Linux.
set -e
cd "$(dirname "$0")/.."
faltan=""
for c in gcc make grub-mkrescue xorriso mformat objcopy python3; do command -v "$c" >/dev/null || faltan="$faltan $c"; done
if [ -n "$faltan" ] || [ ! -d /usr/lib/grub/i386-pc ]; then
  echo "Faltan herramientas:$faltan"
  echo "Instala con:  sudo apt update && sudo apt install -y build-essential gcc-multilib grub-pc-bin xorriso mtools python3"
  exit 1
fi
make -C kernel clean >/dev/null; make -C kernel64 clean >/dev/null
make -C kernel      # kernel de 32 bits
make -C kernel64    # kernel de 64 bits
rm -rf build/iso && mkdir -p build/iso/boot/grub out
cp kernel/kng-kernel.elf kernel/kng-kernel-text.elf kernel64/kng-kernel64.elf kernel64/kng-kernel64-text.elf build/iso/boot/
# rootfs (programas Linux + enlazador dinamico + libc del PC) como modulo de arranque; solo lo usa el kernel de 64 bits
if python3 tools/mkrootfs.py build/iso/boot/rootfs.cpio; then ROOTFS=1; else ROOTFS=0; echo "[aviso] sin rootfs (falta libc estatica: sudo apt install libc6-dev). El ISO igual arranca, sin programas Linux."; fi
python3 tools/check-mb.py build/iso/boot/*.elf
cat > build/iso/boot/grub/grub.cfg <<'CFG'
set timeout=6
set default=0
insmod all_video
menuentry "Königreich OS - 32 bits (grafico)" {
    set gfxpayload=1024x768x32
    multiboot /boot/kng-kernel.elf
    boot
}
menuentry "Königreich OS - 64 bits (grafico)" {
    set gfxpayload=1024x768x32
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
# -d fuerza la plataforma BIOS (i386-pc): el kernel usa Multiboot 1, que GRUB UEFI no carga.
grub-mkrescue -d /usr/lib/grub/i386-pc -o out/KonigreichOS-kng.iso build/iso 2>&1 | tail -3
( cd out && sha256sum KonigreichOS-kng.iso > KonigreichOS-kng.iso.sha256 )
ls -la out/KonigreichOS-kng.iso
echo "Listo: out/KonigreichOS-kng.iso"
echo "Maquina virtual: BIOS/Legacy (no UEFI), 256 MB de RAM o mas, tarjeta de red Intel e1000 (o RTL8139) en NAT, raton PS/2."
