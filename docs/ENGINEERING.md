# Engineering notes

Fourteen real bugs and design flaws found while building Aphelion, kept because they are the kind of thing worth not re-learning.

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
   (including zeroing the handler array) on every core — harmless until a
   handler gets registered on the BSP *before* the APs finish waking. Fixed
   with a build-once guard; every core still needs its own `lidt`.
6. **The physical allocator had no lock**, and single-core boot never
   exercised that gap. Two APs allocating their idle Satellite's stack at
   once could splice the same buddy free-list head, corrupting it; the
   resulting page faults pointed at bogus addresses nowhere near the real bug.
7. **The classic SMP scheduler race: enqueueing a Satellite before it has
   actually stopped running.** Fixed by holding the run-queue lock across the
   whole enqueue→dequeue→switch sequence, released only from whichever
   Satellite's stack resumes next (`orbital::reschedule_locked`, plus a
   matching unlock at the top of `satellite_trampoline`).
8. **The scheduler's one-time setup has to finish before any AP can call
   into it.** An AP racing into `init_core()` before `orbital::init()` finds
   the HHDM offset still zero. `orbital::init()` now runs on the BSP strictly
   before the SMP wake-up loop.
9. **GCC's C++ frontend doesn't support array designated initializers**
   (`arr[i] = x`) even though its C frontend does — "sorry, unimplemented".
   The scancode table is built at runtime instead.
10. **A driver's DMA helper that only works for its own allocations, used
    on a caller's buffer.** virtio-blk's `virt_to_phys` is only valid for
    its own HHDM-backed pages; handing it a kernel-image buffer made the
    device write to the wrong physical address while the kernel read back
    its own zeroed `.bss` and called it success. Fixed with a bounce buffer;
    caught by planting known bytes on a test disk, not by the read
    returning `true`.
11. **A framebuffer-only `printf` clone with a narrower format-specifier
    set than its serial counterpart.** `fb::printf` never got `%lu`, so those
    lines printed the literal characters — invisible in headless testing.
12. **Stellar FS rewrote its entire free bitmap on every allocation, and had
    no sector cache.** On a 64 MB disk that's 32 synchronous sector writes per
    allocated sector run (and it scales with disk size), on top of an
    uncached B+tree descent per lookup. A single `create_file` cost ~53
    sector writes. Found by counting sector I/O in a host-side harness, fixed
    with dirty-sector bitmap writes, a write-through cache and per-operation
    commits (~7 writes).
13. **Snapshots were a recursive metadata clone, not O(1).** Because star
    entries were mutable in place and referenced by global ID, freezing a tree
    meant minting a new star for every file and directory and re-adding every
    edge — ~4000 writes for a 120-entry tree, growing with size. Fixed by
    making the catalog and directories epoch-versioned copy-on-write, so a
    snapshot is just a saved root pointer. The boot self-test now asserts the
    cost is exactly 1 write.
14. **The kernel was built at `-O0`, and turning on `-O2` exposed five latent
    hazards that `-O0` had been hiding.** Port I/O, `cli`/`sti`/`hlt` and
    `wrmsr` inline asm had no memory clobber, so a polled completion loop
    could legally be compiled to read a DMA-written ring once. Descriptors
    written with ordinary stores before a volatile doorbell write had no fence,
    and volatile only orders against other volatile accesses. The freestanding
    `memset`/`memcpy` loops get recognised as `memset`/`memcpy` and call
    themselves; they are now `rep stosb`/`rep movsb`, with
    `-fno-tree-loop-distribute-patterns` for the loops that remain. Byte
    buffers cast to B+tree nodes and ACPI tables need `-fno-strict-aliasing`.
    New kernel threads entered at `rsp % 16 == 0` where the ABI wants 8.
    Separately, the framebuffer scroll copied volatile bytes one at a time,
    which no optimisation level can batch; it now moves whole rows.
