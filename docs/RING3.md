# Ring 3 foundation (series R)

This series lets the kernel run unprivileged code in its own address space, enter the kernel through `syscall`, survive a misbehaving program, and clean up after it. It is the base for the Corona userland renderer and for the APX, ELF and PE/NT executors.

## What was added

| Piece | Where |
|---|---|
| `syscall` entry and exit, `user_enter` | `kernel/src/arch/x86_64/syscall.asm` |
| `swapgs` on every ring-3 interrupt entry and exit, paranoid entry for NMI, #DF and #MC | `kernel/src/arch/x86_64/isr.asm` |
| Per-core `CoreLocal` (user rsp scratch, kernel rsp) reachable through GS | `gdt.hpp`, `gdt.cpp` |
| MSR setup per core (EFER.SCE, STAR, LSTAR, FMASK, GS bases) | `userland::init_core` |
| Process table, address-space build, thread launch, teardown | `kernel/src/userland/userland.cpp` |
| Native syscall ABI constants | `kernel/include/cosmos/userabi.hpp` |
| Safe user memory copy | `kernel/include/cosmos/usercopy.hpp` |
| User-fault hook (terminate the process instead of panicking) | `idt::set_user_fault_handler` |
| Per-thread user GS and FS bases: saved and restored on every context switch | `usergs.hpp`, `orbital.cpp` (`arrive`) |
| `lfence` after the conditional `swapgs`, and the byte-level check that it stays there | `isr.asm`, `entrycheck.hpp`, `userland_selftest.cpp` |
| Policy for NMI, #DF and #MC from ring 3, frame quarantine | `ringabort.hpp`, `userland.cpp`, `idt.cpp` |
| `IST_MCE` stack, `CR4.MCE` | `gdt.cpp`, `percore.cpp` |
| Threads in a process, group exit and external kill | `procgroup.hpp`, `userland.cpp`, `idt.cpp` (return-to-user hook) |
| Per-process handle table, channels that carry handles, handle syscalls | `lens_channel.hpp`, `lens_channel.cpp`, `lens.cpp` (`detach`, `attach`), `lens_stream.cpp` (`acquire`, `await_streams`), `handleabi.hpp`, `userland.cpp` |
| Memory objects, per-process region table, `vm_map`, `vm_unmap`, `vm_protect` | `memobj.hpp`, `memobj.cpp`, `vmregion.hpp`, `userland.cpp` |
| Page tables freed when a user range is unmapped | `vmm.cpp` (`reclaim_tables`) |
| Ring-3 test programs (including x87, SSE and AVX state checks) and orchestrator | `blobs.asm`, `userland_selftest.cpp` |
| Host tests for copy, ABI and GS-switch rules | `tools/userland_host_test.cpp` (`make test-userland`) |

## Invariants

- In ring 0, outside the first and last instructions of an entry stub, `GS_BASE` is this core's `CoreLocal` and `KERNEL_GS_BASE` is the running thread's user GS base. `orbital::arrive` saves it into the outgoing Satellite and loads the incoming one's on every switch that involves a thread with an address space, and loads 0 when a kernel thread runs. A new thread starts at 0. The FS base (`MSR_FS_BASE`) follows the same rule. `swapgs` does not touch it and the kernel never uses FS, so it needs no entry-stub handling, only the same save and restore on a switch.
- A user GS base must be below the canonical hole. The paranoid entry tells a kernel GS from a user GS by the sign of `GS_BASE`, so a user base in the kernel half would be mistaken for the kernel's.
- In ring 3 the two are swapped.
- Interrupts entering from ring 3 swap on entry and on exit. The decision is the saved CS, so it cannot disagree with the hardware.
- NMI, #DF and #MC can land inside the window between `syscall` and `swapgs`. They read `GS_BASE`, swap only when it is a user value, and swap back on exit.
- `syscall` runs with IF, TF, DF, AC, NT and IOPL cleared (`SFMASK`). Clearing AC matters because a user-set AC would turn SMAP off for the kernel.
- The kernel never dereferences a user virtual address. `usercopy` translates each page through the process page tables and reads or writes through the direct map, so SMAP never needs to be lifted and a bad pointer cannot fault in ring 0.
- `sysret` is used only when the return RIP is below the canonical hole. Otherwise the process is terminated, because `sysret` with a non-canonical RCX faults in ring 0 on Intel.
- The flags `sysret` restores come from user-controlled R11 and are masked to the arithmetic flags plus IF.
- Both interrupt entry stubs execute `lfence` after the conditional `swapgs` join point, so a mispredicted branch cannot run kernel code on a user GS base (the speculative-`swapgs` class). `syscall_entry` swaps unconditionally and needs no fence. The self-test reads the emitted bytes of `isr_common` and `isr_paranoid` and fails if an `lfence` does not follow the first `swapgs` within a few bytes, so removing the fence breaks the boot test.
- The kernel stack for `syscall` comes from `CoreLocal.kernel_rsp`, which `gdt::set_kernel_stack` updates together with `TSS.rsp0` on every context switch.

