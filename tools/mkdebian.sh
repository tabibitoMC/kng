#!/usr/bin/env bash
# Crea build/debian.cpio.gz: un Debian 12 (bookworm) REAL con apt/dpkg, curl, busybox y los comandos en espanol de kng.
# Se usa como modulo de arranque de kng 64 bits. Necesita sudo y debootstrap:  sudo apt install -y debootstrap cpio gzip
set -e
cd "$(dirname "$0")/.."
for c in debootstrap cpio gzip sudo; do command -v "$c" >/dev/null || { echo "Falta '$c'. Instala con:  sudo apt update && sudo apt install -y debootstrap cpio gzip"; exit 1; }; done
RAIZ=${KNG_DEBROOT:-/var/tmp/kng-debroot}   # disco de Linux (en /mnt/c, NTFS, debootstrap falla)
sudo rm -rf "$RAIZ"; mkdir -p build
echo "[1/4] Descargando Debian base (unos minutos, necesita internet)..."
sudo debootstrap --variant=minbase \
  --include=busybox,curl,ca-certificates,xz-utils,less,nano,procps,xauth,gnupg \
  bookworm "$RAIZ" http://deb.debian.org/debian > build/debootstrap.log 2>&1 || { tail -30 build/debootstrap.log; exit 1; }
echo "[2/4] Agregando comandos en espanol y ajustes de kng..."
sudo cp -a tools/rootfs-es/. "$RAIZ"/
sudo mkdir -p "$RAIZ"/root "$RAIZ"/tmp "$RAIZ"/run "$RAIZ"/var/lib/xvfb "$RAIZ"/tmp/.X11-unix
sudo chmod 1777 "$RAIZ"/tmp "$RAIZ"/tmp/.X11-unix
sudo tee "$RAIZ"/etc/apt/sources.list > /dev/null <<'L'
deb http://deb.debian.org/debian bookworm main
deb http://deb.debian.org/debian bookworm-updates main
deb http://security.debian.org/debian-security bookworm-security main
L
echo "kng" | sudo tee "$RAIZ"/etc/hostname > /dev/null
sudo rm -rf "$RAIZ"/var/lib/apt/lists/* "$RAIZ"/var/cache/apt/*.bin "$RAIZ"/var/cache/apt/archives/*.deb "$RAIZ"/usr/share/doc/* "$RAIZ"/usr/share/man/* "$RAIZ"/usr/share/info/*
echo "[3/4] Empaquetando..."
( cd "$RAIZ" && sudo find . | sudo cpio -o -H newc --quiet ) | gzip -6 > build/debian.cpio.gz
echo "[4/4] Listo: build/debian.cpio.gz ($(du -h build/debian.cpio.gz | cut -f1))"
