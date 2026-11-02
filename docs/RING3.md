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
| Per-thread user GS base: saved and restored on every context switch | `usergs.hpp`, `orbital.cpp` (`arrive`) |
| Policy for NMI, #DF and #MC from ring 3, frame quarantine | `ringabort.hpp`, `userland.cpp`, `idt.cpp` |
| `IST_MCE` stack, `CR4.MCE` | `gdt.cpp`, `percore.cpp` |
| Ring-3 test programs and orchestrator | `blobs.asm`, `userland_selftest.cpp` |
| Host tests for copy, ABI and GS-switch rules | `tools/userland_host_test.cpp` (`make test-userland`) |

## Invariants

- In ring 0, outside the first and last instructions of an entry stub, `GS_BASE` is this core's `CoreLocal` and `KERNEL_GS_BASE` is the running thread's user GS base. `orbital::arrive` saves it into the outgoing Satellite and loads the incoming one's on every switch that involves a thread with an address space, and loads 0 when a kernel thread runs. A new thread starts at 0.
- A user GS base must be below the canonical hole. The paranoid entry tells a kernel GS from a user GS by the sign of `GS_BASE`, so a user base in the kernel half would be mistaken for the kernel's.
- In ring 3 the two are swapped.
- Interrupts entering from ring 3 swap on entry and on exit. The decision is the saved CS, so it cannot disagree with the hardware.
- NMI, #DF and #MC can land inside the window between `syscall` and `swapgs`. They read `GS_BASE`, swap only when it is a user value, and swap back on exit.
- `syscall` runs with IF, TF, DF, AC, NT and IOPL cleared (`SFMASK`). Clearing AC matters because a user-set AC would turn SMAP off for the kernel.
- The kernel never dereferences a user virtual address. `usercopy` translates each page through the process page tables and reads or writes through the direct map, so SMAP never needs to be lifted and a bad pointer cannot fault in ring 0.
- `sysret` is used only when the return RIP is below the canonical hole. Otherwise the process is terminated, because `sysret` with a non-canonical RCX faults in ring 0 on Intel.
- The flags `sysret` restores come from user-controlled R11 and are masked to the arithmetic flags plus IF.
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

The GS base is per thread. A new thread reads 0. The base need not point at mapped memory; an access through `gs:` to an unmapped address faults like any other.

Errors: -1 no such call, -2 bad pointer, -3 bad argument, -4 limit.

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
   - one `[userland] pid N 'name' took exception ...: terminated` line for each of null, readkernel, jumpkernel, privileged, divzero and datapage
   - `[userland] case 'name': status S, expected E` for thirteen cases, none marked `<-- wrong status`
   - `[userland] 6 concurrent processes: all exited cleanly`
   - `[userland] user GS base kept per thread across 6 concurrent processes: ok`
   - With two or more cores: `[userland] NMI from ring 3: N sent, A landed in ring 3, B landed in ring 0 and were absorbed, process status 0`. With one core: `NMI from ring 3: skipped`. If A is 0 the next line says `inconclusive`
   - `[userland] frames still held: 0, processes still live: 0`
   - `[selftest] userland: ok`

## Where a first boot is most likely to fail

- Triple fault or reset right after `[userland]` lines start: the first `user_enter`. Suspects: GDT selector order against `STAR`, `TSS.rsp0` not yet set for that satellite, or the user mapping flags.
- `took exception 13` in the `hello` case: a selector or `swapgs` imbalance.
- Hang inside a syscall: `CoreLocal.kernel_rsp` not updated on that core.
- `could not be torn down`: a core still has the address space loaded after the thread exited.

## Not covered yet

- One thread per process, no way to kill a running process from outside.
- No user FS base. A Linux ELF executor needs it for thread-local storage. It is an ordinary MSR that `swapgs` does not touch, so it only needs the same save and restore on context switch.
- `CR4.FSGSBASE` is off, so ring 3 cannot use `wrgsbase`. Turning it on needs a different test than the GS sign in the paranoid entry, because user code could then put a kernel-half value in GS.
- The #DF and #MC termination paths are not exercised by any boot test. Software cannot raise either from ring 3 (`int 8` and `int 18` fault with #GP because their gates have DPL 0), and nothing can self-inject them. Their decision logic (`ringabort.hpp`) is covered by host tests including mutation checks, and the code compiles and links, but the first real run of those paths will be on hardware or under QEMU's `mce` monitor command.
- Machine-check bank registers are read through the legacy MSR addresses (`0x401` and up). Newer AMD parts with scalable MCA expose banks at other addresses, so on the target laptop the bank contents may read differently from what the policy expects. The effect is conservative: an unrecognised state panics rather than killing a process.
- A memory error larger than one page is not detected. The kernel poisons the one frame the address names.
- The NMI boot test needs at least two cores. It sends NMIs from one core to the core running a ring-3 spin loop. An NMI that lands in ring 0 is absorbed only while the test has announced one, and the test prints `inconclusive` when no NMI happened to land in ring 3. That outcome is not counted as a failure.
- No `lfence` after the conditional `swapgs` for the speculative-GS class of attacks, no KPTI, no PCID.
- User SSE and AVX state is saved per thread by the scheduler, but no test exercises it from ring 3.
- The kernel half of a new address space is copied when it is created. A kernel PML4 slot created later would not appear in spaces that already exist, which is how `create_space` behaved before this series.