## Native syscall ABI (R subset)

Number in `rax`, arguments in `rdi, rsi, rdx, r10, r8, r9`, result in `rax`. Everything except `rax`, `rcx` and `r11` is preserved. Errors are small negative numbers.

| Nr | Name | Arguments | Result |
|---|---|---|---|
| 0 | exit | status | does not return |
| 1 | yield | none | 0 |
| 2 | gettid | none | thread id |
| 3 | debug_write | pointer, length (at most 512) | bytes written, or an error |
| 4 | set_gs_base | base, below `0x0000800000000000` | 0, or -3 for a base in or above the canonical hole |
| 5 | get_gs_base | none | the calling thread's GS base |
| 6 | set_fs_base | base, below `0x0000800000000000` | 0, or -3 for a base in or above the canonical hole |
| 7 | get_fs_base | none | the calling thread's FS base |
| 8 | thread_create | entry, stack top, argument (arrives in `rdi`) | the new thread's id, -3 for an entry or stack that is zero or at or above `0x0000800000000000`, -4 at the limits |
| 9 | thread_exit | status | does not return |
| 10 | thread_join | thread id | the joined thread's exit status, or -3 for an unknown id, the caller itself or an id above 32 bits |
| 11 | handle_close | handle | 0, or -8 for a bad or stale handle |
| 12 | handle_info | handle | `(subtype << 24) | (kind << 16) | rights`, kind 3 being a stream, subtype 1 a channel end and 2 a memory object (0 for files), or -8 |
| 13 | handle_narrow | handle, rights mask, pointer to a u64 | 0, the new handle is stored through the pointer; its rights are the old ones ANDed with the mask |
| 14 | channel_create | pointer to two u64 | 0, the two ends are stored through the pointer |
| 15 | channel_send | channel, bytes, length (at most 256), handles, count (at most 4) | 0, or an error |
| 16 | channel_recv | channel, buffer, capacity, handle array, handle capacity, flags (bit 0 blocks) | `(handles << 32) | bytes`, or an error |
| 17 | handle_wait | pointer to entries `{handle u64, want u32, got u32}`, count (1 to 32), timeout in ms (all ones waits forever) | how many entries are ready, with `got` filled in, or an error |
| 18 | vmo_create | pages (1 to 4096), flags (must be 0), pointer to a u64 | 0, the handle is stored through the pointer, with rights read, write, execute and delegate. -3 for zero pages or non-zero flags, -9 above 4096 pages, -4 when the system-wide budget of 16384 pages or the handle table is full |
| 19 | vm_map | memory object handle, address (0 chooses), offset in pages, pages, protection | the address mapped, or an error. Protection is 1 read, 3 read-write or 5 read-execute |
| 20 | vm_unmap | address, pages | 0, or -13 if any page of the range is not mapped (nothing is changed) |
| 21 | vm_protect | address, pages, protection | 0, or -13 if any page of the range is not mapped, -7 if the new protection is beyond what the mapping may have (nothing is changed) |
| 22 | vmo_size | memory object handle | the size in pages, or -3 for a handle that is not a memory object |

The GS and FS bases are per thread and independent of each other. A new thread reads 0 for both. A base need not point at mapped memory; an access through `gs:` or `fs:` to an unmapped address faults like any other. A Linux ELF executor uses FS for thread-local storage and a Windows PE host uses GS for the TEB, so both are native.

