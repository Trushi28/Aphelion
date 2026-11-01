#include <cosmos/userland.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/usercopy.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/vmm.hpp>

extern "C" void syscall_entry();
extern "C" NORETURN void user_enter(u64 rip, u64 rsp);

namespace userland {

constexpr u32 MAX_PROCESSES = 16;
constexpr u32 MAX_FRAMES = userabi::MAX_CODE_PAGES + userabi::MAX_DATA_PAGES + userabi::STACK_PAGES;
constexpr u64 PAGE = universe::PAGE_SIZE;

struct Process {
    bool in_use;
    u32 pid;
    u32 task_id;
    char name[orbital::NAME_BYTES];
    constellation::Space* space;
    u64 frames[MAX_FRAMES];
    u32 frame_count;
    u64 entry;
    u64 stack_top;
    orbital::WaitQueue done;
    volatile u32 exited;
    i32 status;
    volatile u64 core_mask;
};

namespace {

constexpr u32 MSR_EFER = 0xC0000080;
constexpr u32 MSR_STAR = 0xC0000081;
constexpr u32 MSR_LSTAR = 0xC0000082;
constexpr u32 MSR_FMASK = 0xC0000084;
constexpr u32 MSR_GS_BASE = 0xC0000101;
constexpr u32 MSR_KERNEL_GS_BASE = 0xC0000102;
constexpr u64 EFER_SCE = 1ull;
constexpr u64 STAR_VALUE = (0x10ull << 48) | (0x08ull << 32);

Process g_procs[MAX_PROCESSES];
cpu::Spinlock g_table_lock;
volatile u32 g_next_pid = 0;
volatile u64 g_frames_live = 0;
volatile bool g_supported = false;

struct SpaceReader {
    static constexpr u64 LIMIT = constellation::USER_LIMIT;
    constellation::Space* space;
    bool need_write;

    usercopy::Window window(u64 uva) {
        constellation::Mapping m{};
        if (!constellation::space_translate(space, uva, &m) || m.size == 0) return { nullptr, 0 };
        if (!(m.flags & constellation::PRESENT) || !(m.flags & constellation::USER)) return { nullptr, 0 };
        if (need_write && !(m.flags & constellation::WRITABLE)) return { nullptr, 0 };
        u64 off = uva & (m.size - 1);
        return { static_cast<u8*>(universe::to_virt(m.phys)), m.size - off };
    }
};

Process* current_process() {
    orbital::Satellite* s = orbital::current();
    return s ? static_cast<Process*>(s->owner) : nullptr;
}

NORETURN void terminate(Process* p, i32 status) {
    p->status = status;
    __atomic_store_n(&p->exited, 1u, __ATOMIC_RELEASE);
    p->done.wake_all();
    orbital::exit_with(status);
}

Process* acquire_slot() {
    u64 f = cpu::irq_save();
    g_table_lock.lock();
    Process* found = nullptr;
    for (auto& p : g_procs) {
        if (!p.in_use) { p.in_use = true; found = &p; break; }
    }
    g_table_lock.unlock();
    cpu::irq_restore(f);
    if (!found) return nullptr;
    found->pid = __atomic_add_fetch(&g_next_pid, 1u, __ATOMIC_RELAXED);
    found->task_id = 0;
    found->name[0] = 0;
    found->space = nullptr;
    found->frame_count = 0;
    found->entry = 0;
    found->stack_top = 0;
    found->exited = 0;
    found->status = 0;
    found->core_mask = 0;
    return found;
}

void release_slot(Process* p) {
    u64 f = cpu::irq_save();
    g_table_lock.lock();
    p->in_use = false;
    g_table_lock.unlock();
    cpu::irq_restore(f);
}

bool take_frame(Process* p, u64* phys) {
    if (p->frame_count >= MAX_FRAMES) return false;
    u64 frame = universe::alloc_zeroed(0);
    if (!frame) return false;
    p->frames[p->frame_count++] = frame;
    __atomic_add_fetch(&g_frames_live, 1ull, __ATOMIC_RELAXED);
    *phys = frame;
    return true;
}

bool map_fresh(Process* p, u64 va, u32 pages, u64 flags, const u8* src, u64 src_bytes) {
    for (u32 i = 0; i < pages; ++i) {
        u64 frame;
        if (!take_frame(p, &frame)) return false;
        u64 offset = static_cast<u64>(i) * PAGE;
        if (src && offset < src_bytes) {
            u64 n = src_bytes - offset < PAGE ? src_bytes - offset : PAGE;
            __builtin_memcpy(universe::to_virt(frame), src + offset, n);
        }
        if (!constellation::space_map(p->space, va + offset, frame, PAGE, flags)) return false;
    }
    return true;
}

bool build(Process* p, const Image& img) {
    if (!img.code || !img.code_bytes || img.code_bytes > userabi::MAX_CODE_PAGES * PAGE) return false;
    if (img.entry_offset >= img.code_bytes || img.data_pages > userabi::MAX_DATA_PAGES) return false;
    p->space = constellation::create_space();
    if (!p->space) return false;
    u32 code_pages = static_cast<u32>((img.code_bytes + PAGE - 1) / PAGE);
    if (!map_fresh(p, userabi::CODE_BASE, code_pages, 0, img.code, img.code_bytes)) return false;
    if (img.data_pages &&
        !map_fresh(p, userabi::DATA_BASE, img.data_pages, constellation::WRITABLE | constellation::NO_EXECUTE, nullptr, 0))
        return false;
    u64 stack_bottom = userabi::STACK_TOP - static_cast<u64>(userabi::STACK_PAGES) * PAGE;
    if (!map_fresh(p, stack_bottom, userabi::STACK_PAGES, constellation::WRITABLE | constellation::NO_EXECUTE, nullptr, 0))
        return false;
    p->entry = userabi::CODE_BASE + img.entry_offset;
    p->stack_top = userabi::STACK_TOP - 16;
    return true;
}

bool teardown(Process* p) {
    if (p->task_id) {
        if (!orbital::wait_reaped(p->task_id, orbital::FOREVER)) return false;
        p->task_id = 0;
    }
    if (p->space) {
        for (u32 spins = 0; constellation::space_cpumask(p->space) != 0; ++spins) {
            if (spins > 5000) return false;
            orbital::sleep_ms(1);
        }
    }
    for (u32 i = 0; i < p->frame_count; ++i) {
        universe::free(p->frames[i], 0);
        __atomic_sub_fetch(&g_frames_live, 1ull, __ATOMIC_RELAXED);
    }
    p->frame_count = 0;
    if (p->space) {
        constellation::destroy_space(p->space);
        p->space = nullptr;
    }
    return true;
}

void process_main(void* arg) {
    Process* p = static_cast<Process*>(arg);
    orbital::current()->owner = p;
    user_enter(p->entry, p->stack_top);
}

bool exited_ready(void* arg) { return __atomic_load_n(&static_cast<Process*>(arg)->exited, __ATOMIC_ACQUIRE) != 0; }

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
    if (!usercopy::copy_in(reader, uva, buf, len)) return userabi::ERR_FAULT;
    for (u64 i = 0; i < len; ++i) {
        u8 c = static_cast<u8>(buf[i]);
        if ((c < 0x20 && c != '\n') || c >= 0x7F) buf[i] = '.';
    }
    buf[len] = 0;
    serial::printf("[user %u] %s", p->pid, buf);
    if (len == 0 || buf[len - 1] != '\n') serial::putc('\n');
    return static_cast<i64>(len);
}

using SysFn = i64 (*)(idt::Frame*);

const SysFn g_table[userabi::SYS_COUNT] = {
    &sys_exit,
    &sys_yield,
    &sys_gettid,
    &sys_debug_write,
};

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
    terminate(p, userabi::fault_status(f->vector));
}

}

