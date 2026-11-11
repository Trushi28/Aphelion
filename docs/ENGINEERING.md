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


51. **`poweroff` "triple reset" in QEMU was mostly the QEMU flags, and the CI images did not boot the mode CI expects.**
    The run targets passed `-no-reboot -no-shutdown`, which turns a successful ACPI power-off into a paused VM, the same
    picture as a triple fault, so a working `poweroff` looked like a crash. The run targets now default to QEMU's real
    machine behaviour (`QEMU_EXIT`), and `poweroff` goes through ACPI S5 with the PM1 control blocks, the SMI enable
    handshake and the `_S5_` sleep type, with the hypervisor ports as a fallback. Separately, the default image had
    become the shell image while `ci-boot.sh` and `crash-fuzz.sh` still booted `build/aphelion.iso` expecting the
    self-tests, and the usb profile defaulted to the self-tests while `ci-usb.sh` expects the shell. `BOOT_DEFAULT` is now
    `shell` for every profile, so `make run*` never starts the test suite, and CI builds its own `build/aphelion-test.iso`
    with `make test-iso`, so a default can no longer change what CI measures. Two shell-test patterns were also stale:
    `cat sys:/version` begins with `kernel: `, and `stat` now colours its key column. The change planner no longer runs
    every host unit when the Makefile changes; it does so only when the diff touches the host test rules, and the host
    units run as one job instead of one job each.

