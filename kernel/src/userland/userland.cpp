#include <cosmos/userland.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/usercopy.hpp>
#include <cosmos/usergs.hpp>
#include <cosmos/ringabort.hpp>
#include <cosmos/procgroup.hpp>
#include <cosmos/handleabi.hpp>
#include <cosmos/lens_channel.hpp>
#include <cosmos/memobj.hpp>
#include <cosmos/apx.hpp>
#include <cosmos/fileabi.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/vmregion.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/timesrc.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/vmm.hpp>

extern "C" void syscall_entry();
extern "C" NORETURN void user_enter(u64 rip, u64 rsp, u64 arg, u64 arg2);

namespace userland {

constexpr u32 MAX_PROCESSES = 16;
constexpr u32 MAX_FRAMES = userabi::MAX_CODE_PAGES + userabi::MAX_DATA_PAGES + userabi::STACK_PAGES;
constexpr u64 PAGE = universe::PAGE_SIZE;

struct Process;

struct Thread {
    Process* proc;
    u32 task_id;
    volatile u32 done;
    i32 status;
    u64 rip;
    u64 rsp;
    u64 arg;
    u64 arg2;
};

struct Process {
    bool in_use;
    u32 pid;
    char name[orbital::NAME_BYTES];
    constellation::Space* space;
    u64 frames[MAX_FRAMES];
    u32 frame_count;
    u64 poisoned[ringabort::MAX_POISON];
    u32 poisoned_count;
    Thread threads[userabi::MAX_THREADS_TOTAL];
    volatile u32 created;
    u64 group;
    lens::Table handles;
    orbital::Mutex vm;
    vmregion::Map<> regions;
    orbital::WaitQueue events;
    orbital::WaitQueue done;
    volatile u32 exited;
    volatile u32 detached;
    volatile u32 reaping;
    i32 status;
    volatile u64 core_mask;
};

Process* launch_impl(const char* name, const Image& image, lens::Table* from, lens::Lens handle, lens::Lens handle2);

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
cpu::Spinlock g_console_lock;
ConsoleFn g_console = nullptr;
void* g_console_ctx = nullptr;
volatile u32 g_next_pid = 0;
volatile u64 g_frames_live = 0;
volatile u64 g_frames_quarantined = 0;
volatile u64 g_detached_reaped = 0;
volatile bool g_supported = false;
volatile u32 g_nmi_expected = 0;
volatile u64 g_nmi_from_user = 0;
volatile u64 g_nmi_absorbed = 0;
u64 g_phys_mask = 0;

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
    found->created = 0;
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

bool owns_frame(const Process* p, u64 frame) {
    for (u32 i = 0; i < p->frame_count; ++i) {
        if ((p->frames[i] & ringabort::FRAME_MASK) == frame) return true;
    }
    return false;
}

struct VmGuard {
    orbital::Mutex& m;
    explicit VmGuard(orbital::Mutex& x) : m(x) { m.lock(); }
    ~VmGuard() { m.unlock(); }
    VmGuard(const VmGuard&) = delete;
    VmGuard& operator=(const VmGuard&) = delete;
};

struct StreamOps {
    void retain(void* o) { static_cast<lens::Stream*>(o)->retain(); }
    void release(void* o) { static_cast<lens::Stream*>(o)->release(); }
};

struct DeferredOps {
    static constexpr u32 CAPACITY = vmregion::MAX_REGIONS + 8;
    void* pending[CAPACITY];
    u32 count = 0;
    void retain(void* o) { static_cast<lens::Stream*>(o)->retain(); }
    void release(void* o) {
        if (count < CAPACITY) pending[count++] = o;
    }
    void flush() {
        for (u32 i = 0; i < count; ++i) static_cast<lens::Stream*>(pending[i])->release();
        count = 0;
    }
};

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

constexpr u32 PROCESS_STREAM_TYPE = 0x434F5250;

class ProcessObj final : public lens::Stream {
public:
    Process* proc;
    u32 refs;

