#include <cosmos/orbital.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/fpu.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/timesrc.hpp>
#include <cosmos/vmm.hpp>

namespace orbital {

namespace {

constexpr u32 FPU_WORKERS = 6;
constexpr u32 SPACE_WORKER = 1;
constexpr u32 RUN_MS = 700;
constexpr u32 PINGPONG_ROUNDS = 1500;
constexpr u32 WAITERS = 3;
constexpr u64 GIVE_UP_MS = 8000;
constexpr u32 MAX_LIST = 64;
constexpr u32 MAX_CORES_LISTED = 16;

struct Worker {
    u32 id;
    constellation::Space* space;
    volatile u32 done;
    u32 state_errors;
    u32 mode_errors;
    u32 stack_errors;
    u32 space_errors;
    u64 checks;
    u64 switches;
};

struct Gate {
    WaitQueue queue;
    volatile u32 open;
    volatile u32 woke;
};

struct PingPong {
    WaitQueue a;
    WaitQueue b;
    volatile u32 turn;
    volatile u32 count;
    volatile u32 finished;
};

struct TimedProbe {
    WaitQueue queue;
    volatile u32 flag;
    volatile u32 result;
    volatile u32 done;
    volatile u64 elapsed_ms;
};

struct SemRig {
    Semaphore sem;
    volatile u32 stop;
    volatile u32 got[2];
    volatile u32 finished;
};

struct LockRig {
    SleepLock lock;
    volatile u32 counter;
    volatile u32 finished;
    volatile u32 try_result;
    volatile u32 timed_result;
    volatile u64 timed_elapsed_ms;
    volatile u32 phase;
    volatile u32 late_result;
};

struct LatencyProbe {
    volatile u64 started_tsc;
    volatile u32 ran;
};

constexpr u32 SEM_TOTAL = 400;
constexpr u32 LOCK_WORKERS = 4;
constexpr u32 LOCK_ROUNDS = 200;
constexpr u32 LATENCY_SAMPLES = 21;
constexpr u64 LATENCY_LIMIT_US = 2000;

TimedProbe g_timed;
SemRig g_sem_rig;
LockRig g_lock_rig;

bool timed_flag_set(void* arg) { return static_cast<TimedProbe*>(arg)->flag != 0; }
bool never_ready(void*) { return false; }

Worker g_workers[FPU_WORKERS];
Gate g_gate;
PingPong g_pp;

bool gate_open(void* arg) { return static_cast<Gate*>(arg)->open != 0; }
bool turn_is(void* arg, u32 want) { return static_cast<PingPong*>(arg)->turn == want; }
bool turn_a(void* arg) { return turn_is(arg, 0); }
bool turn_b(void* arg) { return turn_is(arg, 1); }

void gate_waiter(void*) {
    g_gate.queue.wait_until(&gate_open, &g_gate);
    __atomic_add_fetch(&g_gate.woke, 1u, __ATOMIC_SEQ_CST);
}

void pingpong_a(void*) {
    for (u32 i = 0; i < PINGPONG_ROUNDS; ++i) {
        g_pp.a.wait_until(&turn_a, &g_pp);
        g_pp.turn = 1;
        g_pp.b.wake_one();
    }
    __atomic_add_fetch(&g_pp.finished, 1u, __ATOMIC_SEQ_CST);
}

void pingpong_b(void*) {
    for (u32 i = 0; i < PINGPONG_ROUNDS; ++i) {
        g_pp.b.wait_until(&turn_b, &g_pp);
        __atomic_add_fetch(&g_pp.count, 1u, __ATOMIC_SEQ_CST);
        g_pp.turn = 0;
        g_pp.a.wake_one();
    }
    __atomic_add_fetch(&g_pp.finished, 1u, __ATOMIC_SEQ_CST);
}

void fpu_worker(void* arg) {
    Worker* w = static_cast<Worker*>(arg);
    bool use_fpu = fpu::usable();
    alignas(32) u8 want[512];
    alignas(32) u8 got[512];
    fpu::Modes modes = fpu::default_modes();
    modes.control_word = static_cast<u16>((modes.control_word & ~(3u << 10)) | ((w->id % 4) << 10));
    modes.mxcsr = (modes.mxcsr & ~(3u << 13)) | (((w->id + 1) % 4) << 13);
    u32 bytes = use_fpu ? fpu::register_bytes() : 0;
    if (use_fpu) {
        for (u32 i = 0; i < bytes; ++i) want[i] = static_cast<u8>(w->id * 37 + i * 7 + 1);
        fpu::load_registers(want);
        fpu::set_modes(modes);
    }

    u64 end = clock::uptime_ms() + RUN_MS;
    u64 spin = 0;
    while (clock::uptime_ms() < end) {
        for (u32 k = 0; k < 3000; ++k) spin += k ^ w->id;
        if (w->id % 2 == 1 && (spin & 0x7) == 0) yield();

        ++w->checks;
        if (use_fpu) {
            fpu::store_registers(got);
            if (memcmp(want, got, bytes) != 0) ++w->state_errors;
            fpu::Modes now = fpu::get_modes();
            if (now.control_word != modes.control_word || (now.mxcsr & ~0x3Fu) != modes.mxcsr) ++w->mode_errors;
        }
        u64 f = cpu::irq_save();
        Satellite* me = current();
        if (gdt::kernel_stack() != me->kstack_top) ++w->stack_errors;
        u64 root = w->space ? constellation::space_root_phys(w->space) : constellation::core_pml4_phys();
        if (cpu::read_cr3() != root) ++w->space_errors;
        cpu::irq_restore(f);
    }
    w->switches = current()->switches;
    __atomic_store_n(&w->done, 1u, __ATOMIC_SEQ_CST);
}

void timed_waiter(void*) {
    u64 t0 = clock::uptime_ms();
    bool r = g_timed.queue.wait_until_for(&timed_flag_set, &g_timed, 3000);
    g_timed.elapsed_ms = clock::uptime_ms() - t0;
    g_timed.result = r ? 1u : 0u;
    __atomic_store_n(&g_timed.done, 1u, __ATOMIC_SEQ_CST);
}

void sem_consumer(void* arg) {
    u32 idx = static_cast<u32>(reinterpret_cast<u64>(arg));
    for (;;) {
        if (g_sem_rig.sem.acquire_for(1)) { g_sem_rig.got[idx] = g_sem_rig.got[idx] + 1; continue; }
        if (__atomic_load_n(&g_sem_rig.stop, __ATOMIC_SEQ_CST)) break;
    }
    __atomic_add_fetch(&g_sem_rig.finished, 1u, __ATOMIC_SEQ_CST);
}

void lock_worker(void*) {
    for (u32 i = 0; i < LOCK_ROUNDS; ++i) {
        g_lock_rig.lock.lock();
        u32 v = g_lock_rig.counter;
        if ((i & 7) == 0) yield();
        if ((i & 63) == 0) sleep_ms(1);
        g_lock_rig.counter = v + 1;
        g_lock_rig.lock.unlock();
    }
    __atomic_add_fetch(&g_lock_rig.finished, 1u, __ATOMIC_SEQ_CST);
}

void lock_contender(void*) {
    g_lock_rig.try_result = g_lock_rig.lock.try_lock() ? 1u : 0u;
    u64 t0 = clock::uptime_ms();
    g_lock_rig.timed_result = g_lock_rig.lock.lock_for(30) ? 1u : 0u;
    g_lock_rig.timed_elapsed_ms = clock::uptime_ms() - t0;
    __atomic_store_n(&g_lock_rig.phase, 1u, __ATOMIC_SEQ_CST);
    if (g_lock_rig.lock.lock_for(3000)) {
        g_lock_rig.late_result = 1;
        g_lock_rig.lock.unlock();
    }
    __atomic_store_n(&g_lock_rig.phase, 2u, __ATOMIC_SEQ_CST);
}

void exit_with_arg(void* arg) { exit_with(static_cast<i32>(reinterpret_cast<u64>(arg))); }

void sleepy_exit(void*) {
    sleep_ms(300);
    exit_with(7);
}

void quiet_return(void*) {}

void latency_entry(void* arg) {
    auto* p = static_cast<LatencyProbe*>(arg);
    p->started_tsc = cpu::rdtsc();
    __atomic_store_n(&p->ran, 1u, __ATOMIC_SEQ_CST);
}

bool wait_for(volatile u32* flag, u32 want, u64 limit_ms) {
    u64 end = clock::uptime_ms() + limit_ms;
    while (__atomic_load_n(flag, __ATOMIC_SEQ_CST) < want) {
        if (clock::uptime_ms() > end) return false;
        sleep_ms(5);
    }
    return true;
}

bool test_sleep() {
    u64 t0 = clock::uptime_ms();
    sleep_ms(50);
    u64 dt = clock::uptime_ms() - t0;
    serial::printf("[sched] sleep_ms(50) returned after %lu ms\n", dt);
    return dt >= 50 && dt < 1000;
}

bool test_gate() {
    for (u32 i = 0; i < WAITERS; ++i)
        if (!spawn("gate-waiter", &gate_waiter, nullptr)) return false;
    u64 end = clock::uptime_ms() + 2000;
    while (g_gate.queue.waiters() < WAITERS && clock::uptime_ms() < end) sleep_ms(5);
    bool ok = g_gate.queue.waiters() == WAITERS && g_gate.woke == 0;

    g_gate.open = 1;
    ok = ok && g_gate.queue.wake_one() == 1;
    ok = ok && wait_for(&g_gate.woke, 1, 2000);
    sleep_ms(30);
    ok = ok && g_gate.woke == 1 && g_gate.queue.waiters() == WAITERS - 1;
    ok = ok && g_gate.queue.wake_all() == WAITERS - 1;
    ok = ok && wait_for(&g_gate.woke, WAITERS, 2000);
    ok = ok && g_gate.queue.waiters() == 0 && g_gate.queue.wake_one() == 0;
    serial::printf("[sched] wait queue: %u waiters parked, woken one then all, %u woke\n", WAITERS, g_gate.woke);
    return ok;
}

bool test_pingpong() {
    if (!spawn("pingpong-a", &pingpong_a, nullptr) || !spawn("pingpong-b", &pingpong_b, nullptr)) return false;
    bool ok = wait_for(&g_pp.finished, 2, GIVE_UP_MS);
    ok = ok && g_pp.count == PINGPONG_ROUNDS;
    serial::printf("[sched] wait queue hand-off: %u of %u rounds completed without a lost wakeup\n", g_pp.count, PINGPONG_ROUNDS);
    return ok;
}

bool test_timed_wait() {
    g_timed = {};
    u64 t0 = clock::uptime_ms();
    bool woke = g_timed.queue.wait_until_for(&never_ready, nullptr, 40);
    u64 dt = clock::uptime_ms() - t0;
    bool ok = !woke && dt >= 40 && dt < 500 && g_timed.queue.waiters() == 0;
    serial::printf("[sched] timed wait of 40 ms expired after %lu ms\n", dt);

    ok = ok && spawn("timed-waiter", &timed_waiter, nullptr) != nullptr;
    u64 end = clock::uptime_ms() + 2000;
    while (g_timed.queue.waiters() < 1 && clock::uptime_ms() < end) sleep_ms(2);
    ok = ok && g_timed.queue.waiters() == 1;
    sleep_ms(30);
    g_timed.flag = 1;
    ok = ok && g_timed.queue.wake_one() == 1;
    ok = ok && wait_for(&g_timed.done, 1, 2000);
    ok = ok && g_timed.result == 1 && g_timed.elapsed_ms < 1000 && g_timed.queue.waiters() == 0;
    serial::printf("[sched] timed wait woken early after %lu ms of a 3000 ms budget\n", g_timed.elapsed_ms);
    serial::printf("[selftest] timed wait: %s\n", ok ? "ok" : "MISMATCH");
    return ok;
}

bool test_semaphore() {
    g_sem_rig = {};
    bool ok = !g_sem_rig.sem.try_acquire();
    u64 t0 = clock::uptime_ms();
    ok = ok && !g_sem_rig.sem.acquire_for(30);
    ok = ok && clock::uptime_ms() - t0 >= 30;
    g_sem_rig.sem.release(2);
    ok = ok && g_sem_rig.sem.value() == 2;
    ok = ok && g_sem_rig.sem.try_acquire() && g_sem_rig.sem.acquire_for(10) && !g_sem_rig.sem.try_acquire();

    for (u32 i = 0; i < 2; ++i)
        ok = ok && spawn("sem-consumer", &sem_consumer, reinterpret_cast<void*>(static_cast<u64>(i)));
    for (u32 i = 0; i < SEM_TOTAL; ++i) {
        g_sem_rig.sem.release();
        if ((i & 7) == 7) sleep_ms(1);
        else if ((i & 3) == 3) yield();
    }
    sleep_ms(20);
    __atomic_store_n(&g_sem_rig.stop, 1u, __ATOMIC_SEQ_CST);
    ok = ok && wait_for(&g_sem_rig.finished, 2, GIVE_UP_MS);
    u32 accounted = g_sem_rig.got[0] + g_sem_rig.got[1] + g_sem_rig.sem.value();
    ok = ok && accounted == SEM_TOTAL;
    serial::printf("[sched] semaphore: %u released, consumers took %u + %u, %u left, no permit lost or duplicated\n",
                   SEM_TOTAL, g_sem_rig.got[0], g_sem_rig.got[1], g_sem_rig.sem.value());
    serial::printf("[selftest] semaphore: %s\n", ok ? "ok" : "MISMATCH");
    return ok;
}

bool test_sleeplock() {
    g_lock_rig = {};
    bool ok = true;
    for (u32 i = 0; i < LOCK_WORKERS; ++i) ok = ok && spawn("lock-worker", &lock_worker, nullptr);
    ok = ok && wait_for(&g_lock_rig.finished, LOCK_WORKERS, GIVE_UP_MS);
    ok = ok && g_lock_rig.counter == LOCK_WORKERS * LOCK_ROUNDS;
    serial::printf("[sched] sleep lock: %u of %u guarded increments survived %u contending satellites\n",
                   g_lock_rig.counter, LOCK_WORKERS * LOCK_ROUNDS, LOCK_WORKERS);

    g_lock_rig.lock.lock();
    ok = ok && g_lock_rig.lock.held_by_me();
    ok = ok && spawn("lock-contender", &lock_contender, nullptr) != nullptr;
    ok = ok && wait_for(&g_lock_rig.phase, 1, 3000);
    ok = ok && g_lock_rig.try_result == 0 && g_lock_rig.timed_result == 0 && g_lock_rig.timed_elapsed_ms >= 30;
    g_lock_rig.lock.unlock();
    ok = ok && wait_for(&g_lock_rig.phase, 2, 4000);
    ok = ok && g_lock_rig.late_result == 1 && !g_lock_rig.lock.held_by_me() && g_lock_rig.lock.try_lock();
    if (g_lock_rig.lock.held_by_me()) g_lock_rig.lock.unlock();
    serial::printf("[sched] sleep lock: try_lock refused, lock_for(30) gave up after %lu ms, hand-over on unlock worked\n",
                   g_lock_rig.timed_elapsed_ms);
    serial::printf("[selftest] sleeplock: %s\n", ok ? "ok" : "MISMATCH");
    return ok;
}

bool test_join() {
    bool ok = true;
    i32 status = -1;

    Satellite* a = spawn("join-exit", &exit_with_arg, reinterpret_cast<void*>(42ull));
    ok = ok && a != nullptr;
    u32 aid = a ? a->id : 0;
    ok = ok && join(aid, &status, 2000) == JoinResult::Exited && status == 42;
    status = -1;
    ok = ok && join(aid, &status, 0) == JoinResult::Exited && status == 42;

    Satellite* b = spawn("join-return", &quiet_return, nullptr);
    ok = ok && b != nullptr;
    status = -1;
    ok = ok && join(b ? b->id : 0, &status, 2000) == JoinResult::Exited && status == 0;

    ok = ok && join(0, &status, 10) == JoinResult::Unknown;
    ok = ok && join(0x7FFFFFF0u, &status, 10) == JoinResult::Unknown;
    ok = ok && join(current_id(), &status, 10) == JoinResult::Self;

    Satellite* c = spawn("join-sleepy", &sleepy_exit, nullptr);
    ok = ok && c != nullptr;
    u32 cid = c ? c->id : 0;
    u64 t0 = clock::uptime_ms();
    ok = ok && join(cid, &status, 20) == JoinResult::TimedOut;
    ok = ok && clock::uptime_ms() - t0 >= 20;
    status = -1;
    ok = ok && join(cid, &status, 3000) == JoinResult::Exited && status == 7;

    serial::printf("[sched] join: exit status 42 and 7 delivered, natural return gives 0, unknown/self/timeout reported\n");
    serial::printf("[selftest] join: %s\n", ok ? "ok" : "MISMATCH");
    return ok;
}

bool test_introspection() {
    SatelliteInfo list[MAX_LIST];
    u32 n = snapshot(list, MAX_LIST);
    bool ok = n >= 1;
    bool found_self = false;
    for (u32 i = 0; i < n; ++i) {
        if (list[i].id == current_id()) {
            found_self = true;
            ok = ok && list[i].state == State::Running && list[i].ring >= 0 && list[i].ring < NUM_RINGS;
            ok = ok && list[i].name[0] == 's' && list[i].name[1] == 'c' && list[i].name[2] == 'h';
        }
    }
    ok = ok && found_self;

    CoreInfo cores_list[MAX_CORES_LISTED];
    u32 c = cores(cores_list, MAX_CORES_LISTED);
    ok = ok && c >= 1;
    for (u32 i = 0; i < c; ++i)
        for (u32 j = i + 1; j < c; ++j) ok = ok && cores_list[i].apic_id != cores_list[j].apic_id;
    for (u32 i = 0; i < c; ++i) ok = ok && cores_list[i].running[0] != 0;

    Stats st = stats();
    ok = ok && st.spawned >= st.exited && st.spawned > 0 && st.exited > 0;
    serial::printf("[sched] introspection: %u satellites, %u cores, spawned %lu, exited %lu\n", n, c, st.spawned, st.exited);
    serial::printf("[selftest] sched introspection: %s\n", ok ? "ok" : "MISMATCH");
    return ok;
}

bool test_wake_latency() {
    CoreInfo cores_list[MAX_CORES_LISTED];
    u32 c = cores(cores_list, MAX_CORES_LISTED);
    u64 khz = timesrc::tsc_khz();
    if (c < 2 || khz == 0) {
        serial::printf("[sched] wake latency: skipped (%u core(s), tsc %lu kHz)\n", c, khz);
        serial::printf("[selftest] wake latency: skipped\n");
        return true;
    }

    u64 samples[LATENCY_SAMPLES];
    u32 taken = 0;
    for (u32 i = 0; i < LATENCY_SAMPLES; ++i) {
        sleep_ms(2 * clock::TICK_MS);
        LatencyProbe probe = {};
        u64 t0 = cpu::rdtsc();
        if (!spawn("latency-probe", &latency_entry, &probe)) continue;
        u64 limit = clock::uptime_ms() + 200;
        while (!__atomic_load_n(&probe.ran, __ATOMIC_SEQ_CST) && clock::uptime_ms() < limit) asm volatile("pause");
        if (!__atomic_load_n(&probe.ran, __ATOMIC_SEQ_CST)) continue;
        u64 delta = probe.started_tsc > t0 ? probe.started_tsc - t0 : 0;
        samples[taken++] = delta * 1000 / khz;
    }
    for (u32 i = 1; i < taken; ++i) {
        u64 v = samples[i];
        u32 j = i;
        while (j > 0 && samples[j - 1] > v) { samples[j] = samples[j - 1]; --j; }
        samples[j] = v;
    }
    bool ok = taken >= LATENCY_SAMPLES / 2;
    u64 median = ok ? samples[taken / 2] : 0;
    u64 worst = ok ? samples[taken - 1] : 0;
    ok = ok && median < LATENCY_LIMIT_US;
    Stats st = stats();
    serial::printf("[sched] wake latency over %u spawns on %u cores: median %lu us, worst %lu us (limit %lu us)\n",
                   taken, c, median, worst, LATENCY_LIMIT_US);
    serial::printf("[sched] counters: %lu preemptions, %lu idle wakeups, %lu steals, %lu IPIs sent\n",
                   st.preemptions, st.idle_wakeups, st.steals, st.ipis_sent);
    serial::printf("[selftest] wake latency: %s\n", ok ? "ok" : "MISMATCH");
    return ok;
}

bool test_fpu_isolation(bool* fpu_checked) {
    constellation::Space* space = constellation::create_space();
    bool ok = space != nullptr;
    for (u32 i = 0; i < FPU_WORKERS; ++i) {
        g_workers[i] = {};
        g_workers[i].id = i;
        g_workers[i].space = i == SPACE_WORKER ? space : nullptr;
        ok = ok && spawn_in("fpu-worker", &fpu_worker, &g_workers[i], g_workers[i].space);
    }
    u32 finished = 0;
    u64 end = clock::uptime_ms() + RUN_MS + GIVE_UP_MS;
    while (finished < FPU_WORKERS && clock::uptime_ms() < end) {
        sleep_ms(10);
        finished = 0;
        for (auto& w : g_workers) finished += __atomic_load_n(&w.done, __ATOMIC_SEQ_CST);
    }
    ok = ok && finished == FPU_WORKERS;

    u64 checks = 0, switches = 0;
    u32 state = 0, modes = 0, stacks = 0, spaces = 0;
    bool moved = true;
    for (auto& w : g_workers) {
        checks += w.checks;
        switches += w.switches;
        state += w.state_errors;
        modes += w.mode_errors;
        stacks += w.stack_errors;
        spaces += w.space_errors;
        if (w.switches < 2 || w.checks == 0) moved = false;
    }
    serial::printf("[sched] %u satellites ran %lu register checks across %lu context switches\n", FPU_WORKERS, checks, switches);
    serial::printf("[sched] errors: %u register state, %u x87/MXCSR mode, %u TSS rsp0, %u address space\n", state, modes, stacks, spaces);

    *fpu_checked = fpu::usable();
    if (space) {
        ok = ok && constellation::space_cpumask(space) == 0;
        constellation::destroy_space(space);
    }
    bool isolation_ok = ok && moved && state == 0 && modes == 0;
    serial::printf("[selftest] fpu state isolation: %s\n", !fpu::usable() ? "skipped" : (isolation_ok ? "ok" : "MISMATCH"));
    serial::printf("[selftest] tss rsp0: %s\n", (ok && moved && stacks == 0) ? "ok" : "MISMATCH");
    serial::printf("[selftest] address space switch: %s\n", (ok && moved && spaces == 0) ? "ok" : "MISMATCH");
    return ok && moved && state == 0 && modes == 0 && stacks == 0 && spaces == 0;
}

void orchestrator(void*) {
    bool fpu_checked = false;
    bool ok = test_sleep();
    ok = test_gate() && ok;
    ok = test_pingpong() && ok;
    ok = test_timed_wait() && ok;
    ok = test_semaphore() && ok;
    ok = test_sleeplock() && ok;
    ok = test_join() && ok;
    ok = test_fpu_isolation(&fpu_checked) && ok;
    ok = test_introspection() && ok;
    ok = test_wake_latency() && ok;
    u64 b = boosts();
    serial::printf("[sched] priority boosts performed: %lu\n", b);
    ok = ok && b > 0;
    serial::printf("[selftest] sched: %s\n", ok ? "ok" : "MISMATCH");
}

}

void spawn_selftest() {
    if (!spawn("sched-selftest", &orchestrator, nullptr)) serial::writeln("[sched] FAIL: could not start the scheduler self-test");
}

}
