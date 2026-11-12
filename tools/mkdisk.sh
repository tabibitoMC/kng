#!/usr/bin/env bash
# Imagen de disco instalable: MBR + GRUB (BIOS y UEFI) + particion FAT32 "KNGBOOT" con los kernels y modulos.
# El instalador de kng la copia tal cual al disco duro de la PC y despues crea otra particion (tipo 0xDA) para los datos.
# Uso: bash tools/mkdisk.sh  (despues de generar build/iso/boot). Necesita sudo (losetup, mount) y dosfstools.
set -e
cd "$(dirname "$0")/.."
SRC=build/iso/boot
for c in sfdisk losetup mkfs.vfat grub-install; do command -v $c >/dev/null || { echo "[mkdisk] falta $c (sudo apt install fdisk dosfstools grub-pc-bin grub-efi-amd64-bin)"; exit 1; }; done
need=$(du -sm --exclude=disco.img.gz --exclude=disco.img $SRC | cut -f1)
SZ=$((need + 48)); [ $SZ -lt 64 ] && SZ=64          # MB de la particion de arranque (FAT32 necesita al menos ~33 MB)
IMG=build/kng-disco.img; MNT=build/dmnt
rm -f $IMG; truncate -s $((SZ + 1))M $IMG
printf 'label: dos\nstart=2048, type=ef, bootable\n' | sfdisk -q $IMG
LOOP=$(sudo losetup -P --show -f $IMG)
limpiar(){ sudo umount $MNT 2>/dev/null || true; sudo losetup -d $LOOP 2>/dev/null || true; }
trap limpiar EXIT
for i in 1 2 3 4 5; do [ -b ${LOOP}p1 ] && break; sleep 1; sudo partprobe $LOOP 2>/dev/null || true; done
[ -b ${LOOP}p1 ] || { echo "[mkdisk] el sistema no creo ${LOOP}p1 (losetup -P)"; exit 1; }
sudo mkfs.vfat -F 32 -n KNGBOOT ${LOOP}p1 >/dev/null
mkdir -p $MNT; sudo mount ${LOOP}p1 $MNT
sudo mkdir -p $MNT/boot/grub
for f in $SRC/*; do case "$(basename $f)" in disco.img*|grub) ;; *) sudo cp -r "$f" $MNT/boot/ ;; esac; done
sudo cp $SRC/grub/grub.cfg $MNT/boot/grub/grub.cfg
sudo grub-install --target=i386-pc --boot-directory=$MNT/boot --modules="part_msdos fat" $LOOP 2>&1 | tail -1
if [ -d /usr/lib/grub/x86_64-efi ]; then sudo grub-install --target=x86_64-efi --efi-directory=$MNT --boot-directory=$MNT/boot --removable --no-nvram 2>&1 | tail -1; fi
sync; limpiar; trap - EXIT
gzip -6 -c $IMG > $SRC/disco.img.gz
echo "[mkdisk] imagen de disco: $(du -h $IMG | cut -f1) (comprimida $(du -h $SRC/disco.img.gz | cut -f1))"
