#include "internal.hpp"
#include <cosmos/usergs.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/timesrc.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

namespace userland::impl {

namespace {

bool invariant_tsc() {
    static i32 cached = -1;
    i32 known = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (known >= 0) return known != 0;
    u32 a, b, c, d;
    cpu::cpuid(0x80000000u, 0, &a, &b, &c, &d);
    bool yes = false;
    if (a >= 0x80000007u) {
        cpu::cpuid(0x80000007u, 0, &a, &b, &c, &d);
        yes = (d & (1u << 8)) != 0;
    }
    __atomic_store_n(&cached, yes ? 1 : 0, __ATOMIC_RELAXED);
    return yes;
}

}

i64 sys_exit(idt::Frame* f) { terminate(current_process(), static_cast<i32>(f->rdi)); }

i64 sys_yield(idt::Frame*) {
    orbital::yield();
    return 0;
}

i64 sys_gettid(idt::Frame*) { return static_cast<i64>(orbital::current_id()); }

i64 sys_debug_write(idt::Frame* f) {
    Process* p = current_process();
    u64 uva = f->rdi;
    u64 len = f->rsi;
    if (len > userabi::DEBUG_WRITE_MAX) return userabi::ERR_INVAL;
    char buf[userabi::DEBUG_WRITE_MAX + 1];
    SpaceReader reader{ p->space, false };
    bool copied;
    {
        VmGuard vg(p->vm);
        copied = usercopy::copy_in(reader, uva, buf, len);
    }
    if (!copied) return userabi::ERR_FAULT;
    for (u64 i = 0; i < len; ++i) {
        u8 c = static_cast<u8>(buf[i]);
        if ((c < 0x20 && c != '\n') || c >= 0x7F) buf[i] = '.';
    }
    buf[len] = 0;
    console_write(buf, static_cast<u32>(len));
    serial::printf("[user %u] %s", p->pid, buf);
    if (len == 0 || buf[len - 1] != '\n') serial::putc('\n');
    return static_cast<i64>(len);
}

i64 sys_set_gs_base(idt::Frame* f) {
    if (!usergs::base_ok(f->rdi, userabi::USER_LIMIT)) return userabi::ERR_INVAL;
    cpu::wrmsr(usergs::MSR_KERNEL_GS_BASE, f->rdi);
    return 0;
}

i64 sys_get_gs_base(idt::Frame*) { return static_cast<i64>(cpu::rdmsr(usergs::MSR_KERNEL_GS_BASE)); }

i64 sys_set_fs_base(idt::Frame* f) {
    if (!usergs::base_ok(f->rdi, userabi::USER_LIMIT)) return userabi::ERR_INVAL;
    cpu::wrmsr(usergs::MSR_FS_BASE, f->rdi);
    return 0;
}

i64 sys_get_fs_base(idt::Frame*) { return static_cast<i64>(cpu::rdmsr(usergs::MSR_FS_BASE)); }

i64 sys_clock(idt::Frame* f) {
    Process* p = current_process();
    userabi::ClockRecord record{};
    record.uptime_ms = clock::uptime_ms();
    bool wall = clock::wall_valid();
    record.wall_ms = wall ? clock::now_ms() : 0;
    record.tsc_khz = timesrc::tsc_khz();
    record.flags = (wall ? userabi::CLOCK_WALL_VALID : 0) | (userabi::tsc_usable(record.tsc_khz, timesrc::last().assumed, invariant_tsc()) ? userabi::CLOCK_TSC_USABLE : 0);
    if (!user_write(p, f->rdi, &record, sizeof(record))) return userabi::ERR_FAULT;
    return 0;
}

}
