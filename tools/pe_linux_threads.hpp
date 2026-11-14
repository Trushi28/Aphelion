#pragma once
#include <pthread.h>
#include <cosmos/types.hpp>

namespace linux_threads {

inline i64 raw(u64 nr, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0) {
    i64 result;
    register u64 r10 asm("r10") = d;
    asm volatile("syscall" : "=a"(result) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return result;
}

inline i32 futex_wait(volatile u32* word, u32 expected, u64 timeout_ms) {
    u64 spec[2];
    u64 spec_at = 0;
    if (timeout_ms != ~0ull) {
        spec[0] = timeout_ms / 1000;
        spec[1] = (timeout_ms % 1000) * 1000000;
        spec_at = reinterpret_cast<u64>(spec);
    }
    i64 r = raw(202, reinterpret_cast<u64>(word), 128, expected, spec_at);
    return r == -110 ? 1 : 0;
}

inline void futex_wake(volatile u32* word) { raw(202, reinterpret_cast<u64>(word), 129, 0x7FFFFFFF); }

inline bool set_teb(void* teb) { return raw(158, 0x1001, reinterpret_cast<u64>(teb)) == 0; }

struct Launch {
    void (*entry)(u64);
    u64 argument;
};

inline void* trampoline(void* raw_launch) {
    Launch launch = *static_cast<Launch*>(raw_launch);
    delete static_cast<Launch*>(raw_launch);
    launch.entry(launch.argument);
    return nullptr;
}

inline bool spawn(void (*entry)(u64), u64 argument, u8*) {
    Launch* launch = new Launch{ entry, argument };
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&thread, &attr, &trampoline, launch);
    pthread_attr_destroy(&attr);
    if (r != 0) delete launch;
    return r == 0;
}

}