Errors: -1 no such call, -2 bad pointer, -3 bad argument, -4 limit, -5 would block, -6 peer closed, -7 right missing or protection refused, -8 bad or stale handle, -9 message or buffer too big, -10 timed out, -11 cancelled, -12 address range already mapped, -13 address range not mapped.

## Handles and channels (H1)

Every process owns a handle table: the Lens `Domain` of 128 slots, protected by a mutex because the threads of a process share it. A handle is a 64-bit value holding a slot index and a generation, so a closed handle never names a later object that reuses its slot. Each handle carries a rights mask, and rights can only shrink (`handle_narrow`, or the rights given when one is moved). The table is closed completely when the last thread of the process leaves, which is what makes a dying process visible to the peers of its channels at once and not when the parent gets around to reaping it.

A channel is a pair of ends. Each end can read, write and be delegated. A message is up to 256 bytes and up to 4 handles, and each end holds up to 16 messages waiting. Sending a handle moves it: it leaves the sender's table when the message is accepted and appears in the receiver's table, with the same rights, when the message is received. Nothing is copied and nothing can be sent twice. A handle can only be sent if it carries the Delegate right. A channel end cannot be sent over its own channel (that would let a queue hold the only reference to itself).

What is guaranteed:
- A send either moves every handle or none. Bad, stale, repeated or non-delegable handles refuse the whole send and every handle stays where it was with its old value.
- A receive either delivers the bytes and every handle or leaves the message queued. A buffer that is too small (-9), a table without room for the handles (-4) and a bad pointer (-2) all leave the message in the queue. The destination buffers are probed for writability before anything is consumed; this is sound because every kernel copy into or out of user memory holds the process's address-space lock, which `vm_map`, `vm_unmap` and `vm_protect` also hold.
- A message that was queued when its sender closed is still delivered. When the last reference to an end goes away, the messages waiting for that end are discarded and the handles inside them are released, so their peers see the hang-up.
- A receiver blocked in `channel_recv` or `handle_wait` is woken by `kill` and by a fatal fault in a sibling thread. The stream is pinned while a thread waits, so another thread closing the handle cannot free it underneath the waiter.

The kernel uses the same objects. `lens::Table` pairs a domain with its lock, `lens::channel::create/send/receive` take a table, and `userland::launch_with(name, image, table, handle)` moves one handle out of a kernel table into the new process and passes its value to the first thread in `rdi`. The handle is consumed even if the launch then fails.

Limits: a message carries 4 handles at most, so a service that has to hand over more must send several messages. There is no flow control beyond the 16-message queue. Cycles longer than one channel (end A of channel 1 queued on channel 2 whose only other end is queued on channel 1) leak, as they do in other capability kernels; the direct case is refused. Memory objects (H2) are the second kind of handle; files, pipes and process handles (H3) are the next.

## Threads, group exit and kill

A process is a group of threads that share one address space. Process creation starts one thread at the image entry. `thread_create` starts another at an entry point with the stack pointer the caller supplies and one argument in `rdi`, with every other register zero and FS and GS bases zero. The stack must already be mapped. It can be in the data pages or in memory mapped with `vm_map`; a thread whose stack pointer points at unmapped memory faults at its first push and the process dies.

Limits: 8 threads alive at once and 16 created over the life of a process (a finished thread's slot is not reused, because teardown has to wait for every thread's kernel stack). Both report -4. The kernel's thread pool is shared, so `thread_create` also reports -4 when the pool is empty.

