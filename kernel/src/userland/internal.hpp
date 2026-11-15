#pragma once
#include <cosmos/userland.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/usercopy.hpp>
#include <cosmos/ringabort.hpp>
#include <cosmos/procgroup.hpp>
#include <cosmos/slotpub.hpp>
#include <cosmos/lens_channel.hpp>
#include <cosmos/memobj.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/vmregion.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/vmm.hpp>

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
    slotpub::Roster<userabi::MAX_THREADS_TOTAL> roster;
    u64 group;
    lens::Table handles;
    orbital::Mutex vm;
    vmregion::Map<> regions;
    orbital::WaitQueue events;
    orbital::WaitQueue done;
    orbital::WaitQueue futex;
    volatile u32 futex_seq;
    volatile u32 exited;
    volatile u32 detached;
    volatile u32 reaping;
    i32 status;
    volatile u64 core_mask;
};

Process* launch_impl(const char* name, const Image& image, lens::Table* from, lens::Lens handle, lens::Lens handle2);

namespace impl {

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

inline ProcessObj* process_of(lens::Slot* s) {
    if (s->kind != lens::NodeKind::Stream || !s->stream || s->stream->type() != PROCESS_STREAM_TYPE) return nullptr;
    return static_cast<ProcessObj*>(s->stream);
}

struct TableGuard {
    orbital::Mutex& m;
    explicit TableGuard(orbital::Mutex& x) : m(x) { m.lock(); }
    ~TableGuard() { m.unlock(); }
    TableGuard(const TableGuard&) = delete;
    TableGuard& operator=(const TableGuard&) = delete;
};

struct MemRef {
    memobj::Object* object = nullptr;
    u32 max = 0;
    ~MemRef() { if (object) object->release(); }
    MemRef() = default;
    MemRef(const MemRef&) = delete;
    MemRef& operator=(const MemRef&) = delete;
};

Process* current_process();
Thread* current_thread();
NORETURN void leave_thread(Process* p, Thread* t, i32 own);
NORETURN void terminate(Process* p, i32 status);
void die_if_killed();
bool user_read(Process* p, u64 uva, void* dst, u64 n);
bool user_write(Process* p, u64 uva, const void* src, u64 n);
bool group_dying(void* arg);
void console_write(const char* text, u32 length);
void set_supported();
void thread_main(void* arg);
Process* spawn_file(lens::Table& files, lens::Lens file, const char* name, lens::Table* from, lens::Lens arg, lens::Lens arg2, i64* error);
void install_fault_handlers();

i64 sys_exit(idt::Frame* f);
i64 sys_yield(idt::Frame* f);
i64 sys_gettid(idt::Frame* f);
i64 sys_debug_write(idt::Frame* f);
i64 sys_set_gs_base(idt::Frame* f);
i64 sys_get_gs_base(idt::Frame* f);
i64 sys_set_fs_base(idt::Frame* f);
i64 sys_get_fs_base(idt::Frame* f);
i64 sys_thread_create(idt::Frame* f);
i64 sys_thread_exit(idt::Frame* f);
i64 sys_thread_join(idt::Frame* f);
i64 sys_handle_close(idt::Frame* f);
i64 sys_handle_info(idt::Frame* f);
i64 sys_handle_narrow(idt::Frame* f);
i64 sys_channel_create(idt::Frame* f);
i64 sys_channel_send(idt::Frame* f);
i64 sys_channel_recv(idt::Frame* f);
i64 sys_handle_wait(idt::Frame* f);
i64 sys_vmo_create(idt::Frame* f);
i64 sys_vm_map(idt::Frame* f);
i64 sys_vm_unmap(idt::Frame* f);
i64 sys_vm_protect(idt::Frame* f);
i64 sys_vmo_size(idt::Frame* f);
i64 sys_file_open(idt::Frame* f);
i64 sys_file_read(idt::Frame* f);
i64 sys_file_write(idt::Frame* f);
i64 sys_file_stat(idt::Frame* f);
i64 sys_file_create(idt::Frame* f);
i64 sys_dir_entry(idt::Frame* f);
i64 sys_pipe_create(idt::Frame* f);
i64 sys_process_create(idt::Frame* f);
i64 sys_process_status(idt::Frame* f);
i64 sys_process_kill(idt::Frame* f);
i64 sys_clock(idt::Frame* f);
i64 sys_futex_wait(idt::Frame* f);
i64 sys_futex_wake(idt::Frame* f);
i64 sys_file_unlink(idt::Frame* f);
i64 sys_file_rename(idt::Frame* f);
i64 sys_file_resize(idt::Frame* f);

}

}
