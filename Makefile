ARCH := x86_64
CROSS ?= x86_64-elf-

CC := $(shell command -v $(CROSS)gcc >/dev/null 2>&1 && echo $(CROSS)gcc || echo gcc)
LD := $(shell command -v $(CROSS)ld >/dev/null 2>&1 && echo $(CROSS)ld || echo ld)
AS := nasm
HOSTCC := gcc

CFLAGS := -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-pic -m64 -mno-red-zone -Iinclude -Igfx -Wall -Wextra
LDFLAGS := -T linker.ld -nostdlib
HOSTCFLAGS := -O2 -Wall -Wextra

BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/iso

BOOT_OBJ := $(BUILD_DIR)/boot.o
ISR_OBJ := $(BUILD_DIR)/isr.o
KERNEL_BIN := $(BUILD_DIR)/kernel.bin
ISO_OUT := os.iso

SRC_DIRS := kernel memory process gfx ui desktop input shell fs services apps theme debug net
C_SRCS := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
C_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS))

FONT_TTF ?= /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
FONT_UI_PX ?= 18
FONT_BOOT_PX ?= 12
FONT_BAKE := $(BUILD_DIR)/font_bake
FONT_UI_C := $(BUILD_DIR)/generated/font_ui.c
FONT_BOOT_C := $(BUILD_DIR)/generated/font_boot.c
FONT_UI_O := $(BUILD_DIR)/generated/font_ui.o
FONT_BOOT_O := $(BUILD_DIR)/generated/font_boot.o
FREETYPE_SO ?= /lib/x86_64-linux-gnu/libfreetype.so.6

# GRUB (boot menu) font: keep it small so the menu doesn't look oversized at 720p+.
GRUB_FONT_TTF ?= /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
GRUB_FONT_PX ?= 12
GRUB_FONT_PF2 := $(ISO_DIR)/boot/grub/smileos.pf2

.PHONY: all clean iso run

all: $(ISO_OUT)

QEMU := qemu-system-x86_64
QEMUFLAGS_BASE := -m 512 -smp 2 -cpu max
QEMUFLAGS_KVM := -accel kvm
QEMUFLAGS_TCG := -accel tcg

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT_OBJ): boot/boot.asm | $(BUILD_DIR)
	$(AS) -f elf64 $< -o $@

$(ISR_OBJ): boot/isr.asm | $(BUILD_DIR)
	$(AS) -f elf64 $< -o $@

$(FONT_BAKE): tools/font_bake.c | $(BUILD_DIR)
	$(HOSTCC) $(HOSTCFLAGS) $< -o $@ $(FREETYPE_SO)

$(BUILD_DIR)/generated: | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/generated

$(FONT_UI_C): $(FONT_BAKE) | $(BUILD_DIR)/generated
	$(FONT_BAKE) $(FONT_TTF) $(FONT_UI_PX) $(FONT_UI_C) ui

$(FONT_BOOT_C): $(FONT_BAKE) | $(BUILD_DIR)/generated
	$(FONT_BAKE) $(FONT_TTF) $(FONT_BOOT_PX) $(FONT_BOOT_C) boot

$(FONT_UI_O): $(FONT_UI_C)
	$(CC) $(CFLAGS) -c $< -o $@

$(FONT_BOOT_O): $(FONT_BOOT_C)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(FONT_UI_O) $(FONT_BOOT_O) $(BOOT_OBJ) $(ISR_OBJ) $(C_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(BOOT_OBJ) $(ISR_OBJ) $(FONT_UI_O) $(FONT_BOOT_O) $(C_OBJS)

$(ISO_OUT): $(KERNEL_BIN) grub/grub.cfg
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	cp grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkfont -s $(GRUB_FONT_PX) -o $(GRUB_FONT_PF2) $(GRUB_FONT_TTF)
	grub-mkrescue -o $(ISO_OUT) $(ISO_DIR)

iso: $(ISO_OUT)

run: $(ISO_OUT)
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_KVM) -cdrom $(ISO_OUT) || \
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_TCG) -cdrom $(ISO_OUT)

.PHONY: run-uhci run-xhci run-usb-mouse run-serial run-net
run-xhci: $(ISO_OUT)
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_KVM) -cdrom $(ISO_OUT) -device qemu-xhci || \
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_TCG) -cdrom $(ISO_OUT) -device qemu-xhci

run-usb-mouse: $(ISO_OUT)
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_KVM) -cdrom $(ISO_OUT) -device qemu-xhci -device usb-mouse || \
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_TCG) -cdrom $(ISO_OUT) -device qemu-xhci -device usb-mouse

run-serial: $(ISO_OUT)
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_KVM) -cdrom $(ISO_OUT) -serial stdio || \
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_TCG) -cdrom $(ISO_OUT) -serial stdio

run-net: $(ISO_OUT)
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_KVM) -cdrom $(ISO_OUT) -netdev user,id=n1 -device e1000,netdev=n1 || \
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_TCG) -cdrom $(ISO_OUT) -netdev user,id=n1 -device e1000,netdev=n1

run-uhci: $(ISO_OUT)
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_KVM) -cdrom $(ISO_OUT) -device piix3-usb-uhci -device usb-mouse || \
	$(QEMU) $(QEMUFLAGS_BASE) $(QEMUFLAGS_TCG) -cdrom $(ISO_OUT) -device piix3-usb-uhci -device usb-mouse

clean:
	rm -rf $(BUILD_DIR) $(ISO_OUT)
