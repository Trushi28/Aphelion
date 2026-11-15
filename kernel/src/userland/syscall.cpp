#include "internal.hpp"
#include <cosmos/cpu.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/tlb.hpp>

extern "C" void syscall_entry();

namespace userland {

using namespace impl;

namespace {

constexpr u32 MSR_EFER = 0xC0000080;
constexpr u32 MSR_STAR = 0xC0000081;
constexpr u32 MSR_LSTAR = 0xC0000082;
constexpr u32 MSR_FMASK = 0xC0000084;
constexpr u32 MSR_GS_BASE = 0xC0000101;
constexpr u32 MSR_KERNEL_GS_BASE = 0xC0000102;
constexpr u64 EFER_SCE = 1ull;
constexpr u64 STAR_VALUE = (0x10ull << 48) | (0x08ull << 32);

using SysFn = i64 (*)(idt::Frame*);

const SysFn g_table[userabi::SYS_COUNT] = {
    &sys_exit,
    &sys_yield,
    &sys_gettid,
    &sys_debug_write,
    &sys_set_gs_base,
    &sys_get_gs_base,
    &sys_set_fs_base,
    &sys_get_fs_base,
    &sys_thread_create,
    &sys_thread_exit,
    &sys_thread_join,
    &sys_handle_close,
    &sys_handle_info,
    &sys_handle_narrow,
    &sys_channel_create,
    &sys_channel_send,
    &sys_channel_recv,
    &sys_handle_wait,
    &sys_vmo_create,
    &sys_vm_map,
    &sys_vm_unmap,
    &sys_vm_protect,
    &sys_vmo_size,
    &sys_file_open,
    &sys_file_read,
    &sys_file_write,
    &sys_file_stat,
    &sys_file_create,
    &sys_dir_entry,
    &sys_pipe_create,
    &sys_process_create,
    &sys_process_status,
    &sys_process_kill,
    &sys_clock,
    &sys_futex_wait,
    &sys_futex_wake,
    &sys_file_unlink,
    &sys_file_rename,
    &sys_file_resize,
};

static_assert(tlb::MAX_CORES <= 64, "a process records the cores it ran on as one bit per shootdown slot");

}

extern "C" void syscall_dispatch(idt::Frame* f) {
    Process* p = current_process();
    if (!p) {
        serial::writeln("[userland] syscall from a thread with no process attached, halting this core");
        cpu::hang();
    }
    u32 slot = tlb::core_index();
    if (slot < tlb::MAX_CORES) {
        u64 bit = 1ull << slot;
        if (!(__atomic_load_n(&p->core_mask, __ATOMIC_RELAXED) & bit)) __atomic_or_fetch(&p->core_mask, bit, __ATOMIC_RELAXED);
    }
    cpu::sti();
    u64 nr = f->rax;
    i64 result = nr < userabi::SYS_COUNT ? g_table[nr](f) : userabi::ERR_NOSYS;
    f->rax = static_cast<u64>(result);
    die_if_killed();
}

extern "C" NORETURN void user_bad_return() {
    Process* p = current_process();
    if (!p) cpu::hang();
    serial::printf("[userland] pid %u '%s' returned to a non-canonical address: terminated\n", p->pid, p->name);
    terminate(p, userabi::fault_status(13));
}

void init_core(u32 core) {
    u32 a, b, c, d;
    cpu::cpuid(0x80000000u, 0, &a, &b, &c, &d);
    bool ok = false;
    if (a >= 0x80000001u) {
        cpu::cpuid(0x80000001u, 0, &a, &b, &c, &d);
        ok = (d & (1u << 11)) != 0;
    }
    gdt::CoreLocal* local = gdt::core_local(core);
    if (!ok || !local) {
        if (core == 0) serial::writeln("[userland] this CPU or core slot cannot provide SYSCALL; ring-3 is disabled");
        return;
    }
    if (core == 0) {
        set_supported();
        install_fault_handlers();
    }
    cpu::wrmsr(MSR_EFER, cpu::rdmsr(MSR_EFER) | EFER_SCE);
    cpu::wrmsr(MSR_STAR, STAR_VALUE);
    cpu::wrmsr(MSR_LSTAR, reinterpret_cast<u64>(&syscall_entry));
    cpu::wrmsr(MSR_FMASK, userabi::SYSCALL_FMASK);
    cpu::wrmsr(MSR_KERNEL_GS_BASE, 0);
    cpu::wrmsr(MSR_GS_BASE, reinterpret_cast<u64>(local));
}

}
