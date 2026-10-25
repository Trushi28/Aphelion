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

QEMU_EXIT ?=

PROFILE      ?= dev
BOOT_DEFAULT ?= shell
MENU_TIMEOUT ?= $(if $(filter usb,$(PROFILE)),5,3)

ifeq ($(filter $(PROFILE),dev usb),)
$(error PROFILE must be dev or usb)
endif
ifeq ($(filter $(BOOT_DEFAULT),shell test),)
$(error BOOT_DEFAULT must be shell or test)
endif

LIMINE_CONF := $(BUILD)/limine.conf
DEFAULT_INDEX := $(if $(filter shell,$(BOOT_DEFAULT)),1,2)
SHELL_STORAGE_WORD := $(if $(filter usb,$(PROFILE)),usb,auto)
TEST_STORAGE_WORD := $(if $(filter usb,$(PROFILE)),off,auto)
SHELL_CMDLINE := mode=shell storage=$(SHELL_STORAGE_WORD)
TEST_CMDLINE := mode=test storage=$(TEST_STORAGE_WORD)

define LIMINE_CONF_TEXT
timeout: $(MENU_TIMEOUT)
default_entry: $(DEFAULT_INDEX)
interface_branding: Aphelion

/Aphelion Shell
    protocol: limine
    kernel_path: boot():/boot/kernel.elf
    cmdline: $(SHELL_CMDLINE)

/Aphelion Test
    protocol: limine
    kernel_path: boot():/boot/kernel.elf
    cmdline: $(TEST_CMDLINE)
endef

CXXFLAGS := -std=c++20 $(OPT) -Wall -Wextra \
            -fno-strict-aliasing -fno-tree-loop-distribute-patterns \
            -ffreestanding -fno-stack-protector -fno-stack-check \
            -fno-lto -fno-pic -fno-pie -fno-exceptions -fno-rtti \
            -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only \
            -mcmodel=kernel -m64 -Wa,--noexecstack \
            -MMD -MP -I$(KDIR)/include -I$(LIMINE)

FPU ?= 1
ifeq ($(FPU),1)
CXXFLAGS += -DAPHELION_FPU=1
endif

FLAGS_STAMP := $(BUILD)/.cxxflags

ASMFLAGS := -f elf64 -I $(KDIR)/data/

LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 \
           -T $(KDIR)/linker.ld

CXX_SOURCES := $(shell find $(KDIR)/src -name '*.cpp')
ASM_SOURCES := $(shell find $(KDIR)/src -name '*.asm')
OBJECTS := $(CXX_SOURCES:%.cpp=$(BUILD)/%.o) $(ASM_SOURCES:%.asm=$(BUILD)/%.o)

KERNEL_ELF := $(BUILD)/kernel.elf
ISO := $(BUILD)/aphelion.iso
TEST_ISO := $(BUILD)/aphelion-test.iso

BUILD_ASM_DEPS := $(BUILD)/$(KDIR)/src/console/fontblob.o

.PHONY: test-iso run-test usb-image run-usb run-uefi-usb test-acpi test-mm test-sched test-console test-input test-lens-types test-lens test-nebula test-helm test-usb test-usbstor all iso run run-smp run-headless run-uefi run-uefi-smp run-uefi-headless run-ahci run-uefi-ahci run-nvme run-uefi-nvme run-nodisk test-stellar bench-stellar crash-fuzz stellarfs clean distclean

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

$(BUILD_ASM_DEPS): $(KDIR)/data/corona-fonts.bin

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

-include $(OBJECTS:.o=.d)

BUILD_ID_SRC := $(BUILD)/buildid.cpp
BUILD_ID_TXT := $(BUILD)/buildid.txt
BUILD_ID_OBJ := $(BUILD)/buildid.o

$(BUILD_ID_SRC): $(OBJECTS)
	@mkdir -p $(BUILD)
	@id=$$(cat $(OBJECTS) | sha256sum | cut -c1-12 | tr a-f A-F); \
	printf '%s' "$$id" > $(BUILD_ID_TXT); \
	printf 'extern "C" const char aphelion_build_id[] = "%s";\n' "$$id" > $@.new; \
	cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@

$(BUILD_ID_OBJ): $(BUILD_ID_SRC)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJECTS) $(BUILD_ID_OBJ) $(KDIR)/linker.ld
	$(LD) $(LDFLAGS) $(OBJECTS) $(BUILD_ID_OBJ) -o $@

$(LIMINE_CONF): FORCE
	@mkdir -p $(BUILD)
	$(file >$@.new,$(LIMINE_CONF_TEXT))
	@cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@

