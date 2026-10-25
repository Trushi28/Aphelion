# Engineering notes

Thirty real bugs and design flaws found while building Aphelion, kept because they are the kind of thing worth not re-learning.

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
15. **The Makefile tracked no header dependencies.** Objects depended only on their `.cpp`, so editing a header
    left stale objects behind. Adding a virtual to `blockdev::Device` shifts the vtable and would have made
    untouched drivers call the wrong slot. Fixed with `-MMD -MP`.
16. **A name was truncated on create and compared in full on lookup.** `copy_name` cut at 51 characters while
    `find` compared the whole string, so a long name could be created and never found; nothing stopped the
    same name being created twice, and `link` accepted directories, which makes cycles. Names are validated,
    duplicates refused, and only files can be hard linked. The stress test never saw any of it because it only
    ever made valid, unique names.
17. **`mount()` trusted the superblock.** A `snap_count` above 24 read past the snapshot array and a wrong
    `bitmap_sectors` sized the in-RAM bitmap from untrusted input. Found by corrupting each field of sector 0 in
    the host harness; all 16 corruptions mounted before.
18. **Write errors were swallowed.** `commit()` returned `void` and ignored `raw_wr`, so a failed bitmap or
    superblock write still reported success. `unlink` also freed a file's extents before the catalog update that
    kills the star, so a failed update left a live star pointing at free sectors. Found by failing the disk
    after every possible number of writes for each operation.
19. **Stellar and all three drivers shared global state with no locking.** Two Satellites in the cache,
    bitmap, queue tails or bounce buffers corrupted them. Reproduced with Satellites hammering the filesystem
    and the block layer on 4 cores before any lock existed, then verified fixed with the same tests.
20. **A mutex built on `yield()` livelocked.** `yield()` promotes the caller one ring, so a waiter spinning on
    a lock climbed to ring 0 and kept out-ranking the preempted holder in a lower ring. On one core the
    holder never ran again: virtio persisted boots hung about once in every two or three. Found by sampling the
    stuck CPU through the QEMU monitor and dumping the mutex owner and its ring; fixed by sinking waiters to
    the lowest ring.
21. **The "work stealing confirmed" check was a timing race.** It asked which core eight fixed-length workers
    finished on, which depends on whether an idle core gets scheduled before the boot core finishes. On a
    one-CPU host, where QEMU's four vCPUs share a core, the untouched code confirmed in 2 of 10 boots. Workers now
    run until they have observed more than one core (with a cap), so the test waits for the event.
22. **A recoverable allocation failure leaked into an error code.** When the directory index could not be
    allocated the code correctly fell back to scanning, but the allocator had already recorded `NoMemory` as the
    first error, so a duplicate create reported `NoMemory` instead of `Exists`. Found only by a test that
    starves the index of memory; the index and filter now allocate quietly.
    (The index and filter this note is about were later removed when directories became B+trees; the lesson
    about fallback paths recording errors still applies to any future optional allocation.)
23. **A checksum does not defend against a logic bug.** Mutation-testing the crash harness (a protocol with a
    deliberately wrong step) produced an AddressSanitizer stack-buffer-overflow in `bt_search`, then a stack
    overflow in `bt_upsert`: a stale node that was still CRC-valid had an entry count larger than its array, and a
    cyclic tree recursed forever. Node counts are now bounded, sector numbers range-checked, and directory chains
    and tree recursion given cycle and depth limits. The same mutation run also showed the test helper had the
    same unbounded recursion.
24. **A block-device test wrote into live filesystem sectors.** The multi-request and 4-Satellite block-device
    self-tests used the last sectors of the disk, but the filesystem was formatted to the whole disk, so on a
    long-lived disk the "test" overwrote file data. Found by the author while running the CI disk repeatedly. The
    filesystem is now formatted short of the disk, leaving a reserved test tail, and the scratch area is saved and
    restored.
25. **Any mount failure triggered a format.** `kernel_main` formatted whenever `mount()` returned false, so a
    foreign filesystem, damaged superblocks, a transient read error or a newer incompatible feature bit all ended
    in a silent reformat. Found by reading the boot path, then confirmed by booting the old kernel on a disk of
    random bytes, which it formatted. The kernel now formats only on `NotFormatted`, and only when
    `can_auto_format()` sees a blank disk or a Stellar magic (an older version, reformatted by policy). The two
    self-tests that write the disk tail also only run on a mounted volume. CI boots a random-data disk and
    requires it byte-identical afterwards.
