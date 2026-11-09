# kng-kernel64: kernel de 64 bits. Comparte gfx/fs/html/gui/net con el de 32 bits (../kernel).
CC      = gcc
K       = ../kernel
KFLAGS  = -m64 -std=gnu11 -O2 -ffreestanding -fno-pie -no-pie -mcmodel=small -mno-red-zone -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin -fno-tree-loop-distribute-patterns -mgeneral-regs-only -Wall -Wextra -nostdlib -I$(K) -DKNG_ARCH='"x86_64, 64 bits"'
LDFLAGS = -m64 -T linker64.ld -nostdlib -static -no-pie -Wl,-z,noexecstack -Wl,--build-id=none -Wl,-z,max-page-size=4096
OBJS    = kernel64.o gfx.o fs.o html.o gui.o net.o disk.o kbd.o

all: kng-kernel64.elf kng-kernel64-text.elf

# Multiboot 1 solo carga ELF32: se enlaza en 64 bits y se convierte el contenedor (el codigo sigue siendo de 64 bits)
kng-kernel64.elf: boot64.o $(OBJS) linker64.ld
	$(CC) $(LDFLAGS) -o kng-kernel64.x64 boot64.o $(OBJS)
	objcopy -O elf32-i386 kng-kernel64.x64 $@
kng-kernel64-text.elf: boot64-text.o $(OBJS) linker64.ld
	$(CC) $(LDFLAGS) -o kng-kernel64-text.x64 boot64-text.o $(OBJS)
	objcopy -O elf32-i386 kng-kernel64-text.x64 $@
boot64.o: boot64.S
	$(CC) -m64 -c $< -o $@
boot64-text.o: boot64.S
	$(CC) -m64 -DNOVIDEO -c $< -o $@
kernel64.o: kernel64.c $(K)/kng.h $(K)/gui.h $(K)/net.h
	$(CC) $(KFLAGS) -c $< -o $@
%.o: $(K)/%.c $(K)/kng.h $(K)/gfx.h $(K)/gui.h $(K)/fs.h $(K)/html.h $(K)/net.h $(K)/disk.h $(K)/kbd.h $(K)/net_pcnet_virtio.h
	$(CC) $(KFLAGS) -c $< -o $@
run: kng-kernel64-text.elf
	qemu-system-x86_64 -kernel kng-kernel64-text.elf -serial stdio -m 256M -netdev user,id=n0 -device e1000,netdev=n0
clean:
	rm -f *.o *.x64 kng-kernel64*.elf