52. **The first ring-3 self-test failed every boot configuration for three separate reasons.** The serial `printf` has no `%ld`, and an unsupported specifier does not print an error, it shifts every later argument, so the status line came out wrong; `%ld` is now supported. `file_stat` reported size 0 for a file that had just been written, because writes sit in the handle's buffer until `lens::sync` or close and `nebula::Info.size` does not count them; `sys_file_stat` now asks `lens::size`. Other handles still do not see the data until the writer syncs or closes, which is documented behaviour, not a fix. The flaky four-core NVMe run came from `smp-mm`, which compared the global heap object count before and after while the userland self-test was allocating at the same time; the stress test now counts only the objects its own workers hold. Reproduced with QEMU against the CI script, not guessed from the logs, which were not reachable.
53. **`Mutex` lived in `orbital.hpp`, so a lock change rebuilt and retested everything that merely included the scheduler header.** It is now `mutex.hpp`, and Stellar, Nebula, the RAM realm and the Stellar host tools include that instead; the Stellar host unit no longer depends on the scheduler, MLFQ, FPU or IDT headers. A change to `mutex.hpp` still runs the Stellar tests, correctly, because Stellar uses the lock.
54. **A filesystem-only change no longer boots every disk driver.** The planner previously treated everything under `kernel/` as a reason to boot NVMe with one and four cores, AHCI and VirtIO, and to run crash-fuzz on all three. `tools/ci-affected.sh` now emits the two matrices: when every boot-relevant changed file is a filesystem file (`kernel/src/fs/`, the Stellar, Nebula and Lens headers, `tools/stellarfs.cpp`), CI boots NVMe with one core and fuzzes NVMe only, so the kernel still has to link and boot; any other kernel file, the Makefile, or the nightly run uses the full matrix. What this gives up: a Stellar bug that only shows with AHCI or VirtIO timing is caught by the nightly run and not by the pull request. The drivers' own include closure contains `mutex.hpp` and `orbital.hpp`, so a change to either runs the full matrix.
55. **A PE image with no relocation table is not necessarily fixed.** The loader treated "no table" as "cannot move", which refused a perfectly good MinGW program: GNU ld emits no `.reloc` for position-independent code, sets the dynamic-base flag, and does not set the relocation-stripped flag. `relocatable` is now `!stripped && (table present || dynamic base)`, and such an image moves with zero fixups. Found by building the user's own `hello.exe` toolchain's output and running it through `build/peinfo`, not by the lld-only samples, which is the point of having a second linker in the tests.
56. **`thread_join` returned before the joined thread had left the group, so the process exit status could be the child's.** `leave_thread` publishes `done` and wakes joiners, and only then calls `procgroup::leave`. The order is deliberate: a thread that is not last must not touch process memory after it has left, because the last thread can finish and the process can be reaped. A joiner on another core could wake, call `thread_exit(44)` and leave first; the child's later `leave(21)` then counted as the last one and set the status. The self-test case `lastexit` caught it: 1 failure in 10 four-core boots before the fix (status 21, expected 44), found because the 4-core foreign-disk boot, which `ci-boot.sh` does not gate on, printed the wrong-status line while the boot passed. `sys_thread_join` now also waits with `orbital::join` until the child task has exited, which only happens after its `leave`, so a joiner always leaves after the thread it joined. The wait is bounded at 5 seconds and falls back to returning the status. This race was in the R2.4 patch and plausibly accounts for some of the intermittent four-core NVMe CI failures; the smp-mm baseline problem (entry 52) was a separate, real cause.
57. **The PE host is written so the code that runs in ring 3 is the code the host test runs.** The loader core (`host.hpp`) and the kernel32 stubs (`win32.hpp`) are headers parameterised on a small platform (reserve memory, protect, set the TEB, call a function on a stack, log). Aphelion's platform is five syscalls; the test's platform is `mmap`, `mprotect`, `arch_prctl` and a trampoline in a forked child, so real Windows programs built with lld and GNU ld run through the same loader and the same stubs before anything boots. A dry-run platform that records calls instead of making them lets the mutation sweeps check the load pipeline without executing mutated machine code. The one thing the test cannot show is the kernel's side (the vm_protect splitting, GS base switching across context switches); the boot self-test covers that, and passed on all four disk configurations.
58. **The command-line splitter rejected storage that was exactly big enough.** `cmdline::put` checked `used + 1 >= capacity` before storing a byte, so a program name that exactly filled the buffer with its terminator failed. It did not show in any hand-written case; it showed in the capacity sweep (for every sample line, the exact number of bytes and slots must succeed and one less of either must fail). The same sweep is what would catch the reverse mistake, an overrun, and it runs under ASan.
59. **Anything that ends the process needs its own process in a test.** `ExitProcess` and `TerminateProcess` call the platform's exit, which in the kernel never returns. In the test they run in a forked child that reports its status through a pipe, with a runtime whose exit writes the code and calls `exit_group`; a returning exit would instead hit the `ud2` after it and show as a signal, not a status. The other 40-odd stubs run in-process with a recording runtime.
60. **Windows semantics were checked against the specification, not against what the code did.** Two of my own tests were wrong in the first run and the code was right: `VirtualQuery` reports the page of the queried address as `BaseAddress` and sizes the region from there, not from the first page of the whole run, and `%ld` is 32 bits in msvcrt although it is 64 bits in glibc (the printf oracle test removes the `l` from the reference format and keeps `t` and `j`, which are 64 bits in both). A test that restates the implementation proves nothing; where the two disagreed, the documented Windows behaviour decided.
61. **A mutation sweep found dead code and untested edges in a suite that already covered every line.** 751 single-token mutants of the new headers (the `msvcrt.hpp` set was regenerated after a later edit moved its line numbers; the first pass had 752) were built and run against both test programs. 647 of 752 were killed on the first pass; the survivors were read one by one. Dead code that no input could reach was deleted (a clamp in `Heap::resize`, two alignment tests that the 16-byte rounding makes always true, a guard in the free-list walk, an unreachable bounds test and a return value nobody used); real gaps got tests (exact-fit reuse, splitting at exactly the minimum block, chunk growth up to the cap, forged block headers, truncation of over-long command lines and module paths, stale page protections, region sizes, error codes, exact-size buffers, a capacity sweep of the command-line splitter in exactly sized memory). After that, 709 of 751 are killed. The 42 survivors were each read and are not killable by a test, for these reasons: the mutant changes a value that is never observed (a sentinel's back-size, the high bits of a combined surrogate that `0xF0 |` hides, `>=` against `>` where the two branches compute the same number), two of the branches return the same answer by another route (a failed map giving `nullptr` either way), the mutation needs a pointer value of exactly 0x10000 to be passed as a name (the program would fault reading it), or it reads one element past a table or an uninitialised local, which neither ASan nor UBSan reported here and which GCC has no MemorySanitizer to catch (`GetProcAddress` and `attach`; that one out-of-bounds read of the next table entry is the survivor I could not rule out as harmless by reading alone). Mutants are not a proof either way.

62. **The MinGW test programs must be reproducible.** GNU ld stamps the link time into the PE header, so rebuilding `stdio.exe` gave a different file and a different committed base64 header every time. `tools/pe-samples/mingw.sh` now passes `--no-insert-timestamp`, so `embed-mingw.sh` produces the same bytes on every run and a diff of the generated header means the sample changed.
63. **What W3 does not do.** No floating-point `printf` in the msvcrt path (a MinGW program that uses `__USE_MINGW_ANSI_STDIO` brings its own formatter and does work); every code page is treated as UTF-8; the heap is one 8 MiB budget with no `HeapCreate`, `VirtualAlloc` or `LocalAlloc`; `VirtualProtect` and `VirtualQuery` only know the image's own pages and refuse writable-and-executable; structured exception handling always answers "continue search"; there is no stdin, file I/O, environment, time or locale; under Aphelion the command line is the fixed string `pe.exe`, because the host starts with one handle and has no way to be given arguments yet. `Sleep` is a wait with a timeout on a pipe nothing writes to, because there is no clock syscall.
64. **`extern "C"` objects declared inside an anonymous namespace link as C++ names on some toolchains.** The self-test declared the embedded blob symbols (`apx_hello_start`, `pe_console_start` and the rest) in the anonymous namespace next to the functions that use them. Under the sandbox's GCC 13 they linked as plain C symbols and every boot passed; on the CI runner the linker reported `undefined reference to userland::(anonymous namespace)::apx_hello_start`, i.e. the compiler gave them internal, namespace-qualified names that `apps.asm` cannot define. The declarations now sit at file scope, as the `ublob_*` ones always did. It was not a build-order problem (the kernel embeds already-built userland files; no userland program depends on `kernel.elf`). The failure could not be reproduced here, so the fix is verified only to build and boot on GCC 13; the next CI run is the check.
65. **Programs can be run from the shell.** `userland::apps::install()` writes the bundled programs to `ram:/apps` when the shell starts (not in test mode, where the self-test makes its own directory), and the `run` command launches one: a file beginning with `MZ` through `pehost`, anything else as APX. `debug_write` now also feeds an optional kernel console sink (`userland::set_console`, taken under a spinlock because the writer is another core's syscall), which the launcher buffers in an 8 KiB ring and the shell drains every 40 ms, so program output appears on the shell's screen and not only on the serial log. A full ring drops bytes and says so. One program runs at a time (a mutex), Ctrl+C kills it, and the handle table used for the launch is closed afterwards. Checked in a real QEMU shell session by `tools/ci-shell.sh`.
66. **`peinfo --supported` never matched anything with a capital letter.** The loader reads a `dll!function` list; it folded the whole line to lower case when loading it, but compared against a label whose function part keeps its case, so `kernel32.dll!GetLastError` was always reported MISSING. Nobody noticed because the list option had never been run against a real list. Windows export names are case-sensitive and DLL names are not; only the part before `!` is folded now. Found when W4 generated the first real list from the executor's tables and the first coverage run reported every import as missing.
67. **A supported-imports list that is written by hand goes stale, so it is generated.** The list is the output of the same tables the host binds against, sorted, committed as `docs/pe-supported.txt`, and `make test-pehost` compares the committed file with a fresh one. The ranked "what to implement next" report is only as good as that list; a second check runs the real host on a program with gaps and requires the host's own "missing import" lines to equal what `peinfo` reports.
68. **One function decides what a DLL name means, and three programs call it.** API-set names are folded in `pe::modules::canonical` (`kernel/include/cosmos/pe_modules.hpp`), which the import resolver, the run-time module calls and `peinfo` all include. Having `peinfo` carry its own copy of the rule was the alternative; it would have been correct on the day it was written and wrong the first time the host changed. The rule is an approximation (every non-CRT api set goes to the `kernel32` table) and says so in `docs/WINDOWS.md`.
69. **`crt.exe` was embedded in the test header and never run by the host test.** `pe_samples_mingw.hpp` carried it since W3 and only the kernel boot executed it, so a host-side regression in the C runtime would have shown up as a failed boot instead of a named check. `pehost_host_test` now runs `crt.exe` and the new `ucrt.exe` and compares their whole output and exit status, and `ucrt.exe` once more with a quoted, non-ASCII command line. `msvcrt::reset()` was also dead code (every test forks or sets state itself); `ucrt::reset()` is used by the new tests.
70. **The UCRT output flags are checked against the MinGW headers, not against Microsoft's library.** `ucrt.exe`'s `snprintf` returning 10 with a terminator and `_snprintf` returning -1 show that the bit MinGW passes for standard behaviour is the one the stub reads, and the absent bit gives the legacy result. What Microsoft's `ucrtbase.dll` does with the other documented bit is not known here; the stub ignores it, and `docs/WINDOWS.md` lists it as unverified. Nothing here was run on Windows.
71. **The onexit table functions are checked to failure.** `_register_onexit_function` grows a table through the process heap; the test registers entries until the 8 MiB heap refuses, then checks that the failing call leaves the table intact and that every entry registered before it still runs. A test that stopped at a fixed count never reached that branch (the first version of it did exactly that, at 100,000 entries, and the check failed because the heap had not yet run out).
72. **A variable that shadows another in a recovery path is invisible to ASan.** In `load_image` a loop pointer named `raw` hid the reservation `raw`, so the module record stored the address of itself, and `FreeLibrary` unmapped the page holding the module table. Every sanitizer run passed (the unmap was a legitimate `munmap` of a legitimate mapping, just not the intended one); only an `-O1` build crashed, because the table happened to sit on a page the process then touched, and `-O0` laid it out differently and passed. The lib test now records every range the runtime reserves and fails if the manager unmaps anything else; reintroducing the bug fails exactly that check. The header was also compiled with `-Wshadow`, which finds nothing else.
73. **`TerminateProcess` and `ExitProcess` differ for DLLs, so the test has two children.** The first version routed both through the same exit and detached the DLLs for both. Windows does not call `DllMain` on `TerminateProcess`. A forked child loads `dllplug.dll` (with real page protections, since a DLL's code is executed) and each path is run: one prints the detach, the other does not. The test fails if the two are swapped.
74. **The program's directory is the only place a DLL can come from, and that is a handle, not a path.** The kernel moves a second handle (read and delegate rights on the directory) into the child at start-up, and the host opens DLLs relative to it. A program cannot reach anything outside that directory even by importing a DLL called `..\x`, because only the file name is used and a name of only dots is refused. The entry point of every ring-3 program became `aphelion_main(u64, u64)`; `user_enter` already zeroed every register, so passing the second handle costs one `mov`.
75. **`peinfo` follows a DLL beside the program.** The coverage report used to call `dyn.exe` blocked because `dllchain.dll!add3` is not in the built-in list. It now looks for the file next to the program, checks the export exists (a forwarder counts) and recursively checks that DLL's own imports, to a depth of 4. A test copies `dyn.exe` alone into a directory and expects the report to name `dllchain.dll!add3`.

76. **Real binaries ask for different things than the ones you wrote.** Everything up to W5b was a MinGW program, which links the C runtime from `msvcrt.dll` and needs a small part of kernel32. `ninja.exe` from PyPI links the MSVC runtime statically and imports 136 kernel32 functions; the first run crashed at exit because `PEB->ProcessParameters` was null, which no MinGW program reads. The only way to find that kind of gap was to run something nobody on this project had written. `tools/real-binaries.sh` keeps that possible without committing third-party binaries: it downloads into a directory outside the repository, refuses to use one inside it, prints the hashes and runs the program only in the harness.
77. **"All imports provided" and "will work" are different claims, so the stubs have their own tier.** A function that exists only to make the loader succeed (`CreateProcessA`, `CreateIoCompletionPort`) used to be indistinguishable from a working one in the coverage report. The list now marks them ("stub"), `peinfo` says "20 of them always fail" and prints an ALWAYS FAILS marker for a single file, and the generated `docs/pe-supported.txt` carries the mark so the comparison in `make test-pehost` also catches a stub that quietly became real or the reverse.
78. **A wait nobody can end is an error today and a bug tomorrow.** With one thread an infinite condition wait or a second acquisition of an exclusive lock cannot ever finish, so the host stops the process with `STATUS_POSSIBLE_DEADLOCK` and says why, instead of hanging a test or the machine. That choice is only right while there is one thread, and is listed as such in the limits so it is revisited when threads arrive.
79. **Timestamps are 64-bit values written through `u32` halves.** The first version stored a `FILETIME` with a `u64` store into a structure that Windows programs place at 4-byte alignment, which UBSan reported at once; `put_filetime` writes two `u32` stores. A second class of bug in the same area: scaling a counter to milliseconds as `counter * 1000 / frequency` overflows after about 213 days on a 1 GHz counter, so the scale divides first and handles the remainder separately, and a test with a counter of 2^56 pins it.
80. **Three functions were wrong in the first draft and were found by reading, before any test could.** `GetModuleFileNameW` left the buffer unfilled when it truncated, `GetTempPathA` wrote a character and overwrote it, and `VerifyVersionInfoA` read the service-pack fields of a structure that may be only 148 bytes long. The tests written afterwards pin all three (a truncated name keeps its prefix and a terminator; the extended fields need the 156-byte structure), and environment names became case-insensitive when the first test used a second spelling.
81. **A rebuilt DLL is not byte-identical to the committed one, so the committed binaries stay.** Regenerating the samples with the sandbox's MinGW (GCC 13, win32 threads) reproduces the executables exactly but changes bytes in each of the four DLLs (60 in `dllmath.dll`, the one compared), so only the new `sysio.exe` was added and the four existing DLLs were left as committed. The tests do not depend on the bytes; reproducibility of DLLs is an open item.
82. **A directory handle without the Enumerate right makes a search look empty.** `sysio.exe` printed "search: 0 programs" and failed one check on the first boot because the directory handle given to the PE host had Read and Delegate but not Enumerate, and the host's directory hook reports every error as the end of the listing. The launcher and the self-test now give the handle Enumerate. The hook still cannot tell an error from the end; that is a limit, not a fix.
83. **A program in a pipeline needed no new pipeline.** Helm already carried bytes or rows between commands and turned one into the other when the next command wanted it, so a program is one more command found by name when no verb matches: its input is what the previous stage produced, its output is what the next one reads. The shell asks a resolver (`set_program_resolver`) and never learns what a PE or an APX file is; the launcher picks the executor from the first two bytes. Built-in verbs win over programs of the same name; a mutation that gives programs priority fails five host checks.
84. **Two mutants of the start-up decoder survived the first round, for different reasons.** One removed a check that a later check made redundant (a message with no text and a non-zero string count), so the check was simplified instead of tested. The other removed the guard on the string loop and still returned the right answer, because the later count comparison caught it; what it lost was memory safety, since the loop wrote into fixed-size lists. A test with 240 empty strings now fails it under ASan. A mutation that gives the right result and a wrong memory access is invisible to a result check.
85. **Changing a default breaks the test that encoded it, and the test was right to.** `crt.exe` checked that the command line contained `pe.exe`, the fixed name the host used to give every program; with real arguments it is `crt.exe`. The check now looks for `.exe`. The host harness still names the module `pe.exe` and the kernel names it after the program, so `argv.exe` prints a different module line in the two places and each test states the one it expects.
86. **A pointer into the caller's stack is not a record of what was asked.** The first Helm test kept the name the resolver was given as a pointer, and ASan reported a use after return, because the name lives in a stack buffer of the command expander. The fake now copies it. The same rule is why the start-up message is decoded in place in a buffer the program owns for its whole life.