26. **A scaling change that made `gc` and `check` issue one device command per bitmap page.** Paging the free-space
    bitmap out of RAM was correct and passed every test, but `gc` and `check` then read each page with its own
    single-sector command: 4,226 and 4,087 commands on an 8 GiB disk against 96 and 22 before. Found by the
    benchmark's command counts. Its timings hid it, because a host RAM disk makes a command free. Sequential
    scans now go through a streaming reader that fetches 32 pages per command. The lesson is to benchmark the
    quantity that costs money on the real device, not the one that is easy to time.
27. **A file had to be one contiguous run.** On a disk fragmented into one-sector holes (709 free sectors in
    this case), creating a 40-sector file failed with "no space left on device". Nothing in the tests ever made free
    space discontiguous, because every test disk was filled and emptied in order. Files that cannot find one run now
    fall back to an extent table. The first version of the tests did not notice when the table walker lost its
    "extents add up to the file" check, because they only corrupted a table in the direction (too many sectors) that a
    different check catches first; a short table, which would have returned truncated data without an error, needed its
    own test.
28. **A crash during `format` left a disk that could never be formatted again, or a volume with no root directory.**
    `format` wrote the zeroed free-space bitmap (non-zero bytes, since each page has a CRC) before either superblock
    held anything, and the format guard from note 25 correctly refuses to overwrite a non-blank disk that does not
    carry the Stellar magic. Separately it made a superblock durable with an empty catalog and created star 0, the
    root directory, in a later transaction. Of 90 simulated crash states, 25 were unbootable and 41 mounted without a
    root. The earlier crash exploration never saw it because it always started from an already formatted disk. The
    bug in note 25's fix exposed this one: a guard that refuses to format unrecognised disks needs `format` to leave a
    recognisable disk at every instant.
29. **Random kill times missed a window a millisecond wide, and the boot self-test assumed a pristine disk.** The
    QEMU kill fuzzer found no format bug in about 60 uniformly random kills, because the format takes about a millisecond
    and a shell cannot aim that finely; it needed a throttled disk and a kill keyed to a log line. What it did find
    was the kernel's own stress self-test failing after a boot that had been killed mid-run, because it hard-coded how
    many entries the root directory should hold. A fuzzer is only as good as the window it can reach, and a test that
    only passes on a clean disk will fail for the wrong reason the first time anything crashes.
30. **The host test's spinlock aborted on contention, so the allocator was never tested from two threads.** The shim used to
    make `lock()` call `abort()` if the lock was already held, which was enough to catch a recursive lock in a single-threaded
    test and made any second thread impossible. It is now a real spinlock, and `make test-mm` runs four threads against the
    buddy allocator and the heap, checking that no page is ever handed to two owners. Separately, `kernel_main` used to copy
    `min(entries, 512)` memory-map entries and carry on, so a machine with a longer map would have lost memory silently; it
    now stops with a message.
31. **Nothing ever runs global constructors, and the scheduler's defaults quietly assumed it did.** The linker script
    has no `.init_array` and nothing calls constructors, so a global is only valid if all-zero bytes are a valid state.
    The scheduler's `Satellite` carried non-zero defaults (`slice_left`, `allot_left`, a `"?"` name) that the compiler
    turned into a start-up function that was never called; it was harmless only because every Satellite goes through
    `build_satellite` before use. The new `Semaphore` and `SleepLock` store "taken" rather than "available" so that a
    zeroed one is a valid, empty one, and the defaults in `mlfq::Account` and `Satellite` are now zero.
32. **A calibration loop with no timeout can hang a boot on hardware, not just in a test.** The old APIC calibration
    spun on the PIT counter forever; a laptop without a legacy PIT would have stopped there with nothing on screen.
    Every reference timer wait is now bounded by the TSC, and HPET or the ACPI PM timer is preferred.
33. **A timed wait has two ways to end, and the sleeper list read the epoch after dropping its lock.** The first design
    of `wake_due_sleepers` released the sleep lock and then read the Satellite's epoch, so a Satellite that had been
    woken by a waker, run, and started a new wait could be woken again by the stale deadline. The sleeper list now
    captures `{Satellite, epoch}` under the lock and only a compare-and-swap on `claimed_epoch` can win a wake.
34. **Every i8042 wait was unbounded, so a machine without a controller would hang the boot.** The old driver
    spun on the status register for "input buffer clear" and "output buffer full". On hardware that has no 8042 the
    bus floats and reads 0xFF, which says "input full" forever. Every wait is now bounded, a first status of 0xFF
    means absent, and the driver reports `absent`, `unresponsive` or `ready` instead of hanging. The auxiliary port is
    left disabled and its bytes are discarded.
