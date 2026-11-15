#include "internal.hpp"
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

namespace userland {

namespace impl {

namespace {

volatile u32 g_nmi_expected = 0;

volatile u64 g_nmi_from_user = 0;

volatile u64 g_nmi_absorbed = 0;

u64 g_phys_mask = 0;

bool owns_frame(const Process* p, u64 frame) {
    for (u32 i = 0; i < p->frame_count; ++i) {
        if ((p->frames[i] & ringabort::FRAME_MASK) == frame) return true;
    }
    return false;
}

bool vm_owns_frame(Process* p, u64 frame, bool poison) {
    bool hit = false;
    for (u32 i = 0; i < p->regions.count(); ++i) {
        memobj::Object* o = static_cast<memobj::Object*>(p->regions.at(i).object);
        if (!memobj::owns(o, frame)) continue;
        hit = true;
        if (poison) memobj::poison(o, frame);
        else break;
    }
    return hit;
}

void on_user_fault(idt::Frame* f) {
    Process* p = current_process();
    u64 cr2 = 0;
    if (f->vector == 14) asm volatile("mov %%cr2, %0" : "=r"(cr2));
    if (!p) {
        serial::printf("[userland] ring-3 exception %lu with no process attached, halting this core\n", f->vector);
        cpu::hang();
    }
    serial::printf("[userland] pid %u '%s' took exception %lu (error %lx) at rip=%p cr2=%p: terminated\n",
                    p->pid, p->name, f->vector, f->error_code, f->rip, cr2);
    cpu::sti();
    terminate(p, userabi::fault_status(f->vector));
}

u64 compute_phys_mask() {
    u32 a, b, c, d;
    u32 bits = 36;
    cpu::cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000008u) {
        cpu::cpuid(0x80000008u, 0, &a, &b, &c, &d);
        bits = a & 0xFF;
    }
    return ringabort::phys_mask(bits);
}

void acknowledge_platform_nmi() {
    u8 reason = cpu::in8(ringabort::NMI_PORT);
    if (reason == 0xFF) return;
    if (reason & ringabort::NMI_SERR) {
        cpu::out8(ringabort::NMI_PORT, ringabort::nmi_latch_assert(reason, ringabort::NMI_CLEAR_SERR));
        cpu::out8(ringabort::NMI_PORT, ringabort::nmi_latch_release(reason, ringabort::NMI_CLEAR_SERR));
        serial::writeln("[userland] NMI from ring 3: platform system-error latch (SERR) cleared");
    }
    if (reason & ringabort::NMI_IOCHK) {
        cpu::out8(ringabort::NMI_PORT, ringabort::nmi_latch_assert(reason, ringabort::NMI_CLEAR_IOCHK));
        cpu::out8(ringabort::NMI_PORT, ringabort::nmi_latch_release(reason, ringabort::NMI_CLEAR_IOCHK));
        serial::writeln("[userland] NMI from ring 3: platform I/O-check latch (IOCHK) cleared");
    }
}

bool user_machine_check(Process* p) {
    ringabort::Mce m{};
    m.mcg_status = cpu::rdmsr(ringabort::MSR_MCG_STATUS);
    u32 banks = static_cast<u32>(cpu::rdmsr(ringabort::MSR_MCG_CAP) & 0xFF);
    m.banks = banks < ringabort::MAX_BANKS ? banks : ringabort::MAX_BANKS;
    m.phys_mask = g_phys_mask;
    for (u32 i = 0; i < m.banks; ++i) {
        u32 base = ringabort::MSR_MC0_STATUS + i * ringabort::MSR_BANK_STRIDE;
        m.status[i] = cpu::rdmsr(base);
        if (m.status[i] & ringabort::STATUS_ADDRV) m.addr[i] = cpu::rdmsr(base + 1);
    }
    bool vm_held = p->vm.try_lock();
    ringabort::Verdict v = ringabort::judge_machine_check(true, m, [p, vm_held](u64 frame) {
        return owns_frame(p, frame) || (vm_held && vm_owns_frame(p, frame, false));
    });
    if (v.action != ringabort::Action::Kill) {
        if (vm_held) p->vm.unlock();
        return false;
    }

    for (u32 i = 0; i < v.poisoned; ++i) {
        p->poisoned[p->poisoned_count++] = v.frames[i];
        if (vm_held) vm_owns_frame(p, v.frames[i], true);
    }
    if (vm_held) p->vm.unlock();
    for (u32 i = 0; i < m.banks; ++i) {
        if (m.status[i] & ringabort::STATUS_VAL)
            cpu::wrmsr(ringabort::MSR_MC0_STATUS + i * ringabort::MSR_BANK_STRIDE, 0);
    }
    cpu::wrmsr(ringabort::MSR_MCG_STATUS, 0);
    serial::printf("[userland] pid %u '%s' hit an uncorrectable memory error in %u of its own frame(s), first %p: terminated, frame(s) quarantined\n",
                    p->pid, p->name, v.poisoned, v.frames[0]);
    terminate(p, userabi::fault_status(18));
}

bool on_user_abort(idt::Frame* f) {
    Process* p = current_process();
    if (!p) return false;
    switch (f->vector) {
    case 2:
        __atomic_add_fetch(&g_nmi_from_user, 1ull, __ATOMIC_RELAXED);
        acknowledge_platform_nmi();
        return true;
    case 8:
        serial::printf("[userland] pid %u '%s' took a double fault at rip=%p: terminated\n", p->pid, p->name, f->rip);
        terminate(p, userabi::fault_status(8));
    case 18:
        return user_machine_check(p);
    default:
        return false;
    }
}

void on_user_return(idt::Frame*) { die_if_killed(); }

bool on_kernel_nmi(idt::Frame*) {
    u32 left = __atomic_load_n(&g_nmi_expected, __ATOMIC_RELAXED);
    while (left) {
        if (__atomic_compare_exchange_n(&g_nmi_expected, &left, left - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            __atomic_add_fetch(&g_nmi_absorbed, 1ull, __ATOMIC_RELAXED);
            return true;
        }
    }
    return false;
}

}

void install_fault_handlers() {
    g_phys_mask = compute_phys_mask();
    idt::set_user_fault_handler(&on_user_fault);
    idt::set_user_abort_handler(&on_user_abort);
    idt::set_kernel_nmi_handler(&on_kernel_nmi);
    idt::set_user_return_handler(&on_user_return);
}
}

using namespace impl;

void expect_nmis(u32 count) { __atomic_store_n(&g_nmi_expected, count, __ATOMIC_RELEASE); }

u64 nmis_from_user() { return __atomic_load_n(&g_nmi_from_user, __ATOMIC_RELAXED); }

u64 nmis_absorbed() { return __atomic_load_n(&g_nmi_absorbed, __ATOMIC_RELAXED); }

}