- `exit` ends the whole process. Every other thread is stopped.
- `thread_exit` ends the calling thread. When the last thread leaves, the process status is the status the last leaver passed, unless an `exit`, a fault or a kill came first, in which case that status wins.
- A fault in any thread (page fault, #GP, #UD, double fault, machine check) ends the whole process with the fault status.
- `thread_join` blocks until the named thread has finished and returns its status. When it returns, the joined thread has also left the group, so a thread that joins another and then calls `thread_exit` is always the later leaver and its status is the one the process keeps if it is the last. The kernel waits for the joined thread's task to exit, bounded at 5 seconds, after it sees the thread's done flag. Joining a finished thread again returns the same status. A join that is waiting when the process is being killed is woken and the thread dies.
- `userland::kill(process)` is the external kill. It returns false if the process has already finished or is already being killed. The process status is `-512` (`STATUS_KILLED`).

How a kill reaches a thread. The group word (`procgroup.hpp`, one atomic 64-bit value holding the live count, a dying bit and the status) is checked in three places: before a new thread first enters ring 3, on the way out of every syscall, and after every interrupt that arrived from ring 3 (the interrupt-exit hook in `isr_dispatch`). A thread in a ring-3 loop with no syscalls therefore dies at its next timer tick, which is a few milliseconds. A thread that is queued dies when it is next scheduled. There is no inter-processor interrupt, so a kill can take as long as the longest time a thread waits for a core. A thread that is blocked in the kernel is only woken if it is in `thread_join`, the only blocking call a process has.

Setting the dying bit and publishing the status is one compare-and-swap, so a kill racing a thread's own `exit` or fault has exactly one winner, and a kill that arrives after the last thread left is refused.

A process killed by an exception exits with status `-256 - vector`, so a page fault is -270 and a general protection fault is -269.

## Fatal exceptions that arrive while a process is in ring 3

| Vector | What happens | Why |
|---|---|---|
| NMI (2) | Logged if the platform SERR or IOCHK latch is set (and cleared), then the process resumes | An NMI is a platform event, not something the process did. Killing it would let one core's hardware report end an innocent program |
| #DF (8) | The process is terminated with status `-264` | A double fault from ring 3 means exception delivery itself failed. The kernel stack in the TSS is valid, so the core can carry on |
| #MC (18) | Terminated with status `-274` only when every valid bank agrees the damage is confined to frames the process owns; otherwise the kernel panics | See below |

The same three vectors from ring 0 still panic, as before.

`#MC` has its own IST stack (`IST_MCE`, with a guard page) and `CR4.MCE` is now set on every core that reports MCE and MCA support. Without the IST stack a machine check landing between `syscall` and the stack switch would push its frame on the user's stack. Without `CR4.MCE` the CPU shuts down on a machine check instead of raising the exception.

A machine check kills the process only if all of these hold, using only status bits that mean the same thing on Intel and AMD: it came from ring 3, `MCG_STATUS.MCIP` and `EIPV` are set, no valid bank has `PCC` (processor context corrupt), every uncorrectable bank has no overflow, has a valid address, and that address (masked to the CPU's physical width, which also drops the LSB field AMD keeps in the top bits) is a frame the process owns. Anything else panics. The frames named are quarantined: teardown does not return them to the allocator, so the next process cannot be handed bad memory. The bank status registers and `MCG_STATUS.MCIP` are cleared before the process is terminated, because a set `MCIP` turns the next machine check into a shutdown.

Page-table frames and kernel memory are not in the process's frame list, so an error there panics.

## User address space

Code at `0x400000` (read and execute), optional data at `0x800000` (read, write, no execute), a 32 KiB stack ending at `0x7FFFFFF00000` with an unmapped page below it. These are built at launch and cannot be unmapped or changed from ring 3. W^X is enforced by `constellation::space_map`.

## Memory objects and mappings (H2)

A memory object is a fixed number of zeroed physical pages (up to 4096, 16 MiB) allocated when it is created, with a reference count. It is a Lens stream, so it lives in the handle table, moves over channels and narrows like any other handle. Rights: Read allows mapping, Write allows writable mappings, Execute allows executable mappings, Delegate allows sending the handle. A handle without Read cannot be mapped. `handle_narrow` can hand out a read-only view of the same pages.

Mappings live in the window `0x100000000000` to `0x7F0000000000`. Address 0 asks the kernel to choose (first fit from the bottom of the window); a non-zero address is exact, must be page aligned and inside the window, and is refused with -12 if any page of it is mapped, so a loader that wants a preferred base can try it and fall back to relocating. A process can have 64 mappings and 8192 mapped pages (32 MiB) at once; beyond that `vm_map` reports -4. Partial `vm_unmap` and `vm_protect` split a mapping, and `vm_protect` merges neighbours that become identical again, so toggling the protection of part of a mapping and restoring it does not use up the 64.

Protection is checked per mapping. Read-write (3) and read-execute (5) are accepted; write plus execute (7) is refused with -7, and so is anything the handle's rights do not cover. A page mapped read-write can be flipped to read-execute with `vm_protect` once code has been written to it (a JIT or loader pattern) and back. Executing a read-write page, or writing to a read-execute page, faults. `vm_protect` can never exceed the rights of the handle the mapping was made from, even if the handle is closed afterwards.

A mapping holds its own reference to the object, so closing every handle does not unmap anything, and the pages are freed when the last mapping in any process and the last handle are gone. Process exit releases all of a process's mappings. A process that is killed while its only reference to 16 pages is a mapping gives them back (the `vmspin` test checks this against `memobj::live_pages()`).

How it is made safe with several threads and cores. Each process has one address-space mutex. `vm_map`, `vm_unmap` and `vm_protect` take it, and so does every kernel copy to or from user memory, so a copy never sees a page vanish between its probe and its write, and a freed frame is never written through the direct map. `vm_unmap` removes the page-table entries and shoots down every core that has the space loaded before it drops its references, so a frame cannot be freed while another core can still reach it through a cached translation (`vmshoot` checks this: a second thread touches a page, the first unmaps it, and the second must then fault). The lock order is handle table, then address space. Empty page tables are returned when a range is unmapped, so mapping and unmapping at scattered fixed addresses does not use up memory.

Limits and things that are deliberately not done:
- W^X is a property of each mapping, not of the memory. A memory object with both a Write handle and an Execute handle can be mapped writable through one and executable through the other. Hand out handles narrowed to what each party needs; a loader that wants the stronger rule should keep the Write handle for itself and give out Execute-only ones after it has finished writing.
- Memory is allocated when the object is created, not on first touch, and there is no page-fault handler: touching a page that is not mapped kills the process. The budget is 16384 pages (64 MiB) over all memory objects in the system. One process can use all of it.
- There is no way to map less than a page, to map with no access, or to share a mapping copy-on-write.
- A machine check in a memory-object frame kills the process and quarantines the frame only when the address-space mutex is free at that instant; if another thread of the same process is inside `vm_map` or `vm_unmap` the frame is not recognised and the kernel panics, as it does for any frame it cannot attribute.
- A thread stack in mapped memory has no guard page unless the program leaves a gap itself.

## Files, pipes and processes from files (H3)

A file or directory handle is a Lens like any other, so a process can only reach what it was handed. `file_open` and `file_create` take a directory handle and a relative path; the result can never be wider than the directory handle (rights are checked as a subset), `..`, absolute paths and a realm prefix are refused with -3, and a handle given to a process is the root of everything it can name. The new syscalls, 23 to 32:

| nr | call | arguments | result |
|---|---|---|---|
| 23 | `file_open` | dir, path, path_len, rights, out | 0, handle in `*out` |
| 24 | `file_read` | handle, offset, buf, len | bytes read, 0 at end of file or when a pipe's writer is gone |
| 25 | `file_write` | handle, offset, buf, len | bytes written |
| 26 | `file_stat` | handle, out | 0, 32-byte record: size, mtime_ms, kind (0 file, 1 directory, 2 stream), links, flags |
| 27 | `file_create` | dir, path, path_len, rights, flags, out | 0; flag 1 makes a directory and `out` is not written |
| 28 | `dir_entry` | dir, index, out | 0 and a 280-byte record, or 1 when `index` is past the end |
| 29 | `pipe_create` | capacity, out[2] | 0, read end then write end |
| 30 | `process_create` | file, arg handle or 0, out, flags (0) | 0, process handle in `*out` |
| 31 | `process_status` | process handle, out | 0 and the status as a 64-bit value, or -5 while it runs |
| 32 | `process_kill` | process handle | 0, or -6 if it is already ending |
| 33 | `clock` | out | 0 and a 32-byte record: uptime_ms, wall_ms (0 unless the wall clock is trusted), tsc_khz, flags (1 wall valid, 2 TSC usable) |

New error codes: -14 for an image that fails validation, -15 not found, -16 wrong kind of object (a directory where a file is needed or the reverse). Everything else uses the codes from H1 and H2; -12 is "already exists" here too. A transfer moves at most 1024 bytes per call and returns the count, so callers loop. Pipes never block: an empty read or a full write returns -5, and `handle_wait` is how a process sleeps until a pipe is readable or writable. A pipe's read end carries Read and the write end carries Write, so the direction cannot be reversed by a process that holds only one end. A closed write end gives readers end-of-file once the buffer is drained; a closed read end makes writes return -6.

`process_create` reads an APX v1 image from a file handle that has both Read and Execute, validates it completely (below), builds the address space and starts one thread. The only handle the new process gets is the argument handle, which is moved out of the caller (it needs the Delegate right), and it arrives in `rdi`; a channel end is the usual choice and everything else is negotiated over it. The caller gets a process handle (Read, Write, Delegate). `handle_wait` on it reports Readable and Hangup once the process has exited, `process_status` returns the exit status (a kill gives -512, a fault gives -256 minus the vector), and `process_kill` ends it. Closing the last handle to a process kills it, as in Zircon, so there is no fire-and-forget spawn; a parent that wants a child to outlive it must hand the process handle to someone else first. Exited processes whose handles are gone are freed lazily by `sweep_detached()`, which `process_create` runs first, so the 16 process slots are not used up by finished children; the kernel self-test also calls it.

APX v1 is a 64-byte header followed by a code section and an optional initial-data section. Little-endian fields: magic `APX1`, version 1, header size 64, flags (must be 0), code offset, code size, entry offset into the code, data pages, data offset, data size, total file size, CRC-32 of everything after the header, and 20 reserved bytes that must be zero. Code goes at `0x400000` read and execute, data pages at `0x800000` read and write, with the initial bytes copied in and the rest zero. The parser (`kernel/include/cosmos/apx.hpp`) refuses a wrong magic, version, header size, flags or reserved bytes; a zero or oversized code section; an entry outside the code; more than 64 data pages or initial data larger than its pages; unaligned sections (16 bytes); sections that overlap or lie outside the file; a file size that differs from the header or from the end of the last section; and a checksum mismatch. Padding between sections is allowed and is covered by the checksum. The limits are 64 code pages and 64 data pages (raised from 16 and 8), so an image is at most 524,352 bytes. The whole file is read into a kernel buffer, validated, copied into fresh frames and freed.

Limits and things that are deliberately not done:
- The checksum detects accidents, not tampering. There is no signature, so Execute on a file is the only gate on what can run.
- File syscalls hold the caller's handle-table lock while the filesystem works, so a slow disk stalls the process's other handle operations (not other processes).
- `clock` reports the TSC as usable only when the frequency was measured against a timer (not assumed) and CPUID reports an invariant TSC; it does not verify that the cores' counters agree. Uptime is the 10 ms tick count.
- `dir_entry` lists the whole directory on every call and picks the entry at `index`, so a full walk is quadratic, the order is whatever the realm returns, and entries added or removed during a walk can be skipped or repeated.
- A pipe read copies out of the pipe before it copies into the caller's buffer; the buffer is probed first, but if another thread unmaps it in between those bytes are lost.
- Writing past the end of a file leaves the result to the realm.
- A process can create children until the 16 process slots are gone, and there is no per-process quota. Child threads and memory are limited per process as before.
- The child gets no environment, arguments or working directory beyond the one handle.
- Nothing from R2.3 through H3 has been booted by me. The syscall handlers, `process_create` on real cores, kill-on-close from a dying parent and the lazy sweeper have been compiled and linked, and the ring-3 programs have run under an emulator against a model of the syscalls, but only your boots count.

## Building ring-3 programs (W2a)

`make userland` builds every program in `USER_PROGRAMS` (currently `hello`) into `build/userland/<name>.apx`. The pipeline is plain host g++ and needs no cross compiler:

1. `userland/crt/start.cpp` plus `userland/<name>/*.cpp` are compiled freestanding (`-ffreestanding -nostdlib -static -no-pie -fno-exceptions -fno-rtti`) and linked with `userland/apx.ld` into an ELF. The linker script puts code and read-only data at `0x400000` (R+X) and data and bss at `0x800000` (R+W), which is exactly what an APX image describes.
2. `tools/apxpack` (`tools/apxpack.hpp` is the converter, tested on the host) reads that ELF and writes the APX image: it checks the program headers against the same constants the kernel uses and refuses anything the kernel loader would refuse, with a status that names the problem.
3. `kernel/src/userland/apps.asm` embeds `hello.apx` with `incbin`. The boot self-test writes it into the RAM realm and launches it as a process; it must exit with status 42.

`userland/include/aphelion.hpp` has the syscall wrappers. The argument order follows the kernel: `vm_map(vmo, address, offset_pages, pages, protection)` returns the mapped base (address 0 lets the kernel choose), and `vmo_create(pages, &handle)` takes no flags.

Limits, on purpose: no libc, no exceptions, no RTTI, and no global constructors (the linker script asserts that `.init_array` is empty, because nothing would run it). User code may use SSE and x87; the kernel saves that state. A program larger than 64 code pages or 64 data pages is refused by `apxpack`.

`make test-apxpack` builds the converter test with ASan and UBSan (`-fno-sanitize-recover=undefined`) and checks malformed ELFs, every limit at its exact boundary, every truncation, every single-byte change to the headers, 200,000 random multi-byte header rewrites, and the real `hello.elf`. Mutation check: 79 single-token mutants of `apxpack.hpp` were run against it and all 79 are killed. The first round left 12 survivors; most were exact-limit gaps (a 64-byte file, 64 program headers, code of exactly 64 pages, an output buffer of exactly the needed size, each magic byte on its own) and one was a check that could never be reached, which is now removed.

## Verifying on your machine

1. `make test-userland` runs the copy and ABI tests under ASan and UBSan.
2. `make test-iso` then `tools/ci-boot.sh nvme 1` and `tools/ci-boot.sh nvme 4`, or `make run-test` and watch the serial console.
3. Expected serial lines:
   - `[user N] hello from ring 3`
   - one `[userland] pid N 'name' took exception ...: terminated` line for each of null, readkernel, jumpkernel, privileged, divzero, datapage and threadfault
   - `[userland] case 'name': status S, expected E` for thirty-one cases, none marked `<-- wrong status`
   - `[userland] 6 concurrent processes: all exited cleanly`
   - `[userland] lfence after the conditional swapgs: ordinary entry yes, paranoid entry yes`
   - `[userland] user GS and FS bases kept per thread across 8 concurrent processes: ok`
   - `[userland] user x87, SSE, AVX and MXCSR state kept per thread across 8 concurrent processes: ok`. On a boot where the kernel has no AVX state the text reads `SSE (this boot has no AVX state)` and the AVX programs are expected to report that. With no FPU support at all the line says `skipped`
   - `[userland] handle passing kernel -> ring 3 -> kernel: status 0, handle moved out of the sender, used by the receiver, returned`
   - `[userland] kill of a process blocked in channel receive and handle wait: 2 threads were blocked, status -512 after N ms, its channel hung up`
   - `[userland] two ring-3 processes talk over a channel the kernel handed each one an end of: statuses 0 and 0`
   - `[userland] shared memory object: kernel-made, handed to ring 3 as read-write and read-only handles: status 0, the process's write is visible to the kernel`
   - `[userland] memory object freed once the last handle and mapping are gone: yes`
   - `[userland] kill of a process whose only reference to 16 pages is a mapping: status -512, 16 pages held, 0 left after teardown`
   - `[userland] process started from an APX file in the RAM realm: status 42`
   - `[userland] initial data from the image is in place and the rest of the data page is zero: status 66`
   - six `[userland] launching ...: error E, expected E` lines (no execute right -7, one flipped byte -14, shorter than a header -14, empty file -14, a directory -16, a handle that does not exist -8), none marked `<-- wrong`
   - `[userland] file and pipe syscalls from ring 3 on a directory handle: status 0, the file holds the data: yes, the nested file exists: yes`
   - `[userland] a process that spawns children from files, waits on them, kills one, and leaves two running: status 0, 4 children were reaped by the sweeper, nothing left: yes`
   - `[userland] kill 'spinner': 3 of 3 thread(s) were running, process ended with status -512 after N ms` for each of five kills (spinner and joinhang, some at once, some after a pause), none marked `<-- wrong`, then `[userland] kill of a process that already exited: refused, status stays 42`
   - With two or more cores: `[userland] NMI from ring 3: N sent, A landed in ring 3, B landed in ring 0 and were absorbed, process status 0`. With one core: `NMI from ring 3: skipped`. If A is 0 the next line says `inconclusive`
   - `[userland] frames still held: 0, processes still live: 0` followed by `[userland] memory objects still alive: 0, pages 0`
   - `[selftest] userland: ok`

## Where a first boot is most likely to fail

- Triple fault or reset right after `[userland]` lines start: the first `user_enter`. Suspects: GDT selector order against `STAR`, `TSS.rsp0` not yet set for that satellite, or the user mapping flags.
- `took exception 13` in the `hello` case: a selector or `swapgs` imbalance.
- Hang inside a syscall: `CoreLocal.kernel_rsp` not updated on that core.
- `could not be torn down`: a core still has the address space loaded after the thread exited.

## Not covered yet

- The kernel half of H1 (the syscalls, the handle close at process exit, `launch_with`) has been compiled and linked and its logic exercised on the host (`make test-lens`, `make test-userland`) and, for the ring-3 programs, under an emulator against a model of the syscalls. It has not been booted by me.
- A #DF or #MC that kills a process closes its handle table on the IST stack with interrupts off. That is harmless unless a channel lock is held by another core at that instant, in which case the exit path waits for it on that stack. Ordinary faults, syscalls and kills re-enable interrupts first.
- The kernel half of H2 (memory objects, the region table, the three mapping syscalls, the address-space mutex, MCE attribution) has been compiled and linked, its policy and the VMM page-table reclamation are covered by host tests with mutation checks, and the ring-3 programs ran under an emulator against a model of the syscalls. The syscall handlers themselves, the shootdown on real cores and the mutex under contention have not been booted by me. `vmshoot` and `vmrace` only mean something on two or more cores.
- The kernel half of H3 (the file and pipe syscalls, `process_create`, the process handle, the sweeper, APX loading) has been compiled and linked; the APX parser and the ABI helpers are covered by host tests (an independent oracle checks every truncation, every single-byte and single-bit change and 400,000 random header rewrites, and mutation checks on the parser) and the C++ and Python packers produce identical images. The ring-3 programs ran under an emulator against a model of the syscalls. The handlers themselves have not been booted by me.
- The thread count is capped at 16 per process.
- A kill has no inter-processor interrupt behind it, so its latency is bounded by the timer tick and by scheduling, not by a fixed number.
- The thread and kill tests were run on the host only as assembly logic under an emulator against a model of the syscalls; the kernel side (`userland.cpp`, the return hook, `user_enter` with an argument) has been compiled and linked but not booted by me.
- `CR4.FSGSBASE` is off, so ring 3 cannot use `wrgsbase`. Turning it on needs a different test than the GS sign in the paranoid entry, because user code could then put a kernel-half value in GS.
- The #DF and #MC termination paths are not exercised by any boot test. Software cannot raise either from ring 3 (`int 8` and `int 18` fault with #GP because their gates have DPL 0), and nothing can self-inject them. Their decision logic (`ringabort.hpp`) is covered by host tests including mutation checks, and the code compiles and links, but the first real run of those paths will be on hardware or under QEMU's `mce` monitor command.
- Machine-check bank registers are read through the legacy MSR addresses (`0x401` and up). Newer AMD parts with scalable MCA expose banks at other addresses, so on the target laptop the bank contents may read differently from what the policy expects. The effect is conservative: an unrecognised state panics rather than killing a process.
- A memory error larger than one page is not detected. The kernel poisons the one frame the address names.
- The NMI boot test needs at least two cores. It sends NMIs from one core to the core running a ring-3 spin loop. An NMI that lands in ring 0 is absorbed only while the test has announced one, and the test prints `inconclusive` when no NMI happened to land in ring 3. That outcome is not counted as a failure.
- No KPTI and no PCID, on purpose. KPTI answers Meltdown (rogue data cache load), which affects Intel cores and not AMD Zen, and the real-hardware target is a Zen 3 laptop. PCID only pays for itself together with KPTI or with very frequent address-space switches. If Aphelion is ever run on an affected Intel part, the kernel half of the address space is still mapped while ring 3 runs and user code can try to read it speculatively. Adding KPTI later means a second PML4 per space that maps only the entry stubs, the per-core data and the IST stacks.
- The vector-state test fills x87, XMM and YMM registers with a per-process pattern, spins across context switches and verifies every byte, so a missed save or restore shows up as a wrong exit status. It does not cover AVX-512, which the kernel does not enable.
- The kernel half of a new address space is copied when it is created. A kernel PML4 slot created later would not appear in spaces that already exist, which is how `create_space` behaved before this series.
