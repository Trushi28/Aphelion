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
- Stellar FS, a copy-on-write filesystem with a B+tree catalog, CRC32 checksums, O(1) snapshots, hash-indexed directories,
  Bloom-filtered snapshot lookups, path resolution, typed status codes,
  twin checksummed superblocks, flush-ordered crash-consistent commits and an fsck-style `check()`
- PS/2 keyboard with extended keys, framebuffer console

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
make run            # boot in QEMU
make run-smp        # boot with 4 cores
make run-nvme       # boot with an NVMe disk (SMP=4 for multi-core)
make run-ahci       # boot with an AHCI/SATA disk
make run-headless   # serial output only
make test-stellar   # run the filesystem tests on the host, without QEMU
make reset-disk     # delete disk.img so the next run formats a fresh filesystem
```

The first run creates `disk.img`, a 64 MB raw disk. The filesystem on it is independent of the
block driver, so switching between virtio-blk, AHCI and NVMe keeps the same files. Run
`make reset-disk` to start with a fresh filesystem.
The ISO can also be written to a USB stick to boot real hardware:

```sh
dd if=build/aphelion.iso of=/dev/sdX
```

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
    drivers/    framebuffer, serial, keyboard, virtio-blk, AHCI, NVMe
    fs/         Stellar FS
  include/cosmos/
tools/          host-side filesystem tests and the CI boot check
docs/
```

## Documentation

- [Architecture](docs/ARCHITECTURE.md): memory, scheduler, drivers and interrupts
- [Stellar FS](docs/STELLAR.md): filesystem design, snapshots and known limitations
- [Engineering notes](docs/ENGINEERING.md): bugs found and fixed along the way

## License

Aphelion is released under the [MIT License](LICENSE).