35. **A UART that is not there reads as always ready, and one that is stuck blocks forever.** `serial::putc` waited
    for the transmit-empty bit with no limit. The driver now probes the scratch register at start-up, treats an absent
    UART as a sink and drops a byte after 100,000 failed polls instead of spinning. Receive interrupts are only
    enabled when the probe succeeded.
36. **A lone Escape and the start of an arrow key are the same byte.** The serial decoder cannot tell them apart
    without time, so the blocking read polls every 25 ms and releases a pending ESC after 40 ms. Interrupt handlers
    never print: the console lock is a yielding `Mutex`, and a handler that waits on a lock held by the code it
    interrupted on the same core would hang.
37. **The panic handler could hang on the console lock.** If a Satellite was interrupted while drawing, the lock was
    still owned when the exception handler tried to print. The handler now switches the console into a lock-free
    mode first; the output may be interleaved if another core is drawing, which is better than printing nothing.
38. **The framebuffer is write-combined, so reads from it are slow and scrolling it in place was the slowest thing the
    kernel did.** The console renders into a RAM backbuffer, scrolls it with `memmove` and writes finished rows to the
    screen in sequential runs. A host test replays 40 random escape-sequence streams plus a 400-line scroll and checks
    that the incrementally updated screen is identical to a full redraw, with and without a padded pitch.
39. **The font blob was refused by its own loader.** The loader requires U+FFFD as the fallback glyph, the JetBrains
    Mono web subsets do not contain it, and the first test run caught it (the later tests had been passing against
    stale state, which is why the load check is its own test). The baker now draws the replacement glyph itself.
40. **The first Corona build locked the console with a scheduler mutex before the scheduler existed, and the machine reset
    after the first line of output.** `orbital::Mutex::lock` asks the scheduler who the caller is, which reads the local
    APIC id; at the first `fb::init` the APIC is not initialised, so that read faults, and the IDT is not installed
    yet, so the fault is a triple fault. QEMU with `-no-reboot -no-shutdown` shows exactly that: a black window and a
    serial log that ends at `=== Aphelion booting ===`. The host tests could not see it because their lock is a stub.
    The console now takes no lock until `corona::enable_locking()` runs on the boot core after `orbital::init_core()`.
    Nothing prints from other cores before that point.
41. **The serial driver dropped characters after 100000 polls, and the CI log lost lines when the runner was busy.** The
    old unbounded wait was replaced in series C by a bounded one so that a stuck UART could not hang the boot, but the
    bound was a spin count: when QEMU's serial timer thread is slow to run (four emulated cores on a two-core CI runner),
    100000 polls can pass before the transmitter reports ready, the character is dropped, and a line such as
    `[universe] reclaimed` no longer matches the CI grep. The first wait on a character is now 4 million polls, and once a
    wait has timed out the following characters wait only 2000 polls until the UART answers again, so a dead UART costs
    one long wait instead of one per character. The bootloader-memory reclaim task also waited only 3 seconds for every
    core to leave its bootloader stack and then silently gave up; it now waits 20 seconds and prints how many cores had
    arrived. I could not run QEMU here, so this is the most likely cause rather than a reproduced one: if the CI still
    fails, the new message in the log says which case it is.
42. **The shell test could never have matched its own prompt.** `ci-shell.sh` waited for the literal string
    `aphelion ▸`, but the console writes ANSI colour sequences between the name and the arrow and the serial port
    passes them through unchanged. The check now waits for the arrow alone and every later check allows escapes in
    between. Because a command's own echo also ends in a newline, the checks type commands whose output differs from
    what was typed (a redirect followed by `cat`) so that a match proves the output and not the echo.
43. **A line-by-line review of the first Nebula and Helm build found six defects before any of it ran on a machine.**
    A failed `create` or `open` left an empty file behind; the recursive tree copy kept large path buffers in its stack
    frame, which a 32 KiB Satellite stack cannot afford at sixteen levels, so they moved to the kernel heap; a `sys` file
    whose rendered size differs from its `stat` size failed the size check on read, so dynamic files now accept short and
    long reads; the directory sort carried a half-finished comparison; the option parser tested `c < -1`, which can never
    be true, so a bad option was silently accepted (it now sets an explicit flag); and `echo` rejected any argument that
    began with a dash instead of treating only `-n` as an option. The host tests exercise each of these paths. No global
    constructors run in the kernel, so realms and registries are `constinit` and get anything non-constant when they
    register; the shared kernel and host builds compile the same files, so a constructor that does real work fails to
    compile instead of failing at boot.
