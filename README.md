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
- Nebula, a non-POSIX namespace: `realm:/path` names over RAM, the Stellar disk, read-only snapshots and live kernel
  state, with Unicode names, partial I/O over whole-file Stellar handles and cross-realm copy
- Helm, the shell: typed rows flowing through pipes, named `key: value` options, `=>` and `!>` links, globs, `@name`
  values, scripts, tab completion, history, Ctrl-C, Unicode editing and about fifty Aphelion-named commands
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
make run-test       # build the self-test image (build/aphelion-test.iso) and boot it headless; this is what CI runs
make test-stellar   # run the filesystem tests on the host, without QEMU
make test-sched     # boot words, timer maths, run queues and the MLFQ policy on the host
make test-console   # UTF-8, terminal emulator, font blob and incremental rendering on the host
make test-input     # scancode and ANSI key decoders and the kernel log ring on the host
make test-usb       # USB descriptor walker, BOT and SCSI builders, boot-stick identification and free-space rules
make test-usbstor   # the real USB storage layer against a fake stick: only the boot stick is ever written
make test-mm        # buddy allocator, kernel heap (including 4 threads), page tables and DMA table on the host
make bench-stellar  # filesystem scaling benchmark on sparse virtual disks up to 8 GiB
make crash-fuzz     # kill QEMU at random instants, fsck the crashed disk, boot it again (FUZZ_KIND, FUZZ_CORES, FUZZ_ROUNDS)
make reset-disk     # delete disk.img so the next run formats a fresh filesystem
```

The first run creates `disk.img`, a 64 MB raw disk. The filesystem on it is independent of the
block driver, so switching between virtio-blk, AHCI and NVMe keeps the same files. Run
`make reset-disk` to start with a fresh filesystem.
`make iso` generates a Limine menu with two entries, **Aphelion Shell** and **Aphelion Test**.
`make iso PROFILE=usb` builds the variant meant for real hardware: the shell entry passes `storage=usb` and the
test entry `storage=off`, so no AHCI, NVMe or virtio driver ever starts and nothing on the machine's own drives can be
touched. The shell is the default in both profiles, so every `make run*` boots the shell; pick **Aphelion Test** in the menu for the self-tests, or use `make test-iso` / `make run-test`. `BOOT_DEFAULT=shell|test` and `MENU_TIMEOUT=<seconds>` change the selection.

```sh
make iso PROFILE=usb
```

Then write `build/aphelion.iso` to the whole stick. With Rufus choose **DD Image mode** when asked; `dd if=build/aphelion.iso
of=/dev/sdX` does the same. Check the device name twice: that command overwrites whatever it names.

### Using the stick as storage

With `storage=usb` the kernel starts the xHCI controller, finds the USB mass-storage drives and decides which one it
booted from: first by the GPT disk GUID that Limine reports, then by the ISO volume id (`APH` plus a hash of the
build). Only a drive that matches is ever written, and only in the free space after the image: the kernel keeps a
1 MiB gap behind the image and the backup partition table and needs at least 16 MiB. That free space is the
"window". `usb` lists the drives and what was decided for each, `storage` shows the window, and `storage init erase`
formats it after you type the size back to confirm. The window is mounted automatically on later boots.

What this means in practice:

- Use a stick larger than the image and write it in **DD mode**. Rufus's ISO/FAT32 mode makes one partition covering
  the whole stick, which leaves no provable free space, so the kernel refuses to use it.
- Two sticks written from the same ISO cannot be told apart, so with both plugged in neither gets a window.
- Hubs, UAS-only drives, 4096-byte-sector drives and USB keyboards are not supported yet. Use the laptop's own
  keyboard (PS/2 or the serial console) for the shell.
- `sunset` uses the ACPI S5 sleep state (the `_S5_` package is read from the DSDT without an AML interpreter) and falls
  back to the QEMU, Bochs and VirtualBox ports under a hypervisor. It is untested on real hardware.
- The `make run*` targets let QEMU behave like a machine: `restart` restarts the guest and `sunset` exits QEMU. To keep a
  crashed guest on screen instead, run `make run QEMU_EXIT="-no-reboot -no-shutdown"`; QEMU then shows "paused" after a
  clean `sunset` too, which is the flag, not a crash.

`make run-usb` and `make run-uefi-usb` boot the usb profile in QEMU from an emulated xHCI stick (`build/usb.img`,
the ISO padded to `USB_IMG_SIZE`, 256M by default).

## The shell

The shell boots as `aphelion <location> ▸`. Locations look like `ram:/notes`, `disk:/proj/src`, `snap:/3/proj` and
`sys:/memory`; a colon before the first slash names a realm, and `./a:b` spells a file whose name contains a colon.

| Realm | What it is |
| --- | --- |
| `ram` | memory-backed, writable, 2048 items and 48 MiB |
| `disk` | the live Stellar volume |
| `snap` | read-only Stellar snapshots, `snap:/<id>/...` |
| `sys` | read-only text files: `version uptime memory cpus tasks realms storage log` |

```
say café ₹ Ω | etch ram:/greeting
beacon ram:/greeting | seek fold: yes CAFÉ
chart disk:/ depth: 4 | where size > 1MiB | order size down: yes | first 5
twin tree: yes snap:/1/proj disk:/restored
epoch take
chart snap:/ | where changed > 2026-10-01
kindle erase
help
```

Helm does not follow the POSIX shell. Commands carry Aphelion names (`chart` lists, `beacon` shows a file, `probe`
inspects, `spark` creates, `plant` makes a directory, `fade` removes, `twin` copies, `drift` moves, `seek` searches
text, `orbit` and `suns` show Orbital, `epoch` freezes the disk, `restart` and `sunset` end the session).
Options are named, `key: value`, and may sit anywhere after the command: `chart depth: 3 only: dir`,
`fade tree: yes`. A word that ends in a colon is an option only for a command that declares it, or a realm name;
quote it to use it as plain text. Commands such as `chart`, `probe`, `realms`, `orbit`, `suns` and `memory` produce
typed rows (text, numbers, sizes such as `2KiB`, times such as `2026-10-01`, yes/no) and `where`, `pick`, `order`,
`first`, `last`, `distinct` and `tally` filter, sort and count them by column, so a size compares as a size and a time
as a time. Text piped into a row command becomes one row per line, and rows piped into a text command become their
text column. `etch FILE` writes what arrives to a file (`append: yes` adds to it); there are no redirect operators.
`a ; b` runs both, `a => b` runs `b` only after success and `a !> b` only after failure. Bound values are `@name`,
`@{name}`, `@status` and `@here`. A terminal shows rows as an aligned table with a row count.

The disk is mounted in shell mode when it already holds a Stellar volume, and a completely blank disk is formatted at
boot. `kindle erase` creates an empty volume only on a disk that is blank or already carries a Stellar superblock, so a
disk with someone else's data is refused. `help syntax` lists the option, row, combining, quoting and value rules.


Limits worth knowing: files are capped at 16 MiB and partial writes are buffered in memory until `sync`, close or
exit; a pipe stage's output is capped at 8 MiB; globs work in the last path component only; variables are never word
split; `grep` is a plain substring match with ASCII case folding; `touch` only creates; a single `&` is rejected; keys
typed while a command runs are discarded; the Corona console draws Latin, Greek, Cyrillic, symbols and box drawing but
not CJK or Indic scripts, which the editor still stores and writes correctly to a file or the serial line.

## Tested on

- QEMU q35 with a BIOS boot, using virtio-blk, AHCI and NVMe disks, on 1 to 4 cores. CI runs this
  matrix on every push.
- The USB stack is covered by host tests and by `tools/ci-usb.sh` (QEMU xHCI with a decoy stick). It has not been run
  on a physical xHCI controller yet.
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
    drivers/    framebuffer, serial, keyboard, input queue, klog, virtio-blk, AHCI, NVMe, xHCI, USB storage
    fs/         Stellar FS, the Nebula namespace and its realms
    shell/      Helm: parser, line editor, commands
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