extern "C" void syscall_dispatch(idt::Frame* f) {
    Process* p = current_process();
    if (!p) {
        serial::writeln("[userland] syscall from a thread with no process attached, halting this core");
        cpu::hang();
    }
    __atomic_or_fetch(&p->core_mask, 1ull << (apic::id() & 63), __ATOMIC_RELAXED);
    cpu::sti();
    u64 nr = f->rax;
    i64 result = nr < userabi::SYS_COUNT ? g_table[nr](f) : userabi::ERR_NOSYS;
    f->rax = static_cast<u64>(result);
}

extern "C" NORETURN void user_bad_return() {
    Process* p = current_process();
    if (!p) cpu::hang();
    serial::printf("[userland] pid %u '%s' returned to a non-canonical address: terminated\n", p->pid, p->name);
    terminate(p, userabi::fault_status(13));
}

bool supported() { return g_supported; }

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
        g_supported = true;
        idt::set_user_fault_handler(&on_user_fault);
    }
    cpu::wrmsr(MSR_EFER, cpu::rdmsr(MSR_EFER) | EFER_SCE);
    cpu::wrmsr(MSR_STAR, STAR_VALUE);
    cpu::wrmsr(MSR_LSTAR, reinterpret_cast<u64>(&syscall_entry));
    cpu::wrmsr(MSR_FMASK, userabi::SYSCALL_FMASK);
    cpu::wrmsr(MSR_KERNEL_GS_BASE, 0);
    cpu::wrmsr(MSR_GS_BASE, reinterpret_cast<u64>(local));
}

Process* launch(const char* name, const Image& image) {
    if (!g_supported) return nullptr;
    Process* p = acquire_slot();
    if (!p) return nullptr;
    u32 n = 0;
    for (; name && name[n] && n + 1 < orbital::NAME_BYTES; ++n) p->name[n] = name[n];
    p->name[n] = 0;
    if (!build(p, image)) {
        if (teardown(p)) release_slot(p);
        return nullptr;
    }
    if (!orbital::spawn_in(p->name, &process_main, p, p->space, &p->task_id)) {
        if (teardown(p)) release_slot(p);
        return nullptr;
    }
    return p;
}

bool wait(Process* p, i32* status, u64 timeout_ms) {
    if (!p->done.wait_until_for(&exited_ready, p, timeout_ms)) return false;
    if (status) *status = p->status;
    return true;
}

bool reap(Process* p) {
    if (!teardown(p)) return false;
    release_slot(p);
    return true;
}

u32 pid_of(const Process* p) { return p->pid; }

u32 cores_used(const Process* p) {
    u64 m = p->core_mask;
    u32 n = 0;
    while (m) { n += static_cast<u32>(m & 1); m >>= 1; }
    return n;
}

u64 frames_live() { return __atomic_load_n(&g_frames_live, __ATOMIC_RELAXED); }

u32 live_processes() {
    u32 n = 0;
    for (auto& p : g_procs) if (__atomic_load_n(&p.in_use, __ATOMIC_ACQUIRE)) ++n;
    return n;
}

}
