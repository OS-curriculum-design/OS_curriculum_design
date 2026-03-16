CC      = gcc
LD      = ld

CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -nostdinc -Wall -Wextra
LDFLAGS = -m elf_i386 -T linker.ld

C_SOURCES = \
	kernel/kernel.c \
	console/console.c \
	interrupt/interrupts.c \
	drivers/keyboard.c \
	drivers/mouse.c \
	include/string.c \
	memory/memory.c \
	shell/shell.c \
	timer/timer.c

C_OBJECTS = $(C_SOURCES:.c=.o)

all: check myos.iso

boot/boot.o: boot/boot.s
	$(CC) -m32 -c $< -o $@

interrupt/interrupt_stubs.o: interrupt/interrupt_stubs.s
	$(CC) -m32 -c $< -o $@

kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

console/%.o: console/%.c
	$(CC) $(CFLAGS) -c $< -o $@

drivers/%.o: drivers/%.c
	$(CC) $(CFLAGS) -c $< -o $@

include/%.o: include/%.c
	$(CC) $(CFLAGS) -c $< -o $@

memory/%.o: memory/%.c
	$(CC) $(CFLAGS) -c $< -o $@

shell/%.o: shell/%.c
	$(CC) $(CFLAGS) -c $< -o $@

interrupt/%.o: interrupt/%.c
	$(CC) $(CFLAGS) -c $< -o $@

timer/%.o: timer/%.c
	$(CC) $(CFLAGS) -c $< -o $@

myos.bin: boot/boot.o interrupt/interrupt_stubs.o $(C_OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ boot/boot.o interrupt/interrupt_stubs.o $(C_OBJECTS)

check: myos.bin
	grub-file --is-x86-multiboot myos.bin

iso/boot/myos.bin: myos.bin
	cp myos.bin iso/boot/myos.bin

myos.iso: iso/boot/myos.bin iso/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso iso

run: all
	qemu-system-i386 -cdrom myos.iso

clean:
	rm -f boot/*.o kernel/*.o console/*.o interrupt/*.o drivers/*.o include/*.o memory/*.o shell/*.o timer/*.o
	rm -f myos.bin myos.iso iso/boot/myos.bin

.PHONY: all check run clean
