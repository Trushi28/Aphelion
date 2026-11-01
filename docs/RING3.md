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
| Ring-3 test programs and orchestrator | `blobs.asm`, `userland_selftest.cpp` |
| Host tests for copy and ABI rules | `tools/userland_host_test.cpp` (`make test-userland`) |

## Invariants

- In ring 0, outside the first and last instructions of an entry stub, `GS_BASE` is this core's `CoreLocal` and `KERNEL_GS_BASE` is the user's GS (always 0 in this series).
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

Errors: -1 no such call, -2 bad pointer, -3 bad argument, -4 limit.

A process killed by an exception exits with status `-256 - vector`, so a page fault is -270 and a general protection fault is -269.

## User address space

Code at `0x400000` (read and execute), optional data at `0x800000` (read, write, no execute), a 32 KiB stack ending at `0x7FFFFFF00000` with an unmapped page below it. W^X is enforced by `constellation::space_map`.

## Verifying on your machine

1. `make test-userland` runs the copy and ABI tests under ASan and UBSan.
2. `make test-iso` then `tools/ci-boot.sh nvme 1` and `tools/ci-boot.sh nvme 4`, or `make run-test` and watch the serial console.
3. Expected serial lines:
   - `[user N] hello from ring 3`
   - one `[userland] pid N 'name' took exception ...: terminated` line for each of null, readkernel, jumpkernel, privileged, divzero and datapage
   - `[userland] case 'name': status S, expected E` for ten cases, none marked `<-- wrong status`
   - `[userland] 6 concurrent processes: all exited cleanly`
   - `[userland] frames still held: 0, processes still live: 0`
   - `[selftest] userland: ok`

## Where a first boot is most likely to fail

- Triple fault or reset right after `[userland]` lines start: the first `user_enter`. Suspects: GDT selector order against `STAR`, `TSS.rsp0` not yet set for that satellite, or the user mapping flags.
- `took exception 13` in the `hello` case: a selector or `swapgs` imbalance.
- Hang inside a syscall: `CoreLocal.kernel_rsp` not updated on that core.
- `could not be torn down`: a core still has the address space loaded after the thread exited.

## Not covered yet

- One thread per process, no way to kill a running process from outside.
- No user GS base, so no TEB through GS. This is the next change before PE support.
- #DF, #MC and NMI arriving from ring 3 are not turned into process termination.
- No `lfence` after the conditional `swapgs` for the speculative-GS class of attacks, no KPTI, no PCID.
- User SSE and AVX state is saved per thread by the scheduler, but no test exercises it from ring 3.
- The kernel half of a new address space is copied when it is created. A kernel PML4 slot created later would not appear in spaces that already exist, which is how `create_space` behaved before this series.
