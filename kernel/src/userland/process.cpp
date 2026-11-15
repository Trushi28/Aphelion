#include "internal.hpp"
#include <cosmos/handleabi.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

extern "C" NORETURN void user_enter(u64 rip, u64 rsp, u64 arg, u64 arg2);

namespace userland {

namespace impl {

Process g_procs[MAX_PROCESSES];

cpu::Spinlock g_table_lock;

cpu::Spinlock g_console_lock;

ConsoleFn g_console = nullptr;

void* g_console_ctx = nullptr;

volatile u32 g_next_pid = 0;

volatile u64 g_frames_live = 0;

volatile u64 g_frames_quarantined = 0;

volatile u64 g_detached_reaped = 0;

volatile bool g_supported = false;

Process* current_process() {
    orbital::Satellite* s = orbital::current();
    return s ? static_cast<Process*>(s->owner) : nullptr;
}

Thread* current_thread() {
    orbital::Satellite* s = orbital::current();
    return s ? static_cast<Thread*>(s->arg) : nullptr;
}

void close_handles(Process* p) {
    p->handles.lock.lock();
    lens::close_all(p->handles.domain);
    p->handles.lock.unlock();
}

void announce_dying(Process* p) {
    p->events.wake_all();
    p->futex.wake_all();
    lens::stream_event();
}

NORETURN void leave_thread(Process* p, Thread* t, i32 own) {
    t->status = own;
    __atomic_store_n(&t->done, 1u, __ATOMIC_RELEASE);
    p->events.wake_all();
    procgroup::Leave l = procgroup::leave(&p->group, own);
    if (l.last) {
        close_handles(p);
        p->status = l.status;
        __atomic_store_n(&p->exited, 1u, __ATOMIC_RELEASE);
        p->done.wake_all();
        lens::stream_event();
    }
    orbital::exit_with(own);
}

NORETURN void terminate(Process* p, i32 status) {
    procgroup::request_exit(&p->group, status);
    announce_dying(p);
    leave_thread(p, current_thread(), status);
}

void die_if_killed() {
    Process* p = current_process();
    if (!p) return;
    if (!procgroup::dying(procgroup::load(&p->group))) return;
    cpu::sti();
    leave_thread(p, current_thread(), 0);
}

Process* acquire_slot() {
    u64 f = cpu::irq_save();
    g_table_lock.lock();
    Process* found = nullptr;
    for (auto& p : g_procs) {
        if (!p.in_use) {
            p.in_use = true;
            __atomic_store_n(&p.detached, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&p.reaping, 0u, __ATOMIC_RELEASE);
            found = &p;
            break;
        }
    }
    g_table_lock.unlock();
    cpu::irq_restore(f);
    if (!found) return nullptr;
    found->pid = __atomic_add_fetch(&g_next_pid, 1u, __ATOMIC_RELAXED);
    found->name[0] = 0;
    found->space = nullptr;
    found->frame_count = 0;
    found->poisoned_count = 0;
    for (auto& t : found->threads) t = Thread{};
    new (&found->handles) lens::Table();
    new (&found->vm) orbital::Mutex();
    new (&found->regions) vmregion::Map<>();
    found->roster.reset();
    found->group = 0;
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

bool is_poisoned(const Process* p, u64 frame) {
    for (u32 i = 0; i < p->poisoned_count; ++i) {
        if (p->poisoned[i] == frame) return true;
    }
    return false;
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
    if (img.data_bytes > static_cast<u64>(img.data_pages) * PAGE || (img.data_bytes && !img.data)) return false;
    p->space = constellation::create_space();
    if (!p->space) return false;
    u32 code_pages = static_cast<u32>((img.code_bytes + PAGE - 1) / PAGE);
    if (!map_fresh(p, userabi::CODE_BASE, code_pages, 0, img.code, img.code_bytes)) return false;
    if (img.data_pages &&
        !map_fresh(p, userabi::DATA_BASE, img.data_pages, constellation::WRITABLE | constellation::NO_EXECUTE, img.data, img.data_bytes))
        return false;
    u64 stack_bottom = userabi::STACK_TOP - static_cast<u64>(userabi::STACK_PAGES) * PAGE;
    if (!map_fresh(p, stack_bottom, userabi::STACK_PAGES, constellation::WRITABLE | constellation::NO_EXECUTE, nullptr, 0))
        return false;
    p->threads[0] = Thread{ p, 0, 0, 0, userabi::CODE_BASE + img.entry_offset, userabi::STACK_TOP - 16, 0, 0 };
    (void)p->roster.reserve();
    return true;
}

void release_regions(Process* p) {
    StreamOps ops;
    p->regions.clear(ops);
}

bool teardown(Process* p) {
    for (u32 i = 0, n = p->roster.count(); i < n; ++i) {
        Thread& t = p->threads[i];
        if (!t.task_id) continue;
        if (!orbital::wait_reaped(t.task_id, orbital::FOREVER)) return false;
        t.task_id = 0;
    }
    close_handles(p);
    if (p->space) {
        for (u32 spins = 0; constellation::space_cpumask(p->space) != 0; ++spins) {
            if (spins > 5000) return false;
            orbital::sleep_ms(1);
        }
    }
    for (u32 i = 0; i < p->frame_count; ++i) {
        if (is_poisoned(p, p->frames[i])) {
            __atomic_add_fetch(&g_frames_quarantined, 1ull, __ATOMIC_RELAXED);
        } else {
            universe::free(p->frames[i], 0);
        }
        __atomic_sub_fetch(&g_frames_live, 1ull, __ATOMIC_RELAXED);
    }
    p->frame_count = 0;
    release_regions(p);
    if (p->space) {
        constellation::destroy_space(p->space);
        p->space = nullptr;
    }
    return true;
}

void thread_main(void* arg) {
    Thread* t = static_cast<Thread*>(arg);
    Process* p = t->proc;
    orbital::current()->owner = p;
    die_if_killed();
    user_enter(t->rip, t->rsp, t->arg, t->arg2);
}

bool exited_ready(void* arg) { return __atomic_load_n(&static_cast<Process*>(arg)->exited, __ATOMIC_ACQUIRE) != 0; }

bool user_read(Process* p, u64 uva, void* dst, u64 n) {
    VmGuard vg(p->vm);
    SpaceReader reader{ p->space, false };
    return usercopy::copy_in(reader, uva, dst, n);
}

bool user_write(Process* p, u64 uva, const void* src, u64 n) {
    VmGuard vg(p->vm);
    SpaceReader writer{ p->space, true };
    return usercopy::copy_out(writer, uva, src, n);
}

bool group_dying(void* arg) { return procgroup::dying(procgroup::load(&static_cast<Process*>(arg)->group)); }

void console_write(const char* text, u32 length) {
    u64 fl = cpu::irq_save();
    g_console_lock.lock();
    if (g_console) g_console(g_console_ctx, text, length);
    g_console_lock.unlock();
    cpu::irq_restore(fl);
}

void set_supported() { g_supported = true; }

bool move_handle(Process* p, lens::Table* from, lens::Lens handle, u64* out) {
    lens::Parcel parcel;
    lens::Done moved = lens::Done::success();
    {
        TableGuard guard(from->lock);
        lens::Result<lens::Rights> rights = lens::rights_of(from->domain, handle);
        if (rights.ok()) moved = lens::detach(from->domain, handle, rights.value(), &parcel);
        else moved = lens::Done::fail(rights.status());
    }
    if (!moved.ok()) return false;
    lens::Result<lens::Lens> attached = lens::Result<lens::Lens>::fail(lens::Status::Io);
    {
        TableGuard guard(p->handles.lock);
        attached = lens::attach(p->handles.domain, parcel);
    }
    if (!attached.ok()) {
        lens::discard(parcel);
        return false;
    }
    *out = attached.value().raw;
    return true;
}

}

using namespace impl;

bool supported() { return g_supported; }

Process* launch_impl(const char* name, const Image& image, lens::Table* from, lens::Lens handle, lens::Lens handle2) {
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
    if (from) {
        u64 first = 0;
        u64 second = 0;
        bool ok = move_handle(p, from, handle, &first);
        if (ok && !handle2.is_null()) ok = move_handle(p, from, handle2, &second);
        if (!ok) {
            if (teardown(p)) release_slot(p);
            return nullptr;
        }
        p->threads[0].arg = first;
        p->threads[0].arg2 = second;
    }
    procgroup::start(&p->group);
    p->roster.publish(0);
    if (!orbital::spawn_in(p->name, &thread_main, &p->threads[0], p->space, &p->threads[0].task_id)) {
        if (teardown(p)) release_slot(p);
        return nullptr;
    }
    return p;
}

Process* launch(const char* name, const Image& image) { return launch_impl(name, image, nullptr, lens::Lens(), lens::Lens()); }

Process* launch_with(const char* name, const Image& image, lens::Table& from, lens::Lens handle) {
    return launch_impl(name, image, &from, handle, lens::Lens());
}

void set_console(ConsoleFn fn, void* ctx) {
    u64 fl = cpu::irq_save();
    g_console_lock.lock();
    g_console = fn;
    g_console_ctx = ctx;
    g_console_lock.unlock();
    cpu::irq_restore(fl);
}

u32 sweep_detached() {
    u32 reaped = 0;
    for (auto& p : g_procs) {
        bool claimed = false;
        u64 fl = cpu::irq_save();
        g_table_lock.lock();
        if (p.in_use && __atomic_load_n(&p.detached, __ATOMIC_ACQUIRE) && __atomic_load_n(&p.exited, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&p.reaping, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&p.reaping, 1u, __ATOMIC_RELEASE);
            claimed = true;
        }
        g_table_lock.unlock();
        cpu::irq_restore(fl);
        if (claimed && reap(&p)) {
            ++reaped;
            __atomic_add_fetch(&g_detached_reaped, 1ull, __ATOMIC_RELAXED);
        }
    }
    return reaped;
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
u32 task_of(const Process* p) { return p->threads[0].task_id; }

bool kill(Process* p) {
    if (!procgroup::request_exit(&p->group, userabi::STATUS_KILLED)) return false;
    announce_dying(p);
    return true;
}

u32 live_handles(Process* p) {
    TableGuard guard(p->handles.lock);
    return lens::live_count(p->handles.domain);
}

u32 live_threads(const Process* p) { return procgroup::live(procgroup::load(&p->group)); }
u32 threads_created(const Process* p) { return p->roster.count(); }

u32 cores_used(const Process* p) {
    u64 m = p->core_mask;
    u32 n = 0;
    while (m) { n += static_cast<u32>(m & 1); m >>= 1; }
    return n;
}

u64 frames_live() { return __atomic_load_n(&g_frames_live, __ATOMIC_RELAXED); }
u64 detached_reaped() { return __atomic_load_n(&g_detached_reaped, __ATOMIC_RELAXED); }
u64 frames_quarantined() { return __atomic_load_n(&g_frames_quarantined, __ATOMIC_RELAXED); }

u32 live_processes() {
    u32 n = 0;
    for (auto& p : g_procs) if (__atomic_load_n(&p.in_use, __ATOMIC_ACQUIRE)) ++n;
    return n;
}

}
