#
# 项目构建脚本说明
# =================
# 这个 Makefile 负责把 32 位内核的 C / 汇编源码编译成 ELF，
# 再打包成可由 GRUB 启动的 ISO 镜像，并提供 QEMU 运行入口。
#
# 主要目标：
# - all      : 完整构建并生成 myos.iso
# - check    : 用 grub-file 检查内核是否带有 multiboot 头
# - run      : 创建磁盘镜像并在 QEMU 中启动
# - clean    : 删除中间文件和产物
#
CC      = gcc
LD      = ld

CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -nostdinc -Wall -Wextra
LDFLAGS = -m elf_i386 -T linker.ld

C_SOURCES = \
	kernel/gdt.c \
	kernel/kernel.c \
	kernel/memdemo.c \
	kernel/process.c \
	kernel/usermode.c \
	mm/pmm.c \
	mm/pager.c \
	mm/vmm.c \
	console/console.c \
	interrupt/interrupts.c \
	drivers/ata.c \
	drivers/keyboard.c \
	fs/simplefs.c \
	include/string.c \
	shell/shell.c \
	timer/timer.c

C_OBJECTS = $(C_SOURCES:.c=.o)

all: check myos.iso

# 启动汇编、GDT/TSS 刷新、用户态切换和中断桩都单独编译成目标文件。
boot/boot.o: boot/boot.s
	$(CC) -m32 -c $< -o $@

kernel/gdt_flush.o: kernel/gdt_flush.s
	$(CC) -m32 -c $< -o $@

interrupt/interrupt_stubs.o: interrupt/interrupt_stubs.s
	$(CC) -m32 -c $< -o $@

kernel/usermode_switch.o: kernel/usermode_switch.s
	$(CC) -m32 -c $< -o $@

kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

console/%.o: console/%.c
	$(CC) $(CFLAGS) -c $< -o $@

drivers/%.o: drivers/%.c
	$(CC) $(CFLAGS) -c $< -o $@

fs/%.o: fs/%.c
	$(CC) $(CFLAGS) -c $< -o $@

include/%.o: include/%.c
	$(CC) $(CFLAGS) -c $< -o $@

shell/%.o: shell/%.c
	$(CC) $(CFLAGS) -c $< -o $@

interrupt/%.o: interrupt/%.c
	$(CC) $(CFLAGS) -c $< -o $@

timer/%.o: timer/%.c
	$(CC) $(CFLAGS) -c $< -o $@

mm/%.o: mm/%.c
	$(CC) $(CFLAGS) -c $< -o $@

myos.bin: boot/boot.o kernel/gdt_flush.o kernel/usermode_switch.o interrupt/interrupt_stubs.o $(C_OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ boot/boot.o kernel/gdt_flush.o kernel/usermode_switch.o interrupt/interrupt_stubs.o $(C_OBJECTS)

# GRUB 的 multiboot 检查能尽早发现启动头缺失等问题。
check: myos.bin
	grub-file --is-x86-multiboot myos.bin

iso/boot/myos.bin: myos.bin
	cp myos.bin iso/boot/myos.bin

# 使用 grub-mkrescue 生成可直接挂载到虚拟机的启动 ISO。
myos.iso: iso/boot/myos.bin iso/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso iso

# 额外准备一块 16MiB 的原始磁盘镜像，供 ATA / SimpleFS 使用。
disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=16

# 运行时同时挂载光盘镜像和 IDE 磁盘镜像，便于演示文件系统与分页换出。
run: all disk.img
	qemu-system-i386 -m 128M -boot d -cdrom myos.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk

clean:
	rm -f boot/*.o kernel/*.o mm/*.o console/*.o interrupt/*.o drivers/*.o fs/*.o include/*.o shell/*.o timer/*.o
	rm -f myos.bin myos.iso iso/boot/myos.bin

.PHONY: all check run clean
