#pragma once
#include "win32x.hpp"

extern "C" i32 pehost_ctx_save(u64* context) __attribute__((returns_twice));
extern "C" [[noreturn]] void pehost_ctx_jump(u64* context);

asm(R"(
.text
.globl pehost_ctx_save
.type pehost_ctx_save,@function
pehost_ctx_save:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    retq
.size pehost_ctx_save, .-pehost_ctx_save
.globl pehost_ctx_jump
.type pehost_ctx_jump,@function
pehost_ctx_jump:
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movl $1, %eax
    jmpq *56(%rdi)
.size pehost_ctx_jump, .-pehost_ctx_jump
)");

#if defined(__SANITIZE_ADDRESS__)
extern "C" void __asan_handle_no_return();
#endif

namespace pehost {

constexpr u32 STILL_ACTIVE = 259;
constexpr u32 CREATE_SUSPENDED = 4;
constexpr u32 ERROR_NOT_SUPPORTED_THREADS = ERROR_NOT_SUPPORTED;

namespace threads {

inline u64 pages_of(u64 bytes) { return (bytes + PAGE - 1) / PAGE * PAGE; }

inline u64 main_tls_bytes() {
    const Prepared* run = g_world.prepared;
    if (!run || !run->tls.present) return 0;
    return static_cast<u64>(run->tls.template_bytes) + run->tls.zero_fill;
}

inline void zero_bytes(u8* at, u64 bytes) {
    for (u64 i = 0; i < bytes; ++i) at[i] = 0;
}

inline void copy_bytes(u8* to, const u8* from, u64 bytes) {
    for (u64 i = 0; i < bytes; ++i) to[i] = from[i];
}

inline bool tls_add(u32 index, const u8* source, u32 template_bytes, u64 total_bytes) {
    u32 done[sync::MAX_SLOTS];
    u32 done_count = 0;
    for (u32 i = 1; i < sync::MAX_SLOTS; ++i) {
        sync::Slot& s = sync::g_sync.slots[i];
        if (!s.used) continue;
        u64 bytes = pages_of(total_bytes ? total_bytes : 1);
        u8* block = g_runtime->reserve(bytes);
        if (!block) {
            for (u32 k = 0; k < done_count; ++k) {
                sync::Slot& d = sync::g_sync.slots[done[k]];
                g_runtime->unmap(d.module_tls[index].block, d.module_tls[index].bytes);
                d.module_tls[index] = sync::ModuleBlock{};
                d.tls_array[index] = 0;
            }
            return false;
        }
        zero_bytes(block, bytes);
        copy_bytes(block, source, template_bytes);
        s.module_tls[index] = sync::ModuleBlock{ block, bytes };
        s.tls_array[index] = reinterpret_cast<u64>(block);
        done[done_count++] = i;
    }
    return true;
}

inline void tls_drop(u32 index) {
    for (u32 i = 1; i < sync::MAX_SLOTS; ++i) {
        sync::Slot& s = sync::g_sync.slots[i];
        if (!s.used || !s.module_tls[index].block) continue;
        s.tls_array[index] = 0;
        g_runtime->unmap(s.module_tls[index].block, s.module_tls[index].bytes);
        s.module_tls[index] = sync::ModuleBlock{};
    }
}

inline void exe_callbacks(u32 reason) {
    const Prepared* run = g_world.prepared;
    if (!run || !run->tls.present) return;
    modules::DirectCall call;
    u64 module = reinterpret_cast<u64>(run->base);
    for (u32 i = 0; i < run->tls.callback_count; ++i) call(run->tls.callbacks[i], module, reason, 0);
}

inline void notify(u32 reason) {
    sync::Loader guard;
    modules::DirectCall call;
    bool attaching = reason == modules::REASON_THREAD_ATTACH;
    if (attaching) exe_callbacks(reason);
    modules::notify_threads(reason, call);
    if (!attaching) exe_callbacks(reason);
}

inline bool init_worker(u32 index) {
    sync::Loader guard;
    sync::Slot& s = sync::g_sync.slots[index];
    const Prepared* run = g_world.prepared;
    u64 support_bytes = SUPPORT_HEAD + pages_of(main_tls_bytes());
    u64 stack_bytes = run->stack_bytes;
    u8* support = g_runtime->reserve(support_bytes);
    if (!support) return false;
    u8* stack = g_runtime->reserve(stack_bytes);
    if (!stack) {
        g_runtime->unmap(support, support_bytes);
        return false;
    }
    s.support = support;
    s.support_bytes = support_bytes;
    s.stack = stack;
    s.stack_bytes = stack_bytes;
    s.teb = reinterpret_cast<Teb*>(support);
    s.tls_array = reinterpret_cast<u64*>(support + 3 * PAGE);
    s.fls = s.tls_array + TLS_SLOTS;
    s.teb->self = reinterpret_cast<u64>(s.teb);
    s.teb->host_slot = index;
    s.teb->fls_data = reinterpret_cast<u64>(s.fls);
    s.teb->tls_pointer = reinterpret_cast<u64>(s.tls_array);
    bool ok = true;
    for (u32 k = 0; k < modules::MAX_MODULES && ok; ++k) {
        const modules::Module& m = modules::g_state.items[k];
        if (!m.used || !m.tls_block || m.binding) continue;
        u64 bytes = pages_of(m.tls_bytes ? m.tls_bytes : 1);
        u8* block = g_runtime->reserve(bytes);
        if (!block) {
            ok = false;
            break;
        }
        zero_bytes(block, bytes);
        copy_bytes(block, m.base + m.tls.template_rva, m.tls.template_bytes);
        s.module_tls[m.tls_index] = sync::ModuleBlock{ block, bytes };
    }
    if (!ok) {
        for (u32 k = 0; k < sync::MODULE_TLS; ++k) {
            if (s.module_tls[k].block) g_runtime->unmap(s.module_tls[k].block, s.module_tls[k].bytes);
            s.module_tls[k] = sync::ModuleBlock{};
        }
        g_runtime->unmap(stack, stack_bytes);
        g_runtime->unmap(support, support_bytes);
        s.support = s.stack = nullptr;
        s.teb = nullptr;
        s.tls_array = s.fls = nullptr;
        return false;
    }
    return true;
}

inline void reinit_for_job(sync::Slot& s, const sync::Object& job) {
    sync::Loader guard;
    const Prepared* run = g_world.prepared;
    const Teb* main_teb = sync::g_sync.slots[0].teb;
    u64 process = main_teb ? main_teb->client_process : 1;
    u64 peb = main_teb ? main_teb->peb : reinterpret_cast<u64>(run->peb);
    Teb* t = s.teb;
    zero_bytes(reinterpret_cast<u8*>(t), sizeof(Teb));
    t->stack_base = reinterpret_cast<u64>(s.stack + s.stack_bytes);
    t->stack_limit = reinterpret_cast<u64>(s.stack);
    t->self = reinterpret_cast<u64>(t);
    t->client_process = process;
    t->client_thread = job.id;
    t->tls_pointer = reinterpret_cast<u64>(s.tls_array);
    t->peb = peb;
    t->host_slot = static_cast<u64>(&s - sync::g_sync.slots);
    t->fls_data = reinterpret_cast<u64>(s.fls);
    for (u32 i = 0; i < TLS_SLOTS; ++i) s.tls_array[i] = 0;
    for (u32 i = 0; i < MAX_FLS; ++i) s.fls[i] = 0;
    if (run->tls.present) {
        u8* block = s.support + SUPPORT_HEAD;
        u64 bytes = main_tls_bytes();
        zero_bytes(block, bytes);
        copy_bytes(block, run->base + run->tls.template_rva, run->tls.template_bytes);
        s.tls_array[0] = reinterpret_cast<u64>(block);
    }
    for (u32 k = 0; k < modules::MAX_MODULES; ++k) {
        const modules::Module& m = modules::g_state.items[k];
        if (!m.used || !m.tls_block || m.binding) continue;
        sync::ModuleBlock& b = s.module_tls[m.tls_index];
        if (!b.block) continue;
        zero_bytes(b.block, b.bytes);
        copy_bytes(b.block, m.base + m.tls.template_rva, m.tls.template_bytes);
        s.tls_array[m.tls_index] = reinterpret_cast<u64>(b.block);
    }
}

inline void finish_process(u32 code) {
    win32::end_process(code);
    for (;;) asm volatile("ud2");
}

inline void leave_thread(sync::Slot& s, u32 code) {
    notify(modules::REASON_THREAD_DETACH);
    win32::fls_thread_exit(win32::syncd::fls_values_of(detail::current_teb()));
    u64 handle_to_close = 0;
    u32 remaining = 0;
    bool is_main = &s == &sync::g_sync.slots[0];
    {
        sync::Host guard;
        objects::abandon_mutexes(detail::current_thread_id());
        if (s.object != 0) {
            sync::Object& o = objects::table().items[s.object - 1];
            o.exit_code = code;
            o.finished = true;
            o.slot = -1;
            if (o.auto_close) handle_to_close = o.owner;
            i32 index = s.object - 1;
            s.object = 0;
            objects::unref(index);
            objects::changed();
        }
        sync::store(&s.job, 0);
        if (sync::g_sync.live) --sync::g_sync.live;
        remaining = sync::g_sync.live;
    }
    if (handle_to_close) objects::close(handle_to_close);
    if (remaining == 0) finish_process(code);
    if (is_main) __atomic_store_n(&sync::g_sync.main_gone, true, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s.running, false, __ATOMIC_SEQ_CST);
    sync::confirm_or_stop_leaving(s);
}

inline void run_job(sync::Slot& s) {
    sync::Object* o = &objects::table().items[s.object - 1];
    reinit_for_job(s, *o);
    if (o->suspended) {
        while (sync::load(&o->gate) == 0) sync::block(&o->gate, 0, sync::FOREVER);
    }
    notify(modules::REASON_THREAD_ATTACH);
    u32 code = reinterpret_cast<u32(PEHOST_WINAPI*)(void*)>(o->start)(o->parameter);
    objects::table().items[s.object - 1].exit_code = code;
}

inline void worker_main(u64 index) {
    sync::Slot& s = sync::g_sync.slots[index];
    g_runtime->set_teb(s.teb);
    for (;;) {
        while (sync::load(&s.job) == 0) g_runtime->futex_wait(&s.job, 0, sync::FOREVER);
        if (pehost_ctx_save(s.context) == 0) run_job(s);
        leave_thread(s, objects::table().items[s.object - 1].exit_code);
    }
}

inline void entry(u64 index) { worker_main(index); }

struct Claim {
    i32 index;
    bool fresh;
};

inline Claim claim_slot() {
    sync::Host guard;
    for (u32 i = 1; i < sync::MAX_SLOTS; ++i) {
        sync::Slot& s = sync::g_sync.slots[i];
        if (s.used && !s.running) {
            s.running = true;
            return Claim{ static_cast<i32>(i), false };
        }
    }
    for (u32 i = 1; i < sync::MAX_SLOTS; ++i) {
        sync::Slot& s = sync::g_sync.slots[i];
        if (!s.used && !s.running) {
            s.running = true;
            return Claim{ static_cast<i32>(i), true };
        }
    }
    return Claim{ -1, false };
}

inline bool slots_settling() {
    sync::Host guard;
    for (u32 i = 1; i < sync::MAX_SLOTS; ++i) {
        sync::Slot& s = sync::g_sync.slots[i];
        if (s.used && s.running && sync::load(&s.job) == 0) return true;
    }
    return false;
}

inline void unclaim(i32 index) {
    sync::Host guard;
    sync::g_sync.slots[index].running = false;
}

inline bool start_worker(i32 index) {
    sync::Slot& s = sync::g_sync.slots[index];
    if (!init_worker(static_cast<u32>(index))) return false;
    {
        sync::Host guard;
        s.used = true;
    }
    u8* top = s.stack + s.stack_bytes - 8;
    if (!g_runtime->spawn(&entry, static_cast<u64>(index), top)) {
        sync::Loader guard;
        for (u32 k = 0; k < sync::MODULE_TLS; ++k) {
            if (s.module_tls[k].block) g_runtime->unmap(s.module_tls[k].block, s.module_tls[k].bytes);
            s.module_tls[k] = sync::ModuleBlock{};
        }
        g_runtime->unmap(s.stack, s.stack_bytes);
        g_runtime->unmap(s.support, s.support_bytes);
        sync::Host host;
        s.used = false;
        s.support = s.stack = nullptr;
        s.teb = nullptr;
        s.tls_array = s.fls = nullptr;
        return false;
    }
    return true;
}

inline sync::Object* thread_of(u64 handle, i32* index) {
    if (handle == CURRENT_THREAD) {
        i32 i = objects::current_thread_object();
        if (i < 0) return nullptr;
        if (index) *index = i;
        return &objects::table().items[i];
    }
    sync::Object* o = objects::resolve(handle, index);
    return o && o->kind == sync::OBJ_THREAD ? o : nullptr;
}

}

namespace win32 {

PEHOST_WINAPI inline u64 CreateThread(void*, u64, void* start, void* parameter, u32 flags, u32* thread_id) {
    if (start == nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!sync::available() || !g_runtime->spawn || !g_runtime->set_teb || g_world.prepared == nullptr) {
        set_last_error(ERROR_NOT_SUPPORTED);
        return 0;
    }
    sync::enter_multi();
    threads::Claim claim = threads::claim_slot();
    for (u32 wait = 0; claim.index < 0 && wait < 2000 && threads::slots_settling(); ++wait) {
        g_runtime->sleep_ms(1);
        claim = threads::claim_slot();
    }
    if (claim.index < 0) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (claim.fresh && !threads::start_worker(claim.index)) {
        threads::unclaim(claim.index);
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    sync::Slot& s = sync::g_sync.slots[claim.index];
    sync::Host guard;
    i32 index = objects::allocate(sync::OBJ_THREAD, nullptr);
    if (index < 0) {
        s.running = false;
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    sync::Object& o = objects::table().items[index];
    o.refs = 1;
    u64 handle = objects::handle_for(index);
    if (handle == 0) {
        objects::unref(index);
        s.running = false;
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    o.id = ++sync::g_sync.next_id;
    o.start = start;
    o.parameter = parameter;
    o.suspended = (flags & CREATE_SUSPENDED) != 0;
    o.slot = claim.index;
    s.object = index + 1;
    ++sync::g_sync.live;
    sync::store(&s.job, 1);
    sync::wake(&s.job);
    if (thread_id) *thread_id = o.id;
    return handle;
}

[[noreturn]] PEHOST_WINAPI inline void ExitThread(u32 code) {
    sync::Slot& s = sync::current();
    if (&s == &sync::g_sync.slots[0]) {
        threads::leave_thread(s, code);
        for (;;) {
            if (!sync::available()) threads::finish_process(code);
            g_runtime->futex_wait(&s.private_word, 0, sync::FOREVER);
        }
    }
    if (s.object != 0) objects::table().items[s.object - 1].exit_code = code;
#if defined(__SANITIZE_ADDRESS__)
    __asan_handle_no_return();
#endif
    pehost_ctx_jump(s.context);
}

PEHOST_WINAPI inline i32 GetExitCodeThread(u64 handle, u32* code) {
    sync::Host guard;
    sync::Object* o = threads::thread_of(handle, nullptr);
    if (!o || code == nullptr) {
        set_last_error(o ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return 0;
    }
    *code = o->finished ? o->exit_code : STILL_ACTIVE;
    return 1;
}

PEHOST_WINAPI inline u32 ResumeThread(u64 handle) {
    sync::Host guard;
    sync::Object* o = threads::thread_of(handle, nullptr);
    if (!o) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0xFFFFFFFFu;
    }
    if (!o->suspended || sync::load(&o->gate) != 0) return 0;
    sync::store(&o->gate, 1);
    sync::wake(&o->gate);
    return 1;
}

PEHOST_WINAPI inline u32 SuspendThread(u64 handle) {
    sync::Host guard;
    sync::Object* o = threads::thread_of(handle, nullptr);
    set_last_error(o ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE);
    return 0xFFFFFFFFu;
}

PEHOST_WINAPI inline i32 TerminateThread(u64 handle, u32) {
    sync::Host guard;
    sync::Object* o = threads::thread_of(handle, nullptr);
    set_last_error(o ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE);
    return 0;
}

PEHOST_WINAPI inline i32 SwitchToThread() {
    if (g_runtime->sleep_ms) g_runtime->sleep_ms(0);
    return 1;
}

PEHOST_WINAPI inline u32 GetThreadId(u64 handle) {
    sync::Host guard;
    sync::Object* o = threads::thread_of(handle, nullptr);
    if (!o) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    return o->id;
}

PEHOST_WINAPI inline i32 SetThreadPriority(u64 handle, i32) {
    sync::Host guard;
    if (!threads::thread_of(handle, nullptr)) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    return 1;
}

PEHOST_WINAPI inline i32 GetThreadPriority(u64 handle) {
    sync::Host guard;
    if (!threads::thread_of(handle, nullptr)) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0x7FFFFFFF;
    }
    return 0;
}

PEHOST_WINAPI inline u64 SetThreadAffinityMask(u64 handle, u64 mask) {
    sync::Host guard;
    if (!threads::thread_of(handle, nullptr)) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (!(mask & 1)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

PEHOST_WINAPI inline u32 GetCurrentProcessorNumber() { return 0; }

PEHOST_WINAPI inline i32 SetThreadDescription(u64 handle, const u16*) {
    sync::Host guard;
    return threads::thread_of(handle, nullptr) ? 0 : static_cast<i32>(0x80070006u);
}

PEHOST_WINAPI inline i32 DisableThreadLibraryCalls(u64 module) {
    sync::Loader guard;
    if (!modules::disable_thread_calls(module)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

}

inline const ImportTable* kernel32t_table() {
#define PEHOST_T(name) { "kernel32.dll", #name, reinterpret_cast<void*>(&win32::name) }
    static const Import items[] = {
        PEHOST_T(CreateThread), PEHOST_T(ExitThread), PEHOST_T(GetExitCodeThread), PEHOST_T(ResumeThread), PEHOST_T(SuspendThread),
        PEHOST_T(TerminateThread), PEHOST_T(SwitchToThread), PEHOST_T(GetThreadId), PEHOST_T(SetThreadPriority), PEHOST_T(GetThreadPriority),
        PEHOST_T(SetThreadAffinityMask), PEHOST_T(GetCurrentProcessorNumber), PEHOST_T(SetThreadDescription), PEHOST_T(DisableThreadLibraryCalls),
    };
#undef PEHOST_T
    static const ImportTable table{ items, sizeof(items) / sizeof(items[0]), kernel32x_table() };
    return &table;
}

}