44. **`mkfs erase` is deliberately blunt about what it accepts.** `stellar::can_auto_format` answers true for a blank
    disk and also for a disk that carries a Stellar superblock, because the boot path uses it to recover a volume whose
    metadata is damaged. The shell command therefore also formats a damaged Stellar volume when asked, but only when the
    word `erase` is typed, only when no volume is mounted and never on a disk that holds anything else in its first 32 KiB.
45. **A STALL on the control pipe halts endpoint 0, and the first xHCI draft never recovered it.** Bulk stalls were
    handled, but a device that stalls GET_MAX_LUN (many cheap sticks do) would leave EP0 halted and every later control
    request would time out and kill the device. Control stalls now run Reset Endpoint and Set TR Dequeue Pointer for
    DCI 1 as well. Found by re-reading the driver before writing the layer above it, not by a test, because no test can
    produce a stall without the fake device.
46. **A short transfer is not a successful transfer.** The first `usbstor` returned success whenever the CSW said
    "passed", so a read that stalled and moved no data, or a write the device only partly took, would have reported
    success with garbage or a hole. The transport now computes the accounted bytes from the residue and the bytes that
    actually moved, and reads and writes must match exactly. `tools/usbstor_host_test.cpp` fails if that check is removed
    (verified by removing it).
47. **Identifying the boot stick is a safety property, so every ambiguous case refuses.** A stick that does not match,
    a GPT that fails its CRC, two sticks written from the same ISO, a stick with no free space and a Rufus ISO-mode
    stick all end with no window and no write. The kernel cannot tell two copies of the same image apart, which is a
    real limit: it asks you to unplug the other one rather than guess.
48. **The ISO volume id has to match the kernel that is running, so both come from one hash.** The Makefile hashes
    all kernel objects, writes the first 12 hex digits (upper case, because ISO 9660 volume ids are) into
    `buildid.cpp`, which is linked into the kernel, and into the `-V` label of the ISO. A kernel booted from a stick
    always sits next to its own ISO label, so the ids agree; the stamp matters when Limine reports no GPT disk GUID
    (the fallback) and when another stick was written from a different build, which then does not match.
49. **What the USB tests do and do not prove.** The host tests drive the real `usbstor.cpp` against a fake BOT device
    and the real parsing code, so the protocol sequencing, stall and residue handling, identification and the write
    confinement are checked. They cannot check the xHCI register programming, DMA, the event ring or real devices'
    quirks, and QEMU's xHCI is far more forgiving than a laptop's. `tools/ci-usb.sh` covers QEMU; physical hardware is
    still untested.
50. **The first xHCI draft timed everything against a TSC frequency that did not exist yet.** `timesrc::tsc_khz()` is
    measured by the scheduler's APIC calibration, which runs after the storage drivers, so at xHCI start it was 0 and
    the driver fell back to an assumed 2 GHz: on a 4.5 GHz Ryzen every delay and timeout would have been 2.25 times too
    short, including the reset and power-on settling times. `storage=usb` now calibrates first (a 10 ms window, repeated
    later by the scheduler) and refuses to start USB if no time source can be measured. A review by a separate reader
    of the driver found this and four more: an error completion other than STALL leaves EP0 halted (now reset, and a
    reset of an endpoint that is not halted counts as success), a failed Evaluate Context was ignored, SuperSpeed ports
    still training were hot-reset instead of waited for (now they are given 400 ms to reach U0 on each scan and warm
    reset only from Inactive or Compliance), and Enable Slot always used slot type 0 instead of the one from the
    Supported Protocol capability. Port speed IDs above 5 (controllers with custom speed tables) are still unsupported
    and such a port is listed as "unknown port speed".


51. **`poweroff` "triple reset" in QEMU was mostly the QEMU flags, and the CI boot images were built in the wrong mode.**
    The run targets passed `-no-reboot -no-shutdown`, which turns a successful ACPI power-off into a paused VM, the same
    picture as a triple fault, so a working `poweroff` looked like a crash. The run targets now default to QEMU's real
    machine behaviour (`QEMU_EXIT`), and `poweroff` goes through ACPI S5 with the PM1 control blocks, the SMI enable
    handshake and the `_S5_` sleep type, with the hypervisor ports as a fallback. Separately, `BOOT_DEFAULT` had been
    inverted (`dev` booted the shell and `usb` the self-tests, the opposite of what the docs and `ci-boot.sh`,
    `crash-fuzz.sh` and `ci-usb.sh` assume), so every CI image booted the wrong mode: no filesystem suite, no xHCI
    line, no `hello.txt` self-test. The default is fixed and `ci.yml` now names `BOOT_DEFAULT=test` for the self-test
    image so a default can never silently change what CI measures. Two shell-test patterns were also stale: `cat
    sys:/version` begins with `kernel: `, and `stat` now colours its key column.