    const char* name() const override { return "process"; }
    lens::Result<usize> read(lens::Bytes) override { return lens::Result<usize>::fail(lens::Status::Unsupported); }
    lens::Result<usize> write(lens::ConstBytes) override { return lens::Result<usize>::fail(lens::Status::Unsupported); }
    lens::Readiness ready() override {
        lens::Readiness r;
        if (__atomic_load_n(&proc->exited, __ATOMIC_ACQUIRE)) r = lens::Ready::Readable | lens::Ready::Hangup;
        return r;
    }
    void retain() override { __atomic_add_fetch(&refs, 1u, __ATOMIC_ACQ_REL); }
    void release() override {
        if (__atomic_sub_fetch(&refs, 1u, __ATOMIC_ACQ_REL) != 0) return;
        Process* p = proc;
        (void)kill(p);
        __atomic_store_n(&p->detached, 1u, __ATOMIC_RELEASE);
        kheap::free(this);
    }
    u32 type() const override { return PROCESS_STREAM_TYPE; }
};

ProcessObj* process_of(lens::Slot* s) {
    if (s->kind != lens::NodeKind::Stream || !s->stream || s->stream->type() != PROCESS_STREAM_TYPE) return nullptr;
    return static_cast<ProcessObj*>(s->stream);
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
    p->created = 1;
    return true;
}

void release_regions(Process* p) {
    StreamOps ops;
    p->regions.clear(ops);
}

bool teardown(Process* p) {
    for (u32 i = 0; i < p->created; ++i) {
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

i64 sys_exit(idt::Frame* f) { terminate(current_process(), static_cast<i32>(f->rdi)); }

i64 sys_thread_exit(idt::Frame* f) { leave_thread(current_process(), current_thread(), static_cast<i32>(f->rdi)); }

i64 sys_thread_create(idt::Frame* f) {
    Process* p = current_process();
    if (!procgroup::entry_ok(f->rdi, f->rsi, userabi::USER_LIMIT)) return userabi::ERR_INVAL;
    if (!procgroup::try_enter(&p->group, userabi::MAX_LIVE_THREADS)) return userabi::ERR_LIMIT;
    u32 index = __atomic_load_n(&p->created, __ATOMIC_ACQUIRE);
    for (;;) {
        if (index >= userabi::MAX_THREADS_TOTAL) {
            procgroup::leave(&p->group, 0);
            return userabi::ERR_LIMIT;
        }
        if (__atomic_compare_exchange_n(&p->created, &index, index + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    Thread& t = p->threads[index];
    t = Thread{ p, 0, 0, 0, f->rdi, f->rsi, f->rdx, 0 };
    if (!orbital::spawn_in(p->name, &thread_main, &t, p->space, &t.task_id)) {
        t.task_id = 0;
        __atomic_store_n(&t.done, 1u, __ATOMIC_RELEASE);
        procgroup::leave(&p->group, 0);
        return userabi::ERR_LIMIT;
    }
    return static_cast<i64>(t.task_id);
}

struct JoinWait {
    Process* p;
    Thread* t;
};

bool join_ready(void* arg) {
    JoinWait* w = static_cast<JoinWait*>(arg);
    return __atomic_load_n(&w->t->done, __ATOMIC_ACQUIRE) != 0 || procgroup::dying(procgroup::load(&w->p->group));
}

constexpr u64 JOIN_SETTLE_MS = 5000;

i64 sys_thread_join(idt::Frame* f) {
    Process* p = current_process();
    Thread* self = current_thread();
    u32 tid = static_cast<u32>(f->rdi);
    if (!tid || f->rdi != tid) return userabi::ERR_INVAL;
    u32 created = __atomic_load_n(&p->created, __ATOMIC_ACQUIRE);
    Thread* target = nullptr;
    for (u32 i = 0; i < created; ++i) {
        if (__atomic_load_n(&p->threads[i].task_id, __ATOMIC_RELAXED) == tid) { target = &p->threads[i]; break; }
    }
    if (!target || target == self) return userabi::ERR_INVAL;
    JoinWait w{ p, target };
    p->events.wait_until(&join_ready, &w);
    if (!__atomic_load_n(&target->done, __ATOMIC_ACQUIRE)) return userabi::ERR_INVAL;
    i32 settled = 0;
    (void)orbital::join(tid, &settled, JOIN_SETTLE_MS);
    return static_cast<i64>(target->status);
}

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
    {
        u64 fl = cpu::irq_save();
        g_console_lock.lock();
        if (g_console) g_console(g_console_ctx, buf, static_cast<u32>(len));
        g_console_lock.unlock();
        cpu::irq_restore(fl);
    }
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

struct TableGuard {
    orbital::Mutex& m;
    explicit TableGuard(orbital::Mutex& x) : m(x) { m.lock(); }
    ~TableGuard() { m.unlock(); }
    TableGuard(const TableGuard&) = delete;
    TableGuard& operator=(const TableGuard&) = delete;
};

i64 sys_handle_close(idt::Frame* f) {
    Process* p = current_process();
    TableGuard guard(p->handles.lock);
    return handleabi::error_of(lens::close(p->handles.domain, lens::Lens(f->rdi)).status());
}

i64 sys_handle_info(idt::Frame* f) {
    Process* p = current_process();
    TableGuard guard(p->handles.lock);
    lens::Lens l(f->rdi);
    lens::Result<lens::Rights> r = lens::rights_of(p->handles.domain, l);
    if (!r.ok()) return handleabi::error_of(r.status());
    lens::Result<lens::NodeKind> k = lens::kind_of(p->handles.domain, l);
    if (!k.ok()) return handleabi::error_of(k.status());
    u64 subtype = handleabi::SUBTYPE_NONE;
    if (k.value() == lens::NodeKind::Stream) {
        lens::Result<lens::Slot*> sr = lens::slot_of(p->handles.domain, l);
        if (sr.ok() && sr.value()->stream) {
            u32 type = sr.value()->stream->type();
            if (type == lens::channel::STREAM_TYPE) subtype = handleabi::SUBTYPE_CHANNEL;
            else if (type == memobj::STREAM_TYPE) subtype = handleabi::SUBTYPE_MEMOBJ;
            else if (type == PROCESS_STREAM_TYPE) subtype = handleabi::SUBTYPE_PROCESS;
        }
    }
    return static_cast<i64>(handleabi::info_pack(r.value().bits, static_cast<u8>(k.value()), static_cast<u8>(subtype)));
}

i64 sys_handle_narrow(idt::Frame* f) {
    Process* p = current_process();
    u64 zero = 0;
    if (!user_write(p, f->rdx, &zero, sizeof(zero))) return userabi::ERR_FAULT;
    TableGuard guard(p->handles.lock);
    lens::Result<lens::Lens> r = lens::narrow(p->handles.domain, lens::Lens(f->rdi), lens::Rights::from_bits(static_cast<u32>(f->rsi)));
    if (!r.ok()) return handleabi::error_of(r.status());
    u64 raw = r.value().raw;
    user_write(p, f->rdx, &raw, sizeof(raw));
    return 0;
}

i64 sys_channel_create(idt::Frame* f) {
    Process* p = current_process();
    u64 pair[2] = { 0, 0 };
    if (!user_write(p, f->rdi, pair, sizeof(pair))) return userabi::ERR_FAULT;
    lens::Lens a, b;
    lens::Done d = lens::channel::create(p->handles, &a, &b);
    if (!d.ok()) return handleabi::error_of(d.status());
    pair[0] = a.raw;
    pair[1] = b.raw;
    user_write(p, f->rdi, pair, sizeof(pair));
    return 0;
}

i64 sys_channel_send(idt::Frame* f) {
    Process* p = current_process();
    u64 len = f->rdx;
    u64 count = f->r8;
    if (len > lens::channel::MAX_BYTES || count > lens::channel::MAX_HANDLES) return handleabi::ERR_TOOBIG;
    u8 bytes[lens::channel::MAX_BYTES];
    u64 raw[lens::channel::MAX_HANDLES];
    if (len && !user_read(p, f->rsi, bytes, len)) return userabi::ERR_FAULT;
    if (count && !user_read(p, f->r10, raw, count * sizeof(u64))) return userabi::ERR_FAULT;
    lens::Lens handles[lens::channel::MAX_HANDLES];
    for (u64 i = 0; i < count; ++i) handles[i] = lens::Lens(raw[i]);
    lens::Done d = lens::channel::send(p->handles, lens::Lens(f->rdi), lens::ConstBytes(bytes, len), handles, static_cast<u32>(count));
    return handleabi::error_of(d.status());
}

i64 sys_channel_recv(idt::Frame* f) {
    Process* p = current_process();
    u64 cap = f->rdx < lens::channel::MAX_BYTES ? f->rdx : lens::channel::MAX_BYTES;
    u64 hcap = f->r8 < lens::channel::MAX_HANDLES ? f->r8 : lens::channel::MAX_HANDLES;
    u8 bytes[lens::channel::MAX_BYTES] = {};
    u64 raw[lens::channel::MAX_HANDLES] = {};
    if (cap && !user_write(p, f->rsi, bytes, cap)) return userabi::ERR_FAULT;
    if (hcap && !user_write(p, f->r10, raw, hcap * sizeof(u64))) return userabi::ERR_FAULT;
    lens::Lens handles[lens::channel::MAX_HANDLES];
    bool block = (f->r9 & handleabi::RECV_BLOCK) != 0;
    lens::Result<lens::channel::Received> r = lens::channel::receive(p->handles, lens::Lens(f->rdi), lens::Bytes(bytes, cap), handles,
                                                                     static_cast<u32>(hcap), block, &group_dying, p);
    if (!r.ok()) return handleabi::error_of(r.status());
    lens::channel::Received got = r.value();
    for (u32 i = 0; i < got.handles; ++i) raw[i] = handles[i].raw;
    bool wrote = (got.bytes == 0 || user_write(p, f->rsi, bytes, got.bytes)) &&
                 (got.handles == 0 || user_write(p, f->r10, raw, got.handles * sizeof(u64)));
    if (!wrote) {
        TableGuard guard(p->handles.lock);
        for (u32 i = 0; i < got.handles; ++i) (void)lens::close(p->handles.domain, handles[i]);
        return userabi::ERR_FAULT;
    }
    return handleabi::recv_pack(got.bytes, got.handles);
}

i64 sys_handle_wait(idt::Frame* f) {
    Process* p = current_process();
    u64 count = f->rsi;
    if (count == 0 || count > handleabi::MAX_WAIT_ENTRIES) return userabi::ERR_INVAL;
    handleabi::WaitEntry entries[handleabi::MAX_WAIT_ENTRIES];
    if (!user_read(p, f->rdi, entries, count * sizeof(handleabi::WaitEntry))) return userabi::ERR_FAULT;
    lens::Wait waits[handleabi::MAX_WAIT_ENTRIES];
    for (u64 i = 0; i < count; ++i) {
        if (!handleabi::wait_want_ok(entries[i].want)) return userabi::ERR_INVAL;
        lens::Readiness want;
        want.bits = static_cast<u8>(entries[i].want);
        waits[i] = lens::Wait{ lens::Lens(entries[i].handle), want, lens::Readiness() };
    }
    if (!user_write(p, f->rdi, entries, count * sizeof(handleabi::WaitEntry))) return userabi::ERR_FAULT;
    lens::Stream* streams[handleabi::MAX_WAIT_ENTRIES];
    {
        TableGuard guard(p->handles.lock);
        lens::Result<u32> pinned = lens::acquire(p->handles.domain, waits, static_cast<u32>(count), streams);
        if (!pinned.ok()) return handleabi::error_of(pinned.status());
    }
    lens::Result<u32> r = lens::await_streams(streams, waits, static_cast<u32>(count), f->rdx, &group_dying, p);
    lens::unpin(streams, static_cast<u32>(count));
    if (!r.ok()) return handleabi::error_of(r.status());
    for (u64 i = 0; i < count; ++i) entries[i].got = waits[i].got.bits;
    user_write(p, f->rdi, entries, count * sizeof(handleabi::WaitEntry));
    return static_cast<i64>(r.value());
}


struct MemRef {
    memobj::Object* object = nullptr;
    u32 max = 0;
    ~MemRef() { if (object) object->release(); }
    MemRef() = default;
    MemRef(const MemRef&) = delete;
    MemRef& operator=(const MemRef&) = delete;
};

i64 resolve_memobj(Process* p, u64 raw, bool for_map, MemRef* out) {
    TableGuard guard(p->handles.lock);
    lens::Result<lens::Slot*> sr = lens::slot_of(p->handles.domain, lens::Lens(raw));
    if (!sr.ok()) return handleabi::error_of(sr.status());
    lens::Slot* s = sr.value();
    memobj::Object* o = s->kind == lens::NodeKind::Stream ? memobj::from_stream(s->stream) : nullptr;
    if (!o) return userabi::ERR_INVAL;
    if (for_map) {
        if (!s->rights.has(lens::Right::Read)) return handleabi::ERR_DENIED;
        out->max = vmregion::PROT_READ | (s->rights.has(lens::Right::Write) ? vmregion::PROT_WRITE : 0u) |
                   (s->rights.has(lens::Right::Execute) ? vmregion::PROT_EXEC : 0u);
    }
    o->retain();
    out->object = o;
    return 0;
}

bool vm_args_ok(u64 pages, u64 prot) { return pages != 0 && pages <= vmregion::MAX_MAPPED_PAGES && prot <= 0xFFFFFFFFull; }

i64 sys_vmo_create(idt::Frame* f) {
    Process* p = current_process();
    if (f->rsi != 0 || f->rdi == 0) return userabi::ERR_INVAL;
    if (f->rdi > memobj::MAX_PAGES) return handleabi::ERR_TOOBIG;
    u64 raw = 0;
    if (!user_write(p, f->rdx, &raw, sizeof(raw))) return userabi::ERR_FAULT;
    memobj::Object* o = memobj::create(static_cast<u32>(f->rdi));
    if (!o) return userabi::ERR_LIMIT;
    {
        TableGuard guard(p->handles.lock);
        lens::Result<lens::Lens> r = lens::alloc_slot(p->handles.domain, nullptr, nebula::Node{0, 0}, nebula::Node{0, 0},
                                                      memobj::HANDLE_RIGHTS, false, 0, "memobj", o);
        if (!r.ok()) {
            memobj::discard(o);
            return handleabi::error_of(r.status());
        }
        raw = r.value().raw;
        if (!user_write(p, f->rdx, &raw, sizeof(raw))) {
            (void)lens::close(p->handles.domain, r.value());
            return userabi::ERR_FAULT;
        }
    }
    return 0;
}

i64 sys_vmo_size(idt::Frame* f) {
    Process* p = current_process();
    MemRef ref;
    i64 e = resolve_memobj(p, f->rdi, false, &ref);
    if (e) return e;
    return static_cast<i64>(ref.object->pages);
}

i64 sys_vm_map(idt::Frame* f) {
    Process* p = current_process();
    u64 addr = f->rsi;
    u64 offset = f->rdx;
    u64 pages = f->r10;
    u64 prot = f->r8;
    if (!vm_args_ok(pages, prot) || offset > memobj::MAX_PAGES) return userabi::ERR_INVAL;
    MemRef ref;
    i64 e = resolve_memobj(p, f->rdi, true, &ref);
    if (e) return e;
    memobj::Object* o = ref.object;
    if (pages > o->pages || offset > o->pages - pages) return userabi::ERR_INVAL;
    vmregion::Status ps = vmregion::check_prot(static_cast<u32>(prot), ref.max);
    if (ps != vmregion::Status::Ok) return handleabi::vm_error(ps);

    VmGuard vg(p->vm);
    for (u64 i = 0; i < pages; ++i)
        if (memobj::poisoned(o, static_cast<u32>(offset + i))) return userabi::ERR_FAULT;
    u64 base = addr;
    vmregion::Status st = addr ? p->regions.check_free(addr, static_cast<u32>(pages)) : p->regions.find_gap(static_cast<u32>(pages), &base);
    if (st != vmregion::Status::Ok) return handleabi::vm_error(st);
    vmregion::Region r{ base, static_cast<u32>(pages), static_cast<u32>(offset), static_cast<u32>(prot), ref.max, o };
    st = p->regions.insert(r);
    if (st != vmregion::Status::Ok) return handleabi::vm_error(st);
    o->retain();

    u64 flags = vmregion::pte_flags(static_cast<u32>(prot));
    u64 mapped = 0;
    for (; mapped < pages; ++mapped) {
        if (!constellation::space_map(p->space, base + mapped * PAGE, memobj::frame(o, static_cast<u32>(offset + mapped)), PAGE, flags)) break;
    }
    if (mapped != pages) {
        if (mapped) constellation::space_unmap(p->space, base, mapped * PAGE);
        StreamOps ops;
        p->regions.unmap(base, static_cast<u32>(pages), ops);
        return userabi::ERR_LIMIT;
    }
    return static_cast<i64>(base);
}

i64 sys_vm_unmap(idt::Frame* f) {
    Process* p = current_process();
    if (f->rsi == 0 || f->rsi > vmregion::MAX_MAPPED_PAGES) return userabi::ERR_INVAL;
    u64 base = f->rdi;
    u32 pages = static_cast<u32>(f->rsi);
    DeferredOps ops;
    VmGuard vg(p->vm);
    vmregion::Status st = p->regions.unmap(base, pages, ops);
    if (st != vmregion::Status::Ok) return handleabi::vm_error(st);
    if (!constellation::space_unmap(p->space, base, static_cast<u64>(pages) * PAGE)) {
        serial::printf("[userland] pid %u: page tables disagree with the region table, memory kept alive\n", p->pid);
        return userabi::ERR_FAULT;
    }
    ops.flush();
    return 0;
}

i64 sys_vm_protect(idt::Frame* f) {
    Process* p = current_process();
    if (!vm_args_ok(f->rsi, f->rdx)) return userabi::ERR_INVAL;
    u64 base = f->rdi;
    u32 pages = static_cast<u32>(f->rsi);
    u32 prot = static_cast<u32>(f->rdx);
    StreamOps ops;
    VmGuard vg(p->vm);
    vmregion::Status st = p->regions.protect(base, pages, prot, ops);
    if (st != vmregion::Status::Ok) return handleabi::vm_error(st);
    if (!constellation::space_protect(p->space, base, static_cast<u64>(pages) * PAGE, vmregion::pte_flags(prot))) {
        serial::printf("[userland] pid %u: page tables disagree with the region table after protect\n", p->pid);
        return userabi::ERR_FAULT;
    }
    return 0;
}


i64 fetch_path(Process* p, u64 uva, u64 len, char (&dst)[fileabi::MAX_PATH + 1]) {
    if (!fileabi::path_len_ok(len)) return userabi::ERR_INVAL;
    if (!user_read(p, uva, dst, len)) return userabi::ERR_FAULT;
    dst[len] = 0;
    if (!fileabi::path_text_ok(dst, len)) return userabi::ERR_INVAL;
    return 0;
}

i64 sys_file_open(idt::Frame* f) {
    Process* p = current_process();
    if (!fileabi::rights_ok(f->r10)) return userabi::ERR_INVAL;
    char path[fileabi::MAX_PATH + 1];
    i64 e = fetch_path(p, f->rsi, f->rdx, path);
    if (e) return e;
    u64 raw = 0;
    if (!user_write(p, f->r8, &raw, sizeof(raw))) return userabi::ERR_FAULT;
    TableGuard guard(p->handles.lock);
    lens::Result<lens::Lens> r = lens::open_at(p->handles.domain, lens::Lens(f->rdi), path, lens::Rights::from_bits(static_cast<u32>(f->r10)));
    if (!r.ok()) return fileabi::error_of(r.status());
    raw = r.value().raw;
    if (!user_write(p, f->r8, &raw, sizeof(raw))) {
        (void)lens::close(p->handles.domain, r.value());
        return userabi::ERR_FAULT;
    }
    return 0;
}

i64 sys_file_read(idt::Frame* f) {
    Process* p = current_process();
    u64 n = fileabi::clamp_io(f->r10);
    if (n == 0) return 0;
    u8 bounce[fileabi::MAX_IO] = {};
    if (!user_write(p, f->rdx, bounce, n)) return userabi::ERR_FAULT;
    usize got = 0;
    {
        TableGuard guard(p->handles.lock);
        lens::Result<usize> r = lens::read(p->handles.domain, lens::Lens(f->rdi), f->rsi, lens::Bytes(bounce, n));
        if (!r.ok()) return fileabi::error_of(r.status());
        got = r.value();
    }
    if (got > n) return userabi::ERR_INVAL;
    if (got && !user_write(p, f->rdx, bounce, got)) return userabi::ERR_FAULT;
    return static_cast<i64>(got);
}

i64 sys_file_write(idt::Frame* f) {
    Process* p = current_process();
    u64 n = fileabi::clamp_io(f->r10);
    if (n == 0) return 0;
    u8 bounce[fileabi::MAX_IO];
    if (!user_read(p, f->rdx, bounce, n)) return userabi::ERR_FAULT;
    TableGuard guard(p->handles.lock);
    lens::Result<usize> r = lens::write(p->handles.domain, lens::Lens(f->rdi), f->rsi, lens::ConstBytes(bounce, n));
    if (!r.ok()) return fileabi::error_of(r.status());
    if (r.value() > n) return userabi::ERR_INVAL;
    return static_cast<i64>(r.value());
}

i64 sys_file_stat(idt::Frame* f) {
    Process* p = current_process();
    fileabi::Stat st{};
    if (!user_write(p, f->rsi, &st, sizeof(st))) return userabi::ERR_FAULT;
    {
        TableGuard guard(p->handles.lock);
        lens::Lens l(f->rdi);
        lens::Result<lens::NodeKind> k = lens::kind_of(p->handles.domain, l);
        if (!k.ok()) return fileabi::error_of(k.status());
        if (k.value() == lens::NodeKind::Stream) {
            st.kind = fileabi::KIND_STREAM;
        } else {
            lens::Result<nebula::Info> info = lens::stat(p->handles.domain, l);
            if (!info.ok()) return fileabi::error_of(info.status());
            st.kind = info.value().kind == nebula::Kind::Dir ? fileabi::KIND_DIRECTORY : fileabi::KIND_FILE;
            st.size = info.value().size;
            if (info.value().kind == nebula::Kind::File) {
                lens::Result<u64> live = lens::size(p->handles.domain, l);
                if (live.ok()) st.size = live.value();
            }
            st.mtime_ms = info.value().mtime_ms;
            st.links = info.value().links;
            st.flags = fileabi::stat_flags(info.value().read_only, info.value().dynamic);
        }
    }
    if (!user_write(p, f->rsi, &st, sizeof(st))) return userabi::ERR_FAULT;
    return 0;
}

i64 sys_file_create(idt::Frame* f) {
    Process* p = current_process();
    u64 flags = f->r8;
    if ((flags & ~static_cast<u64>(fileabi::CREATE_FLAGS_MASK)) != 0 || !fileabi::rights_ok(f->r10)) return userabi::ERR_INVAL;
    char path[fileabi::MAX_PATH + 1];
    i64 e = fetch_path(p, f->rsi, f->rdx, path);
    if (e) return e;
    u64 raw = 0;
    bool directory = (flags & fileabi::CREATE_DIRECTORY) != 0;
    if (!directory && !user_write(p, f->r9, &raw, sizeof(raw))) return userabi::ERR_FAULT;
    TableGuard guard(p->handles.lock);
    if (directory) {
        lens::Done d = lens::mkdir(p->handles.domain, lens::Lens(f->rdi), path, false);
        return d.ok() ? 0 : fileabi::error_of(d.status());
    }
    lens::Result<lens::Lens> r = lens::create(p->handles.domain, lens::Lens(f->rdi), path, lens::Rights::from_bits(static_cast<u32>(f->r10)));
    if (!r.ok()) return fileabi::error_of(r.status());
    raw = r.value().raw;
    if (!user_write(p, f->r9, &raw, sizeof(raw))) {
        (void)lens::close(p->handles.domain, r.value());
        return userabi::ERR_FAULT;
    }
    return 0;
}

struct DirPick {
    u64 index;
    u64 seen;
    bool found;
    fileabi::DirEntry entry;
};

bool dir_pick(void* ctx, const nebula::Entry& e) {
    DirPick* d = static_cast<DirPick*>(ctx);
    if (d->seen != d->index) {
        ++d->seen;
        return true;
    }
    d->found = true;
    d->entry.size = e.size;
    d->entry.mtime_ms = e.mtime_ms;
    d->entry.kind = e.kind == nebula::Kind::Dir ? fileabi::KIND_DIRECTORY : fileabi::KIND_FILE;
    u32 n = 0;
    while (e.name[n] && n + 1 < fileabi::NAME_CAP) {
        d->entry.name[n] = e.name[n];
        ++n;
    }
    d->entry.name[n] = 0;
    d->entry.name_len = n;
    return false;
}

static_assert(nebula::NAME_BYTES + 1 <= fileabi::NAME_CAP, "a directory entry name must fit the user record");

i64 sys_dir_entry(idt::Frame* f) {
    Process* p = current_process();
    DirPick pick{};
    pick.index = f->rsi;
    if (!user_write(p, f->rdx, &pick.entry, sizeof(pick.entry))) return userabi::ERR_FAULT;
    {
        TableGuard guard(p->handles.lock);
        lens::Result<lens::EnumSummary> r = lens::enumerate(p->handles.domain, lens::Lens(f->rdi), &dir_pick, &pick, true);
        if (!r.ok()) return fileabi::error_of(r.status());
    }
    if (!pick.found) return fileabi::DIR_END;
    if (!user_write(p, f->rdx, &pick.entry, sizeof(pick.entry))) return userabi::ERR_FAULT;
    return 0;
}

i64 sys_pipe_create(idt::Frame* f) {
    Process* p = current_process();
    if (f->rdi == 0 || f->rdi > fileabi::MAX_PIPE_BYTES) return userabi::ERR_INVAL;
    u64 pair[2] = { 0, 0 };
    if (!user_write(p, f->rsi, pair, sizeof(pair))) return userabi::ERR_FAULT;
    TableGuard guard(p->handles.lock);
    lens::Lens r, w;
    lens::Done d = lens::make_pipe(p->handles.domain, static_cast<usize>(f->rdi), &r, &w);
    if (!d.ok()) return fileabi::error_of(d.status());
    pair[0] = r.raw;
    pair[1] = w.raw;
    if (!user_write(p, f->rsi, pair, sizeof(pair))) {
        (void)lens::close(p->handles.domain, r);
        (void)lens::close(p->handles.domain, w);
        return userabi::ERR_FAULT;
    }
    return 0;
}

struct Loaded {
    u8* buf = nullptr;
    apx::Header header{};
    char label[orbital::NAME_BYTES] = {};
    Loaded() = default;
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    ~Loaded() { if (buf) kheap::free(buf); }
};

lens::Status read_exact(lens::Table& t, lens::Lens l, u8* dst, u64 n) {
    u64 got = 0;
    while (got < n) {
        lens::Result<usize> r = lens::read(t.domain, l, got, lens::Bytes(dst + got, static_cast<usize>(n - got)));
        if (!r.ok()) return r.status();
        if (r.value() == 0) return lens::Status::Io;
        got += r.value();
    }
    return lens::Status::Ok;
}

i64 load_apx(lens::Table& t, lens::Lens file, Loaded* out) {
    TableGuard guard(t.lock);
    lens::Result<lens::Slot*> sr = lens::slot_of(t.domain, file);
    if (!sr.ok()) return fileabi::error_of(sr.status());
    lens::Slot* s = sr.value();
    if (s->kind != lens::NodeKind::Blob) return fileabi::ERR_WRONGTYPE;
    if (!s->rights.has_all(fileabi::LAUNCH_RIGHTS)) return handleabi::ERR_DENIED;
    lens::Result<u64> sz = lens::size(t.domain, file);
    if (!sz.ok()) return fileabi::error_of(sz.status());
    u64 size = sz.value();
    if (size < apx::HEADER_BYTES || size > apx::MAX_FILE_BYTES) return handleabi::ERR_BADIMAGE;
    u8 head[apx::HEADER_BYTES];
    lens::Status st = read_exact(t, file, head, sizeof(head));
    if (st != lens::Status::Ok) return fileabi::error_of(st);
    if (apx::parse_header(head, sizeof(head), size).status != apx::Status::Ok) return handleabi::ERR_BADIMAGE;
    out->buf = static_cast<u8*>(kheap::alloc(static_cast<usize>(size)));
    if (!out->buf) return userabi::ERR_LIMIT;
    st = read_exact(t, file, out->buf, size);
    if (st != lens::Status::Ok) return fileabi::error_of(st);
    apx::Parsed full = apx::parse(out->buf, static_cast<usize>(size));
    if (full.status != apx::Status::Ok) return handleabi::ERR_BADIMAGE;
    out->header = full.header;
    lens::Result<u32> label = lens::label_of(t.domain, file, out->label, sizeof(out->label));
    if (!label.ok() || out->label[0] == 0) {
        const char fallback[] = "apx";
        for (u32 i = 0; i < sizeof(fallback); ++i) out->label[i] = fallback[i];
    }
    return 0;
}

bool arg_handle_ok(lens::Table& t, lens::Lens arg, i64* error) {
    TableGuard guard(t.lock);
    lens::Result<lens::Rights> r = lens::rights_of(t.domain, arg);
    if (!r.ok()) {
        *error = handleabi::error_of(r.status());
        return false;
    }
    if (!r.value().has(lens::Right::Delegate)) {
        *error = handleabi::ERR_DENIED;
        return false;
    }
    return true;
}

Process* spawn_file(lens::Table& files, lens::Lens file, const char* name, lens::Table* from, lens::Lens arg, lens::Lens arg2, i64* error) {
    i64 scratch = 0;
    if (!error) error = &scratch;
    *error = 0;
    if (!g_supported) {
        *error = userabi::ERR_NOSYS;
        return nullptr;
    }
    if (from && !arg.is_null() && !arg_handle_ok(*from, arg, error)) return nullptr;
    if (from && !arg2.is_null() && (arg.is_null() || !arg_handle_ok(*from, arg2, error))) {
        if (*error == 0) *error = userabi::ERR_INVAL;
        return nullptr;
    }
    Loaded loaded;
    i64 e = load_apx(files, file, &loaded);
    if (e) {
        *error = e;
        return nullptr;
    }
    const apx::Header& h = loaded.header;
    Image img{ loaded.buf + h.code_offset, h.code_bytes, h.entry_offset, h.data_pages, h.data_bytes ? loaded.buf + h.data_offset : nullptr, h.data_bytes };
    bool with_arg = from && !arg.is_null();
    Process* p = launch_impl(name ? name : loaded.label, img, with_arg ? from : nullptr, arg, arg2);
    if (!p) *error = userabi::ERR_LIMIT;
    return p;
}

i64 sys_process_create(idt::Frame* f) {
    Process* self = current_process();
    if (f->r10 != 0) return userabi::ERR_INVAL;
    u64 raw = 0;
    if (!user_write(self, f->rdx, &raw, sizeof(raw))) return userabi::ERR_FAULT;
    (void)sweep_detached();
    ProcessObj* obj = static_cast<ProcessObj*>(kheap::alloc_zeroed(sizeof(ProcessObj)));
    if (!obj) return userabi::ERR_LIMIT;
    new (obj) ProcessObj();
    i64 error = 0;
    Process* child = spawn_file(self->handles, lens::Lens(f->rdi), nullptr, &self->handles, lens::Lens(f->rsi), lens::Lens(), &error);
    if (!child) {
        kheap::free(obj);
        return error;
    }
    obj->proc = child;
    obj->refs = 0;
    TableGuard guard(self->handles.lock);
    lens::Result<lens::Lens> made = lens::alloc_slot(self->handles.domain, nullptr, nebula::Node{0, 0}, nebula::Node{0, 0},
                                                     fileabi::PROCESS_RIGHTS, false, 0, "process", obj);
    if (!made.ok()) {
        (void)kill(child);
        __atomic_store_n(&child->detached, 1u, __ATOMIC_RELEASE);
        kheap::free(obj);
        return handleabi::error_of(made.status());
    }
    raw = made.value().raw;
    if (!user_write(self, f->rdx, &raw, sizeof(raw))) {
        (void)lens::close(self->handles.domain, made.value());
        return userabi::ERR_FAULT;
    }
    return 0;
}

i64 sys_process_status(idt::Frame* f) {
    Process* p = current_process();
    i64 status = 0;
    if (!user_write(p, f->rsi, &status, sizeof(status))) return userabi::ERR_FAULT;
    {
        TableGuard guard(p->handles.lock);
        lens::Result<lens::Slot*> sr = lens::slot_of(p->handles.domain, lens::Lens(f->rdi));
        if (!sr.ok()) return handleabi::error_of(sr.status());
        ProcessObj* o = process_of(sr.value());
        if (!o) return userabi::ERR_INVAL;
        if (!sr.value()->rights.has(lens::Right::Read)) return handleabi::ERR_DENIED;
        if (!__atomic_load_n(&o->proc->exited, __ATOMIC_ACQUIRE)) return handleabi::ERR_AGAIN;
        status = o->proc->status;
    }
    if (!user_write(p, f->rsi, &status, sizeof(status))) return userabi::ERR_FAULT;
    return 0;
}

i64 sys_process_kill(idt::Frame* f) {
    Process* p = current_process();
    TableGuard guard(p->handles.lock);
    lens::Result<lens::Slot*> sr = lens::slot_of(p->handles.domain, lens::Lens(f->rdi));
    if (!sr.ok()) return handleabi::error_of(sr.status());
    ProcessObj* o = process_of(sr.value());
    if (!o) return userabi::ERR_INVAL;
    if (!sr.value()->rights.has(lens::Right::Write)) return handleabi::ERR_DENIED;
    return kill(o->proc) ? 0 : handleabi::ERR_CLOSED;
}

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
};

static_assert(vmregion::PTE_WRITABLE == constellation::WRITABLE && vmregion::PTE_NO_EXECUTE == constellation::NO_EXECUTE,
              "the region table's page flags must match the VMM");

static_assert(userabi::USER_LIMIT == constellation::USER_LIMIT, "the native ABI and the VMM must agree on the user limit");

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
    die_if_killed();
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
        g_phys_mask = compute_phys_mask();
        idt::set_user_fault_handler(&on_user_fault);
        idt::set_user_abort_handler(&on_user_abort);
        idt::set_kernel_nmi_handler(&on_kernel_nmi);
        idt::set_user_return_handler(&on_user_return);
    }
    cpu::wrmsr(MSR_EFER, cpu::rdmsr(MSR_EFER) | EFER_SCE);
    cpu::wrmsr(MSR_STAR, STAR_VALUE);
    cpu::wrmsr(MSR_LSTAR, reinterpret_cast<u64>(&syscall_entry));
    cpu::wrmsr(MSR_FMASK, userabi::SYSCALL_FMASK);
    cpu::wrmsr(MSR_KERNEL_GS_BASE, 0);
    cpu::wrmsr(MSR_GS_BASE, reinterpret_cast<u64>(local));
}

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

Process* launch_file(const char* name, lens::Table& files, lens::Lens file, lens::Table* from, lens::Lens handle, i64* error, lens::Lens handle2) {
    return spawn_file(files, file, name, from, handle, handle2, error);
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
u32 threads_created(const Process* p) { return __atomic_load_n(&p->created, __ATOMIC_ACQUIRE); }

u32 cores_used(const Process* p) {
    u64 m = p->core_mask;
    u32 n = 0;
    while (m) { n += static_cast<u32>(m & 1); m >>= 1; }
    return n;
}

u64 frames_live() { return __atomic_load_n(&g_frames_live, __ATOMIC_RELAXED); }
u64 detached_reaped() { return __atomic_load_n(&g_detached_reaped, __ATOMIC_RELAXED); }
u64 frames_quarantined() { return __atomic_load_n(&g_frames_quarantined, __ATOMIC_RELAXED); }

void expect_nmis(u32 count) { __atomic_store_n(&g_nmi_expected, count, __ATOMIC_RELEASE); }
u64 nmis_from_user() { return __atomic_load_n(&g_nmi_from_user, __ATOMIC_RELAXED); }
u64 nmis_absorbed() { return __atomic_load_n(&g_nmi_absorbed, __ATOMIC_RELAXED); }

u32 live_processes() {
    u32 n = 0;
    for (auto& p : g_procs) if (__atomic_load_n(&p.in_use, __ATOMIC_ACQUIRE)) ++n;
    return n;
}

}
