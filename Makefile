CROSS_CXX := $(shell command -v x86_64-elf-g++ 2>/dev/null)
CROSS_LD  := $(shell command -v x86_64-elf-ld 2>/dev/null)

CXX := $(if $(CROSS_CXX),$(CROSS_CXX),g++)
LD  := $(if $(CROSS_LD),$(CROSS_LD),ld)
ASM := nasm

KDIR      := kernel
BUILD     := build
ISO_ROOT  := iso_root
LIMINE    := limine

LIMINE_REPO   := https://github.com/limine-bootloader/limine.git
LIMINE_BRANCH := v9.x-binary

CXXFLAGS := -std=c++20 -Wall -Wextra \
            -ffreestanding -fno-stack-protector -fno-stack-check \
            -fno-lto -fno-pic -fno-pie -fno-exceptions -fno-rtti \
            -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only \
            -mcmodel=kernel -m64 -Wa,--noexecstack \
            -I$(KDIR)/include -I$(LIMINE)

ASMFLAGS := -f elf64

LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 \
           -T $(KDIR)/linker.ld

CXX_SOURCES := $(shell find $(KDIR)/src -name '*.cpp')
ASM_SOURCES := $(shell find $(KDIR)/src -name '*.asm')
OBJECTS := $(CXX_SOURCES:%.cpp=$(BUILD)/%.o) $(ASM_SOURCES:%.asm=$(BUILD)/%.o)

KERNEL_ELF := $(BUILD)/kernel.elf
ISO := $(BUILD)/aphelion.iso

.PHONY: all iso run run-smp run-headless clean distclean

all: $(KERNEL_ELF)

$(LIMINE)/limine.h:
	git clone $(LIMINE_REPO) --branch=$(LIMINE_BRANCH) --depth=1 $(LIMINE)
	$(MAKE) -C $(LIMINE)

$(BUILD)/%.o: %.cpp $(LIMINE)/limine.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

$(KERNEL_ELF): $(OBJECTS) $(KDIR)/linker.ld
	$(LD) $(LDFLAGS) $(OBJECTS) -o $@

iso: $(KERNEL_ELF) $(LIMINE)/limine.h
	@mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL_ELF) $(ISO_ROOT)/boot/kernel.elf
	cp $(KDIR)/limine.conf $(ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin $(LIMINE)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	cp $(LIMINE)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT) -o $(ISO)
	$(LIMINE)/limine bios-install $(ISO)

run: iso
	qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown

run-smp: iso
	qemu-system-x86_64 -M q35 -cpu max -m 256M -smp 4 -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown

run-headless: iso
	qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom $(ISO) -serial stdio -display none -no-reboot -no-shutdown

clean:
	rm -rf $(BUILD) $(ISO_ROOT)/boot/kernel.elf

distclean: clean
	rm -rf $(LIMINE) $(ISO_ROOT)
