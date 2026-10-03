CROSS_CXX := $(shell command -v x86_64-elf-g++ 2>/dev/null)
CROSS_LD  := $(shell command -v x86_64-elf-ld 2>/dev/null)

CXX := $(if $(CROSS_CXX),$(CROSS_CXX),g++)
LD  := $(if $(CROSS_LD),$(CROSS_LD),ld)
ASM := nasm

KDIR      := kernel
BUILD     := build
ISO_ROOT  := iso_root
LIMINE    := limine
DISK_IMG  := disk.img
DISK_SIZE := 64M
OVMF      ?= $(firstword $(wildcard /usr/share/edk2/x64/OVMF.4m.fd /usr/share/edk2/x64/OVMF.fd /usr/share/edk2/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/ovmf/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/qemu/OVMF.fd))

LIMINE_REPO   := https://github.com/limine-bootloader/limine.git
LIMINE_BRANCH := v9.x-binary

OPT ?= -O2

CXXFLAGS := -std=c++20 $(OPT) -Wall -Wextra \
            -fno-strict-aliasing -fno-tree-loop-distribute-patterns \
            -ffreestanding -fno-stack-protector -fno-stack-check \
            -fno-lto -fno-pic -fno-pie -fno-exceptions -fno-rtti \
            -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only \
            -mcmodel=kernel -m64 -Wa,--noexecstack \
            -I$(KDIR)/include -I$(LIMINE)

FLAGS_STAMP := $(BUILD)/.cxxflags

ASMFLAGS := -f elf64

LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 \
           -T $(KDIR)/linker.ld

CXX_SOURCES := $(shell find $(KDIR)/src -name '*.cpp')
ASM_SOURCES := $(shell find $(KDIR)/src -name '*.asm')
OBJECTS := $(CXX_SOURCES:%.cpp=$(BUILD)/%.o) $(ASM_SOURCES:%.asm=$(BUILD)/%.o)

KERNEL_ELF := $(BUILD)/kernel.elf
ISO := $(BUILD)/aphelion.iso

.PHONY: all iso run run-smp run-headless run-uefi run-uefi-smp run-uefi-headless run-ahci run-uefi-ahci run-nvme run-uefi-nvme run-nodisk test-stellar clean distclean

all: $(KERNEL_ELF)

$(LIMINE)/limine.h:
	git clone $(LIMINE_REPO) --branch=$(LIMINE_BRANCH) --depth=1 $(LIMINE)
	$(MAKE) -C $(LIMINE)

.PHONY: FORCE
FORCE:

$(FLAGS_STAMP): FORCE
	@mkdir -p $(BUILD)
	@echo '$(CXXFLAGS)' | cmp -s - $@ || echo '$(CXXFLAGS)' > $@

$(BUILD)/%.o: %.cpp $(LIMINE)/limine.h $(FLAGS_STAMP)
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

$(DISK_IMG):
	qemu-img create -f raw $(DISK_IMG) $(DISK_SIZE)

.PHONY: reset-disk
reset-disk:
	rm -f $(DISK_IMG)

run: iso $(DISK_IMG)
	qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio -no-reboot -no-shutdown

run-smp: iso $(DISK_IMG)
	qemu-system-x86_64 -M q35 -cpu max -m 256M -smp 4 -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio -no-reboot -no-shutdown

run-headless: iso $(DISK_IMG)
	qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio -display none -no-reboot -no-shutdown

run-uefi: iso $(DISK_IMG)
	@test -n "$(OVMF)" || { echo "OVMF firmware not found; pass OVMF=/path/to/OVMF.fd"; exit 1; }
	qemu-system-x86_64 -M q35 -cpu max -m 256M -bios $(OVMF) -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio -no-reboot -no-shutdown

run-uefi-smp: iso $(DISK_IMG)
	@test -n "$(OVMF)" || { echo "OVMF firmware not found; pass OVMF=/path/to/OVMF.fd"; exit 1; }
	qemu-system-x86_64 -M q35 -cpu max -m 256M -smp 4 -bios $(OVMF) -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio -no-reboot -no-shutdown

run-uefi-headless: iso $(DISK_IMG)
	@test -n "$(OVMF)" || { echo "OVMF firmware not found; pass OVMF=/path/to/OVMF.fd"; exit 1; }
	qemu-system-x86_64 -M q35 -cpu max -m 256M -bios $(OVMF) -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio -display none -no-reboot -no-shutdown

SMP        ?= 4
QEMU_Q35    = qemu-system-x86_64 -M q35 -cpu max -m 256M -smp $(SMP)
DRIVE_AHCI  = -drive file=$(DISK_IMG),if=none,id=ahd0,format=raw -device ide-hd,drive=ahd0,bus=ide.0
DRIVE_NVME  = -drive file=$(DISK_IMG),if=none,id=nvm0,format=raw -device nvme,drive=nvm0,serial=aphelion0
QEMU_TAIL   = -serial stdio -no-reboot -no-shutdown

run-ahci: iso $(DISK_IMG)
	$(QEMU_Q35) -cdrom $(ISO) $(DRIVE_AHCI) $(QEMU_TAIL)

run-uefi-ahci: iso $(DISK_IMG)
	@test -n "$(OVMF)" || { echo "OVMF firmware not found; pass OVMF=/path/to/OVMF.fd"; exit 1; }
	$(QEMU_Q35) -bios $(OVMF) -cdrom $(ISO) $(DRIVE_AHCI) $(QEMU_TAIL)

run-nvme: iso $(DISK_IMG)
	$(QEMU_Q35) -cdrom $(ISO) $(DRIVE_NVME) $(QEMU_TAIL)

run-uefi-nvme: iso $(DISK_IMG)
	@test -n "$(OVMF)" || { echo "OVMF firmware not found; pass OVMF=/path/to/OVMF.fd"; exit 1; }
	$(QEMU_Q35) -bios $(OVMF) -cdrom $(ISO) $(DRIVE_NVME) $(QEMU_TAIL)

run-nodisk: iso
	qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown

test-stellar:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined -fno-sanitize=alignment -I$(KDIR)/include $(KDIR)/src/fs/stellar.cpp tools/stellar_host_test.cpp -o $(BUILD)/stellar_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/stellar_host_test

clean:
	rm -rf $(BUILD) $(ISO_ROOT)/boot/kernel.elf

distclean: clean
	rm -rf $(LIMINE) $(ISO_ROOT)
