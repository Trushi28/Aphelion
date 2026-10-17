#include <cosmos/orbital.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/fpu.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/vmm.hpp>

namespace orbital {

namespace {

constexpr u32 FPU_WORKERS = 6;
constexpr u32 SPACE_WORKER = 1;
constexpr u32 RUN_MS = 700;
constexpr u32 PINGPONG_ROUNDS = 1500;
constexpr u32 WAITERS = 3;
constexpr u64 GIVE_UP_MS = 8000;

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
    ok = test_fpu_isolation(&fpu_checked) && ok;
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
