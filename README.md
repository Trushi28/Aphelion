# Aphelion

**A from-scratch, non-POSIX x86_64 operating system.** Higher-half C++20
freestanding kernel, booted via [Limine](https://github.com/limine-bootloader/limine)
— no GRUB, no multiboot2, no legacy PIC, no VGA text mode anywhere in the
codebase. Built and boot-tested end-to-end, single-core and SMP up to 8
vCPUs, with a real preemptive multi-core scheduler and a real block device
driver, not just a "hello world" that halts.

Every subsystem below is running code that has actually been proven to
work — booted in QEMU, exercised, and checked against real output — not
just written and assumed correct. Where that took finding and fixing a
real bug, it's written up in [Engineering notes](#engineering-notes)
instead of quietly disappearing from the history.

## Contents

- [Features](#features)
- [Building](#building)
- [Architecture](#architecture--the-cosmos-model)
- [Project layout](#project-layout)
- [Engineering notes](#engineering-notes)
- [Roadmap](#roadmap)

## Features

- **Modern boot** — [Limine](https://github.com/limine-bootloader/limine)
  protocol, higher-half kernel, hybrid BIOS+UEFI ISO
- **x2APIC** (MSR-based) with automatic **xAPIC** (MMIO) fallback for CPUs
  or hypervisors that lack it — no legacy 8259 PIC involved anywhere
- **SMP** — every core brought up via Limine's SMP request, each with its
  own GDT/TSS/IDT/local APIC
- **A real physical + virtual memory model** — buddy allocator with actual
  split/coalesce, and the kernel's own page tables built and switched onto
  before any bootloader-owned memory is reclaimed
- **A real preemptive scheduler** — MLFQ-style priority rings, genuine
  context switching, timer-driven preemption calibrated against the PIT,
  and threads that migrate cores on their own via a shared run queue
- **A real block device driver** — virtio-blk over PCI, full modern
  virtio-pci transport, verified against actual disk data
- **A real filesystem** — Stellar FS, a graph-based namespace on top of
  virtio-blk, verified with real files that persist across reboots
- **A real input device driver** — IOAPIC-routed PS/2 keyboard, verified
  by injecting keystrokes and reading them back out
- **A framebuffer console** with its own embedded font — no VGA text mode

## Building

**Requirements:** `x86_64-elf-gcc`/`x86_64-elf-g++` (a native x86_64 `g++`
also works — the Makefile falls back automatically), `git`, `nasm`,
`xorriso`, `mtools`, `qemu-system-x86_64`.

Limine is fetched automatically on first build (shallow clone,
`v9.x-binary` branch) — it isn't vendored in this repo.

```sh
make iso            # fetch Limine if needed, build the kernel, build the ISO
make run             # build + boot in QEMU with a display window
make run-smp         # same, with 4 vCPUs
make run-headless    # serial-only, no display — for scripted/automated runs
```

To exercise the virtio-blk driver, attach a disk:

```sh
qemu-img create -f raw disk.img 64M
qemu-system-x86_64 -M q35 -cpu max -m 256M -cdrom build/aphelion.iso \
  -drive file=disk.img,if=none,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
  -serial stdio
```

`build/aphelion.iso` also writes directly to a USB stick
(`dd if=build/aphelion.iso of=/dev/sdX`) for real hardware — BIOS and UEFI
both boot it.

## Architecture — the "Cosmos" model

Every subsystem shares one naming scheme instead of the usual generic
OS-dev vocabulary. It's not just theming — the metaphor maps cleanly onto
the actual mechanics in each case.

### Memory

**Physical — `Universe`.** `Galaxy` = one contiguous region the firmware
reported. `Orbit` = the free list for one buddy order. `Planet` = a single
allocated block. Real buddy split + coalesce (`kernel/src/mm/pmm.cpp`),
spinlock-protected since every core can allocate concurrently.

**Virtual — `Constellation`.** An address space (one PML4). *The Core* is
the kernel's own, built fresh rather than continuing to run on Limine's
bootloader-owned tables (`kernel/src/mm/vmm.cpp`) — not just tidiness:
reusing bootloader-reclaimable memory before replacing the tables that
point into it is a real use-after-free. A `Nebula` is one mapped region.

### Scheduler — "Orbital"

`Sun` = a core. `Satellite` = a kernel thread. `Orbit` (ring) = a priority
level, four of them. A Satellite that burns its whole timeslice is demoted
one ring out; one that yields voluntarily before its slice is up is
promoted one ring in — classic MLFQ shape, mapped onto rings.

Real context switching (`kernel/src/sched/context_switch.asm` +
`orbital.cpp`), real timer-driven preemption off the APIC timer (calibrated
against the legacy PIT at boot — the tick rate depends on the machine's bus
clock, so a hardcoded constant would just be wrong on real hardware), and
one run queue shared across every Sun rather than per-core queues —
Satellites migrate cores on their own as a natural consequence. That's a
fine starting point, not yet core-affine; per-core queues with
work-stealing is the natural next step, not a correctness gap.

### Storage — virtio-blk

The first real block device, over PCI (legacy port-based config space
access, brute-force bus scan — no ACPI PCI routing needed for that). Full
modern virtio-pci transport: capability-list parsing to locate the
common-config/notify/device-config regions, the standard
reset→ACKNOWLEDGE→DRIVER→FEATURES_OK→DRIVER_OK handshake negotiating only
`VIRTIO_F_VERSION_1`, a single split virtqueue, synchronous polled requests.

### Filesystem — "Stellar FS"

Graph-based namespace instead of a strict tree — a `Constellation`
(directory) is just a `Star` whose data is a set of named edges, so nothing
stops the same Star being pointed to from more than one Constellation, no
hard-link special-casing required. `kernel/src/fs/stellar.cpp`.

What v1 actually is, plainly: the Star Catalog is a flat, fixed-size array
(star ID *is* the slot index) rather than the B+tree with independent
128-bit IDs the original design calls for, and a Constellation's edge list
lives in a single fixed sector (8 entries, not yet growable). Free space is
a plain linear sector bitmap rather than reusing the Universe/Orbit buddy
machinery for disk space too. Copy-on-write extents, per-extent checksums,
and O(1) snapshots aren't built yet — they're the natural next layer once
there's a growable catalog and directory structure under them. None of
that is hidden in the code; it's the honest state of a first working slice,
not a finished design pretending otherwise.

What *is* real: format, mount, `create_file`/`create_constellation`,
`find`, `list`, `read_file` all work end to end on top of virtio-blk, and
it's been checked, not just trusted — booted against a fresh disk (format
path), then rebooted against the *same* disk image (mount path) and
confirmed the file written on the first boot was still there, byte-for-byte
correct, after a full simulated power cycle. A file nested inside a
Constellation two levels deep round-trips the same way.

### Hardware / interrupts

x2APIC (MSR-based) with automatic fallback to xAPIC (MMIO) for CPUs or
hypervisors without it. ACPI/MADT parsing for CPU + IOAPIC enumeration.
IOAPIC-routed PS/2 keyboard on IRQ1 — the first real external device on a
real IRQ line, no legacy PIC anywhere in the path.

## Project layout

```
kernel/
  src/
    boot/       entry point, Limine requests, subsystem bring-up
    arch/       GDT, IDT, APIC, ACPI, IOAPIC, PCI
    mm/         Universe (physical) + Constellation (virtual) memory
    sched/      Orbital scheduler + context switching
    drivers/    framebuffer, serial, PIT, PS/2 keyboard, virtio-blk
    fs/         Stellar FS
  include/cosmos/   public headers for all of the above
  linker.ld
Makefile
```

## Engineering notes

<details>
<summary><strong>11 real bugs found and fixed while building this — click to expand</strong></summary>

Kept here because they're the kind of thing worth not re-learning.

1. **2MB huge pages silently break when virt/phys alignment don't match.**
   The kernel's physical load address (from Limine) is *not* 2MB-aligned,
   even though its virtual address (`0xffffffff80000000`) conventionally
   is. Masking both down to their own 2MB boundary independently maps the
   virtual address to the *wrong* physical block — no fault until you
   actually jump there. Fixed by mapping the kernel image at 4KB
   granularity instead (`constellation::map_4k`).
2. **The firmware memory map includes a "reserved" entry for the 64-bit
   PCIe MMIO hole**, parked around the 1TB physical mark. Naively spanning
   "0 to the highest address in the memmap" to build a direct map turns
   into a 500,000+ iteration loop for nothing. Filter it out.
3. **Limine's own pre-kernel page tables don't cover everything** — not
   the LAPIC's MMIO register page, and not every ACPI table's address.
   Only the kernel's *own* Constellation, built from the full memory map,
   can be relied on for that; anything touching those needs to wait until
   after the switch.
4. **APs boot on their own copy of Limine's tables, not the BSP's
   Constellation.** Forgetting `write_cr3` at the top of the AP entry point
   means every core after the first faults the instant it touches anything
   only the kernel's own tables map.
5. **A shared IDT handler table rebuilt on every core's bring-up silently
   erases handlers already registered.** `idt::init()` used to run fully
   (including zeroing the handler array) on every single core, BSP and
   every AP alike — harmless until a handler gets registered on the BSP
   *before* the APs finish waking, at which point each AP wipes it on its
   way up. Fixed with a build-once guard; every core still needs its own
   `lidt`, just not a fresh table.
6. **The physical allocator had no lock**, and single-core boot never
   exercised that gap — nothing called it concurrently. The moment
   multiple APs each allocate their own idle Satellite's stack during
   bring-up, two cores can splice the same buddy free-list head at once,
   corrupting it. The resulting page faults pointed at bogus stack
   addresses nowhere near the actual bug (`kernel/src/mm/pmm.cpp`, now
   spinlock-protected).
7. **The classic SMP scheduler race: enqueueing a Satellite before it has
   actually stopped running.** `yield()` used to unlock the run-queue lock
   right after making the outgoing Satellite visible to other cores, then
   call `switch_context` — leaving a window where another core could
   dequeue it and switch into its stack while the original core was still
   mid-switch, saving state that hadn't finished being saved yet. Two cores
   both "running" the same Satellite corrupts fast. Fixed by holding the
   lock across the entire enqueue→dequeue→switch sequence, releasing it
   only from whichever Satellite's stack execution actually resumes next
   (`orbital::reschedule_locked`, plus a matching unlock at the top of
   `satellite_trampoline` for a Satellite's very first run). First
   surfaced, fittingly, exactly when cross-core migration started
   happening for real.
8. **Ordering: the scheduler's one-time setup has to finish before any AP
   can call into it.** `orbital::init()` sets a module-wide HHDM offset and
   calibrates the timer; an AP that races in and calls `init_core()` before
   that's done finds the HHDM offset still zero and treats a physical
   address as a virtual one directly. Moved `orbital::init()` to run on the
   BSP strictly before the SMP wake-up loop.
9. **GCC's C++ frontend doesn't support array designated initializers**
   (`arr[i] = x` inside a brace-init-list) even though its C frontend does —
   it's a GNU C extension, not a GNU C++ one, and fails with "sorry,
   unimplemented" rather than a normal error. The scancode table got built
   at runtime instead, which is arguably clearer anyway.
10. **A driver's DMA helper that only works for its own allocations, used
    on a caller's buffer.** virtio-blk's `virt_to_phys` is only valid for
    pointers that came from its own HHDM-backed `alloc_pages` — but the
    first version handed the caller's buffer straight to the device
    descriptor, and that buffer lived in the kernel image, mapped through
    an entirely different virt→phys relationship. The device dutifully
    wrote real data into whatever wrong physical address that subtraction
    produced; the kernel then read its own zeroed `.bss` back and called it
    a successful read. Fixed with an internal bounce buffer that every
    transfer goes through, and caught by planting known bytes on a real
    test disk and checking for them — not by the read merely returning
    `true`.
11. **A framebuffer-only `printf` clone with a narrower format-specifier
    set than its serial counterpart.** `fb::printf` never got a `%lu` case
    added when `serial::printf` did, so every boot line using it printed
    the literal characters `%lu` instead of the number — invisible in
    headless serial-only testing, since those specific lines only ever
    went to the framebuffer. Only surfaced once someone was actually
    looking at the graphical console, which is exactly why it went
    unnoticed for as long as it did.

</details>

## Roadmap

- Grow Stellar FS toward the real design: a B+tree Star Catalog with
  independent IDs instead of ID-as-slot-index, growable Constellation edge
  lists instead of a fixed 8-entry sector, copy-on-write extents,
  per-extent checksums, O(1) snapshots
- Interrupt-driven virtio-blk completion instead of polling, and
  multi-request queueing instead of one synchronous request at a time
- Shift/Ctrl/Alt tracking and MADT Interrupt Source Override parsing for
  the keyboard driver
- Per-core run queues with work-stealing, instead of Orbital's current
  single shared queue
- A Satellite exit/reap path — right now a thread whose entry function
  returns just parks itself in a permanent yield loop rather than actually
  freeing its stack, since doing that safely needs a reaper running on a
  different stack than the one being freed
