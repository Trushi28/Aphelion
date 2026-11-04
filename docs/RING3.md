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
| 12 | handle_info | handle | `(kind << 16) | rights`, kind 3 being a stream, or -8 |
| 13 | handle_narrow | handle, rights mask, pointer to a u64 | 0, the new handle is stored through the pointer; its rights are the old ones ANDed with the mask |
| 14 | channel_create | pointer to two u64 | 0, the two ends are stored through the pointer |
| 15 | channel_send | channel, bytes, length (at most 256), handles, count (at most 4) | 0, or an error |
| 16 | channel_recv | channel, buffer, capacity, handle array, handle capacity, flags (bit 0 blocks) | `(handles << 32) | bytes`, or an error |
| 17 | handle_wait | pointer to entries `{handle u64, want u32, got u32}`, count (1 to 32), timeout in ms (all ones waits forever) | how many entries are ready, with `got` filled in, or an error |

The GS and FS bases are per thread and independent of each other. A new thread reads 0 for both. A base need not point at mapped memory; an access through `gs:` or `fs:` to an unmapped address faults like any other. A Linux ELF executor uses FS for thread-local storage and a Windows PE host uses GS for the TEB, so both are native.

Errors: -1 no such call, -2 bad pointer, -3 bad argument, -4 limit, -5 would block, -6 peer closed, -7 right missing, -8 bad or stale handle, -9 message or buffer too big, -10 timed out, -11 cancelled.

## Handles and channels (H1)

Every process owns a handle table: the Lens `Domain` of 128 slots, protected by a mutex because the threads of a process share it. A handle is a 64-bit value holding a slot index and a generation, so a closed handle never names a later object that reuses its slot. Each handle carries a rights mask, and rights can only shrink (`handle_narrow`, or the rights given when one is moved). The table is closed completely when the last thread of the process leaves, which is what makes a dying process visible to the peers of its channels at once and not when the parent gets around to reaping it.

A channel is a pair of ends. Each end can read, write and be delegated. A message is up to 256 bytes and up to 4 handles, and each end holds up to 16 messages waiting. Sending a handle moves it: it leaves the sender's table when the message is accepted and appears in the receiver's table, with the same rights, when the message is received. Nothing is copied and nothing can be sent twice. A handle can only be sent if it carries the Delegate right. A channel end cannot be sent over its own channel (that would let a queue hold the only reference to itself).

What is guaranteed:
- A send either moves every handle or none. Bad, stale, repeated or non-delegable handles refuse the whole send and every handle stays where it was with its old value.
- A receive either delivers the bytes and every handle or leaves the message queued. A buffer that is too small (-9), a table without room for the handles (-4) and a bad pointer (-2) all leave the message in the queue. The destination buffers are probed for writability before anything is consumed; this is sound because the address space never changes while a process runs.
- A message that was queued when its sender closed is still delivered. When the last reference to an end goes away, the messages waiting for that end are discarded and the handles inside them are released, so their peers see the hang-up.
- A receiver blocked in `channel_recv` or `handle_wait` is woken by `kill` and by a fatal fault in a sibling thread. The stream is pinned while a thread waits, so another thread closing the handle cannot free it underneath the waiter.

The kernel uses the same objects. `lens::Table` pairs a domain with its lock, `lens::channel::create/send/receive` take a table, and `userland::launch_with(name, image, table, handle)` moves one handle out of a kernel table into the new process and passes its value to the first thread in `rdi`. The handle is consumed even if the launch then fails.

Limits: a message carries 4 handles at most, so a service that has to hand over more must send several messages. There is no flow control beyond the 16-message queue. Cycles longer than one channel (end A of channel 1 queued on channel 2 whose only other end is queued on channel 1) leak, as they do in other capability kernels; the direct case is refused. Handles for files and memory objects come with H2 and H3, so today every handle a process can hold is a channel end.

## Threads, group exit and kill

A process is a group of threads that share one address space. Process creation starts one thread at the image entry. `thread_create` starts another at an entry point with the stack pointer the caller supplies and one argument in `rdi`, with every other register zero and FS and GS bases zero. The stack must already be mapped: the address space is built once and never changes while the process runs, which is what makes sharing it between cores safe without a lock. Until memory objects exist, a thread stack therefore lives in the data pages.

