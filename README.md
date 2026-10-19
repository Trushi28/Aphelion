```
                        ...............................
              ...........                             ...........
        .......                                                 .......
    .....         ___    ____  __  __________    ________  _   __     .....
  ...            /   |  / __ \/ / / / ____/ /   /  _/ __ \/ | / /         ...
 ..             / /| | / /_/ / /_/ / __/ / /    / // / / /  |/ /            ..
 .      *      / ___ |/ ____/ __  / /___/ /____/ // /_/ / /|  /              o
 ..           /_/  |_/_/   /_/ /_/_____/_____/___/\____/_/ |_/              ..
  ...                                                                     ...
    .....                                                             .....
        .......                                                 .......
              ...........                             ...........
                        ...............................
```

A from-scratch, non-POSIX x86_64 operating system, written in C++20.

[![CI](https://github.com/Trushi28/Aphelion/actions/workflows/ci.yml/badge.svg)](https://github.com/Trushi28/Aphelion/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Aphelion boots through [Limine](https://github.com/limine-bootloader/limine) into a higher-half
freestanding kernel. It has no GRUB, no multiboot2, no legacy 8259 PIC and no VGA text mode. It is an
early-stage hobby project: the kernel, scheduler, storage drivers and filesystem work, and there is
no user space yet.

## About

Aphelion is an experiment in building an operating system without inheriting POSIX. The native
interface, memory model, scheduler and filesystem are designed from scratch rather than copied from
Unix, and the hardware target is modern x86_64 only: no legacy interrupt controller, no VGA text mode,
no BIOS-era assumptions in the kernel. The long-term goal is a small system with a local AI assistant
built in, described under [Goals](#goals).

## Features

- Limine boot protocol, hybrid BIOS + UEFI ISO
- x2APIC with automatic xAPIC fallback, IOAPIC, ACPI MADT parsing, SMP
- Buddy physical memory allocator and the kernel's own page tables
- Preemptive MLFQ scheduler with per-core run queues and work-stealing
- Storage drivers for virtio-blk, AHCI/SATA (NCQ) and NVMe, behind one block-device interface
- Stellar FS, a copy-on-write filesystem with a B+tree catalog, CRC32 checksums, O(1) snapshots, B+tree directories,
  extent-table files for fragmented disks, path resolution, atomic rename, typed status codes,
  twin checksummed superblocks, flush-ordered crash-consistent commits and an fsck-style `check()`
- Corona, a Unicode terminal console on the framebuffer: JetBrains Mono at three sizes, 24-bit colour, box drawing and
  braille, a RAM backbuffer with pixel scrolling
- PS/2 keyboard and serial input through one key-event queue, a kernel log ring and ACPI/keyboard-controller reboot
- Two boot entries from one ISO: a shell and a test suite
- RTC wall clock plus a tick counter, so files carry real modification times

## Goals

None of the following exists yet. This is the direction:

- **Small footprint.** Keep the whole resident system, including the AI runtime, within 100 to 200 MiB of RAM.
- **On-device AI model, written in Rust.** A lightweight local model with no Python in the stack.
- **Voice assistant.** Speech recognition and command execution in the style of Siri, built as a
  separate service from the model.
- **Binary loaders for ELF, PE (NT) and Mach-O**, so programs built for Linux, Windows and macOS can run.

## Roadmap

- [x] Boot, memory management, SMP, scheduler
- [x] Block drivers and Stellar FS
- [ ] User space: ring 3, system calls, processes, IPC, ELF loader
- [ ] Rust toolchain support and a service manager
- [ ] Audio driver and voice pipeline
- [ ] On-device model runtime
- [ ] PE and Mach-O loaders

## Building

Requires a C++20 compiler (`g++` or `x86_64-elf-g++`), `nasm`, `xorriso`, `mtools`, `git` and QEMU.
Limine is downloaded automatically on the first build.

```sh
# Arch
sudo pacman -S base-devel git nasm libisoburn mtools qemu-system-x86

# Debian / Ubuntu
sudo apt install build-essential git nasm xorriso mtools qemu-system-x86 qemu-utils
```

```sh
make iso            # build the kernel and build/aphelion.iso (-O2)
make iso OPT=-O0    # unoptimized build for debugging; objects rebuild when OPT changes
make iso FPU=1      # x87/SSE2/AVX on every core, saved per Satellite (kernel code itself stays general-register only)
make run            # boot in QEMU
make run-smp        # boot with 4 cores
make run-nvme       # boot with an NVMe disk (SMP=4 for multi-core)
make run-ahci       # boot with an AHCI/SATA disk
make run-headless   # serial output only
make test-stellar   # run the filesystem tests on the host, without QEMU
make test-sched     # boot words, timer maths, run queues and the MLFQ policy on the host
make test-console   # UTF-8, terminal emulator, font blob and incremental rendering on the host
make test-input     # scancode and ANSI key decoders and the kernel log ring on the host
make test-mm        # buddy allocator, kernel heap (including 4 threads), page tables and DMA table on the host
make bench-stellar  # filesystem scaling benchmark on sparse virtual disks up to 8 GiB
make crash-fuzz     # kill QEMU at random instants, fsck the crashed disk, boot it again (FUZZ_KIND, FUZZ_CORES, FUZZ_ROUNDS)
make reset-disk     # delete disk.img so the next run formats a fresh filesystem
```

The first run creates `disk.img`, a 64 MB raw disk. The filesystem on it is independent of the
block driver, so switching between virtio-blk, AHCI and NVMe keeps the same files. Run
`make reset-disk` to start with a fresh filesystem.
`make iso` generates a Limine menu with two entries, **Aphelion Shell** and **Aphelion Test**.
`make iso PROFILE=usb` builds the variant meant for real hardware: both entries pass `storage=off`, so no disk
driver starts and nothing on the machine's own drives can be touched, and the shell is the default.
`BOOT_DEFAULT=shell|test` and `MENU_TIMEOUT=<seconds>` change the selection.

```sh
make iso PROFILE=usb
```

Then write `build/aphelion.iso` to the stick. With Rufus choose **DD Image mode** when asked; `dd if=build/aphelion.iso
of=/dev/sdX` does the same. Check the device name twice: that command overwrites whatever it names.

## Tested on

- QEMU q35 with a BIOS boot, using virtio-blk, AHCI and NVMe disks, on 1 to 4 cores. CI runs this
  matrix on every push.
- x2APIC mode checked on a physical machine. Under QEMU without KVM the kernel falls back to xAPIC.

## Project layout

```
kernel/
  src/
    boot/       entry point and subsystem bring-up
    arch/       GDT, IDT, APIC, ACPI, IOAPIC, PCI
    mm/         physical and virtual memory
    sched/      scheduler and context switching
    console/    Corona terminal, font and UTF-8
    drivers/    framebuffer, serial, keyboard, input queue, klog, virtio-blk, AHCI, NVMe
    fs/         Stellar FS
  include/cosmos/
tools/          host-side tests, the font baker, the CI boot and shell checks
docs/
```

## Documentation

- [Architecture](docs/ARCHITECTURE.md): memory, scheduler, drivers and interrupts
- [Stellar FS](docs/STELLAR.md): filesystem design, snapshots and known limitations
- [Engineering notes](docs/ENGINEERING.md): bugs found and fixed along the way

## License

Aphelion is released under the [MIT License](LICENSE).