iso: $(KERNEL_ELF) $(LIMINE)/limine.h $(LIMINE_CONF)
	@mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL_ELF) $(ISO_ROOT)/boot/kernel.elf
	cp $(LIMINE_CONF) $(ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin $(LIMINE)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	cp $(LIMINE)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		-V APH$$(cat $(BUILD_ID_TXT)) $(ISO_ROOT) -o $(ISO)
	$(LIMINE)/limine bios-install $(ISO)

test-iso:
	$(MAKE) iso BOOT_DEFAULT=test ISO=$(TEST_ISO)

$(DISK_IMG):
	qemu-img create -f raw $(DISK_IMG) $(DISK_SIZE)

.PHONY: reset-disk
reset-disk:
	rm -f $(DISK_IMG)

run: iso $(DISK_IMG)
	qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio $(QEMU_EXIT)

run-test: test-iso $(DISK_IMG)
	qemu-system-x86_64 -M q35 -cpu max -m 256M -smp $(SMP) -cdrom $(TEST_ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio -display none $(QEMU_EXIT)

run-smp: iso $(DISK_IMG)
	qemu-system-x86_64 -M q35 -cpu max -m 256M -smp 4 -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio $(QEMU_EXIT)

run-headless: iso $(DISK_IMG)
	qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio -display none $(QEMU_EXIT)

run-uefi: iso $(DISK_IMG)
	@test -n "$(OVMF)" || { echo "OVMF firmware not found; pass OVMF=/path/to/OVMF.fd"; exit 1; }
	qemu-system-x86_64 -M q35 -cpu max -m 256M -bios $(OVMF) -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio $(QEMU_EXIT)

run-uefi-smp: iso $(DISK_IMG)
	@test -n "$(OVMF)" || { echo "OVMF firmware not found; pass OVMF=/path/to/OVMF.fd"; exit 1; }
	qemu-system-x86_64 -M q35 -cpu max -m 256M -smp 4 -bios $(OVMF) -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio $(QEMU_EXIT)

run-uefi-headless: iso $(DISK_IMG)
	@test -n "$(OVMF)" || { echo "OVMF firmware not found; pass OVMF=/path/to/OVMF.fd"; exit 1; }
	qemu-system-x86_64 -M q35 -cpu max -m 256M -bios $(OVMF) -cdrom $(ISO) \
		-drive file=$(DISK_IMG),if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
		-serial stdio -display none $(QEMU_EXIT)

SMP        ?= 4
QEMU_Q35    = qemu-system-x86_64 -M q35 -cpu max -m 256M -smp $(SMP)
DRIVE_AHCI  = -drive file=$(DISK_IMG),if=none,id=ahd0,format=raw -device ide-hd,drive=ahd0,bus=ide.0
DRIVE_NVME  = -drive file=$(DISK_IMG),if=none,id=nvm0,format=raw -device nvme,drive=nvm0,serial=aphelion0
QEMU_TAIL   = -serial stdio $(QEMU_EXIT)

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

USB_IMG      := $(BUILD)/usb.img
USB_IMG_SIZE ?= 256M
DRIVE_USB     = -device qemu-xhci,id=xhci -drive if=none,id=stick,file=$(USB_IMG),format=raw -device usb-storage,bus=xhci.0,drive=stick,bootindex=0

usb-image: iso
	cp $(ISO) $(USB_IMG)
	truncate -s $(USB_IMG_SIZE) $(USB_IMG)

run-usb:
	$(MAKE) PROFILE=usb usb-image
	qemu-system-x86_64 -M q35 -cpu max -m 256M -smp 2 $(DRIVE_USB) $(QEMU_TAIL)

run-uefi-usb:
	$(MAKE) PROFILE=usb usb-image
	@test -n "$(OVMF)" || { echo "OVMF firmware not found; pass OVMF=/path/to/OVMF.fd"; exit 1; }
	qemu-system-x86_64 -M q35 -cpu max -m 256M -smp 2 -bios $(OVMF) $(DRIVE_USB) $(QEMU_TAIL)

run-nodisk: iso
	qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom $(ISO) -serial stdio $(QEMU_EXIT)

TEST_UNITS := stellar mm sched console input usb usbstor lens-types lens nebula helm acpi

unit_inc_stellar  := -I$(KDIR)/include
unit_src_stellar  := $(KDIR)/src/fs/stellar.cpp tools/stellar_host_test.cpp
unit_inc_mm       := -Itools/hostshim -I$(KDIR)/include
unit_src_mm       := tools/mm_host_test.cpp
unit_inc_console  := -Itools/hostshim -I$(KDIR)/include
unit_src_console  := tools/console_host_test.cpp
unit_inc_input    := -Itools/hostshim -I$(KDIR)/include
unit_src_input    := tools/input_host_test.cpp
unit_inc_usb      := -Itools/hostshim -I$(KDIR)/include
unit_src_usb      := tools/usb_host_test.cpp
unit_inc_usbstor  := -Itools/hostshim-usb -Itools/hostshim -I$(KDIR)/include
unit_src_usbstor  := tools/usbstor_host_test.cpp
unit_inc_sched    := -Itools/hostshim -I$(KDIR)/include
unit_src_sched    := tools/sched_host_test.cpp
unit_inc_acpi     := -I$(KDIR)/include
unit_src_acpi     := tools/acpi_host_test.cpp
unit_inc_lens-types := -I$(KDIR)/include
unit_src_lens-types := tools/lens_types_host_test.cpp

FS_CORE_SRC := $(KDIR)/src/fs/stellar.cpp $(KDIR)/src/fs/nebula.cpp $(KDIR)/src/fs/realm_ram.cpp $(KDIR)/src/fs/realm_stellar.cpp
LENS_SRC    := $(KDIR)/src/fs/lens.cpp $(KDIR)/src/fs/lens_ops.cpp

unit_inc_nebula   := -I$(KDIR)/include
unit_src_nebula   := $(FS_CORE_SRC) $(KDIR)/src/fs/nebula_selftest.cpp tools/nebula_host_test.cpp
unit_inc_lens     := -I$(KDIR)/include
unit_src_lens     := $(FS_CORE_SRC) $(LENS_SRC) $(KDIR)/src/fs/lens_epoch.cpp $(KDIR)/src/fs/lens_batch.cpp $(KDIR)/src/fs/lens_stream.cpp $(KDIR)/src/fs/lens_text.cpp $(KDIR)/src/fs/realm_sys.cpp tools/lens_host_test.cpp
unit_inc_helm     := -I$(KDIR)/include
unit_src_helm     := $(FS_CORE_SRC) $(LENS_SRC) $(KDIR)/src/fs/lens_text.cpp $(KDIR)/src/shell/helm_core.cpp $(KDIR)/src/shell/helm_builtin.cpp $(KDIR)/src/shell/helm_fs.cpp $(KDIR)/src/shell/helm_text.cpp $(KDIR)/src/shell/helm_rows.cpp $(KDIR)/src/fs/lens_stream.cpp tools/helm_host_test.cpp

.PHONY: unit-deps print-units
print-units:
	@echo $(TEST_UNITS)

unit-deps:
	@test -n "$(UNIT)" || { echo "usage: make unit-deps UNIT=<$(TEST_UNITS)>" >&2; exit 2; }
	@g++ -std=c++20 -MM $(unit_inc_$(UNIT)) $(unit_src_$(UNIT)) | sed -e 's/^[^:]*: *//' -e 's/ \\$$//' | tr ' ' '\n' | sed -e '/^$$/d' -e 's,[^ /]*/\.\./,,g' | sort -u

test-stellar:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -pthread -fsanitize=address,undefined -fno-sanitize=alignment $(unit_inc_stellar) $(unit_src_stellar) -o $(BUILD)/stellar_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/stellar_host_test

test-mm:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -pthread -fsanitize=address,undefined $(unit_inc_mm) $(unit_src_mm) -o $(BUILD)/mm_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/mm_host_test

test-console:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined $(unit_inc_console) $(unit_src_console) -o $(BUILD)/console_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/console_host_test

test-input:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined $(unit_inc_input) $(unit_src_input) -o $(BUILD)/input_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/input_host_test

test-usb:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined $(unit_inc_usb) $(unit_src_usb) -o $(BUILD)/usb_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/usb_host_test

test-usbstor:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined $(unit_inc_usbstor) $(unit_src_usbstor) -o $(BUILD)/usbstor_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/usbstor_host_test

test-sched:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined $(unit_inc_sched) $(unit_src_sched) -o $(BUILD)/sched_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/sched_host_test

test-acpi:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined $(unit_inc_acpi) $(unit_src_acpi) -o $(BUILD)/acpi_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/acpi_host_test

test-lens-types:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined $(unit_inc_lens-types) $(unit_src_lens-types) -o $(BUILD)/lens_types_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/lens_types_host_test

test-lens:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -pthread -fsanitize=address,undefined -fno-sanitize=alignment $(unit_inc_lens) $(unit_src_lens) -o $(BUILD)/lens_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/lens_host_test

test-nebula:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined -fno-sanitize=alignment $(unit_inc_nebula) $(unit_src_nebula) -o $(BUILD)/nebula_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/nebula_host_test

test-helm:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined -fno-sanitize=alignment $(unit_inc_helm) $(unit_src_helm) -o $(BUILD)/helm_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/helm_host_test

test-stellar-thread:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O1 -g -w -pthread -fsanitize=thread -fno-sanitize=alignment -I$(KDIR)/include $(KDIR)/src/fs/stellar.cpp tools/stellar_host_test.cpp -o $(BUILD)/stellar_host_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/stellar_host_test

FUZZ_KIND   ?= nvme
FUZZ_CORES  ?= 4
FUZZ_ROUNDS ?= 30
FUZZ_SEED   ?= 1

crash-fuzz: test-iso stellarfs
	tools/crash-fuzz.sh $(FUZZ_KIND) $(FUZZ_CORES) $(FUZZ_ROUNDS) $(FUZZ_SEED)

bench-stellar:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O2 -w -I$(KDIR)/include $(KDIR)/src/fs/stellar.cpp tools/stellar_bench.cpp -o $(BUILD)/stellar_bench
	$(BUILD)/stellar_bench

stellarfs:
	@mkdir -p $(BUILD)
	g++ -std=c++20 -O2 -w -I$(KDIR)/include $(KDIR)/src/fs/stellar.cpp tools/stellarfs.cpp -o $(BUILD)/stellarfs

clean:
	rm -rf $(BUILD) $(ISO_ROOT)/boot/kernel.elf

distclean: clean
	rm -rf $(LIMINE) $(ISO_ROOT)