Limits: 8 threads alive at once and 16 created over the life of a process (a finished thread's slot is not reused, because teardown has to wait for every thread's kernel stack). Both report -4. The kernel's thread pool is shared, so `thread_create` also reports -4 when the pool is empty.

- `exit` ends the whole process. Every other thread is stopped.
- `thread_exit` ends the calling thread. When the last thread leaves, the process status is the status the last leaver passed, unless an `exit`, a fault or a kill came first, in which case that status wins.
- A fault in any thread (page fault, #GP, #UD, double fault, machine check) ends the whole process with the fault status.
- `thread_join` blocks until the named thread has finished and returns its status. Joining a finished thread again returns the same status. A join that is waiting when the process is being killed is woken and the thread dies.
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

Code at `0x400000` (read and execute), optional data at `0x800000` (read, write, no execute), a 32 KiB stack ending at `0x7FFFFFF00000` with an unmapped page below it. W^X is enforced by `constellation::space_map`.

## Verifying on your machine

1. `make test-userland` runs the copy and ABI tests under ASan and UBSan.
2. `make test-iso` then `tools/ci-boot.sh nvme 1` and `tools/ci-boot.sh nvme 4`, or `make run-test` and watch the serial console.
3. Expected serial lines:
   - `[user N] hello from ring 3`
   - one `[userland] pid N 'name' took exception ...: terminated` line for each of null, readkernel, jumpkernel, privileged, divzero, datapage and threadfault
   - `[userland] case 'name': status S, expected E` for twenty-two cases, none marked `<-- wrong status`
   - `[userland] 6 concurrent processes: all exited cleanly`
   - `[userland] lfence after the conditional swapgs: ordinary entry yes, paranoid entry yes`
   - `[userland] user GS and FS bases kept per thread across 8 concurrent processes: ok`
   - `[userland] user x87, SSE, AVX and MXCSR state kept per thread across 8 concurrent processes: ok`. On a boot where the kernel has no AVX state the text reads `SSE (this boot has no AVX state)` and the AVX programs are expected to report that. With no FPU support at all the line says `skipped`
   - `[userland] handle passing kernel -> ring 3 -> kernel: status 0, handle moved out of the sender, used by the receiver, returned`
   - `[userland] kill of a process blocked in channel receive and handle wait: 2 threads were blocked, status -512 after N ms, its channel hung up`
   - `[userland] two ring-3 processes talk over a channel the kernel handed each one an end of: statuses 0 and 0`
   - `[userland] kill 'spinner': 3 of 3 thread(s) were running, process ended with status -512 after N ms` for each of five kills (spinner and joinhang, some at once, some after a pause), none marked `<-- wrong`, then `[userland] kill of a process that already exited: refused, status stays 42`
   - With two or more cores: `[userland] NMI from ring 3: N sent, A landed in ring 3, B landed in ring 0 and were absorbed, process status 0`. With one core: `NMI from ring 3: skipped`. If A is 0 the next line says `inconclusive`
   - `[userland] frames still held: 0, processes still live: 0`
   - `[selftest] userland: ok`

## Where a first boot is most likely to fail

- Triple fault or reset right after `[userland]` lines start: the first `user_enter`. Suspects: GDT selector order against `STAR`, `TSS.rsp0` not yet set for that satellite, or the user mapping flags.
- `took exception 13` in the `hello` case: a selector or `swapgs` imbalance.
- Hang inside a syscall: `CoreLocal.kernel_rsp` not updated on that core.
- `could not be torn down`: a core still has the address space loaded after the thread exited.

## Not covered yet

- The kernel half of H1 (the syscalls, the handle close at process exit, `launch_with`) has been compiled and linked and its logic exercised on the host (`make test-lens`, `make test-userland`) and, for the ring-3 programs, under an emulator against a model of the syscalls. It has not been booted by me.
- A #DF or #MC that kills a process closes its handle table on the IST stack with interrupts off. That is harmless unless a channel lock is held by another core at that instant, in which case the exit path waits for it on that stack. Ordinary faults, syscalls and kills re-enable interrupts first.
- Memory is fixed at launch: no `mmap`, so a thread stack must sit in the data pages and the thread count is capped at 16 per process.
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
