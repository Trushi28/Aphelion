#include "internal.hpp"
#include <cosmos/handleabi.hpp>
#include <cosmos/cpu.hpp>

namespace userland::impl {

namespace {

struct JoinWait {
    Process* p;
    Thread* t;
};

bool join_ready(void* arg) {
    JoinWait* w = static_cast<JoinWait*>(arg);
    return __atomic_load_n(&w->t->done, __ATOMIC_ACQUIRE) != 0 || procgroup::dying(procgroup::load(&w->p->group));
}

constexpr u64 JOIN_SETTLE_MS = 5000;

struct FutexWait {
    Process* p;
    u32 seen;
};

bool futex_ready(void* arg) {
    FutexWait* w = static_cast<FutexWait*>(arg);
    return __atomic_load_n(&w->p->futex_seq, __ATOMIC_ACQUIRE) != w->seen || group_dying(w->p);
}

}

i64 sys_thread_exit(idt::Frame* f) { leave_thread(current_process(), current_thread(), static_cast<i32>(f->rdi)); }

i64 sys_thread_create(idt::Frame* f) {
    Process* p = current_process();
    if (!procgroup::entry_ok(f->rdi, f->rsi, userabi::USER_LIMIT)) return userabi::ERR_INVAL;
    if (!procgroup::try_enter(&p->group, userabi::MAX_LIVE_THREADS)) return userabi::ERR_LIMIT;
    u32 index = p->roster.reserve();
    if (index >= userabi::MAX_THREADS_TOTAL) {
        procgroup::leave(&p->group, 0);
        return userabi::ERR_LIMIT;
    }
    Thread& t = p->threads[index];
    t = Thread{ p, 0, 0, 0, f->rdi, f->rsi, f->rdx, 0 };
    p->roster.publish(index);
    if (!orbital::spawn_in(p->name, &thread_main, &t, p->space, &t.task_id)) {
        __atomic_store_n(&t.done, 1u, __ATOMIC_RELEASE);
        procgroup::leave(&p->group, 0);
        return userabi::ERR_LIMIT;
    }
    return static_cast<i64>(__atomic_load_n(&t.task_id, __ATOMIC_ACQUIRE));
}

i64 sys_thread_join(idt::Frame* f) {
    Process* p = current_process();
    Thread* self = current_thread();
    u32 tid = static_cast<u32>(f->rdi);
    if (!tid || f->rdi != tid) return userabi::ERR_INVAL;
    u32 created = p->roster.count();
    Thread* target = nullptr;
    for (u32 i = 0; i < created; ++i) {
        if (!p->roster.visible(i)) continue;
        if (__atomic_load_n(&p->threads[i].task_id, __ATOMIC_ACQUIRE) == tid) { target = &p->threads[i]; break; }
    }
    if (!target || target == self) return userabi::ERR_INVAL;
    JoinWait w{ p, target };
    p->events.wait_until(&join_ready, &w);
    if (!__atomic_load_n(&target->done, __ATOMIC_ACQUIRE)) return userabi::ERR_INVAL;
    i32 settled = 0;
    (void)orbital::join(tid, &settled, JOIN_SETTLE_MS);
    return static_cast<i64>(target->status);
}

i64 sys_futex_wait(idt::Frame* f) {
    Process* p = current_process();
    u64 address = f->rdi;
    if (!userabi::futex_address_ok(address)) return userabi::ERR_INVAL;
    u32 expected = static_cast<u32>(f->rsi);
    if (f->rsi != expected) return userabi::ERR_INVAL;
    u32 seen = __atomic_load_n(&p->futex_seq, __ATOMIC_ACQUIRE);
    u32 current = 0;
    {
        VmGuard vg(p->vm);
        SpaceReader reader{ p->space, false };
        usercopy::Window w = reader.window(address);
        if (!w.host || w.room < sizeof(u32)) return userabi::ERR_FAULT;
        current = __atomic_load_n(reinterpret_cast<const u32*>(w.host), __ATOMIC_ACQUIRE);
    }
    if (current != expected) return handleabi::ERR_AGAIN;
    FutexWait w{ p, seen };
    if (!p->futex.wait_until_for(&futex_ready, &w, f->rdx)) return handleabi::ERR_TIMEDOUT;
    return 0;
}

i64 sys_futex_wake(idt::Frame* f) {
    Process* p = current_process();
    if (!userabi::futex_address_ok(f->rdi)) return userabi::ERR_INVAL;
    if (f->rsi == 0) return 0;
    __atomic_add_fetch(&p->futex_seq, 1u, __ATOMIC_ACQ_REL);
    (void)p->futex.wake_all();
    return 0;
}

}
