#!/bin/sh
# Prueba automática: arranca kng-kernel en QEMU, ejecuta 'selftest' por el puerto serie y busca "SELFTEST OK".
cd "$(dirname "$0")" || exit 1
command -v qemu-system-i386 >/dev/null || { echo "falta qemu-system-i386 (sudo apt install qemu-system-x86)"; exit 2; }
make -s || exit 1
out=$( { sleep 3; printf 'selftest\n'; sleep 20; printf 'halt\n'; sleep 1; } | timeout 40 qemu-system-i386 -kernel kng-kernel-text.elf -display none -serial stdio -no-reboot -m 256M 2>&1 )
echo "$out"
echo "$out" | grep -q 'SELFTEST OK' && { echo "== TEST OK =="; exit 0; }
echo "== TEST FALLO =="; exit 1
