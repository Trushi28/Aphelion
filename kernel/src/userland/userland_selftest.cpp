#include <cosmos/userland.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/entrycheck.hpp>
#include <cosmos/fpu.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/lens_channel.hpp>
#include <cosmos/serial.hpp>

extern "C" const u8 isr_common[];
extern "C" const u8 isr_paranoid[];

#define BLOB_DECL(name) extern "C" const u8 ublob_##name[]; extern "C" const u8 ublob_##name##_end[]

BLOB_DECL(hello);
BLOB_DECL(exit42);
BLOB_DECL(null);
BLOB_DECL(readkernel);
BLOB_DECL(jumpkernel);
BLOB_DECL(privileged);
BLOB_DECL(divzero);
BLOB_DECL(badptr);
BLOB_DECL(registers);
BLOB_DECL(worker);
BLOB_DECL(datapage);
BLOB_DECL(gsbase);
BLOB_DECL(gsbad);
BLOB_DECL(gsiso);
BLOB_DECL(gszero);
BLOB_DECL(fsbase);
BLOB_DECL(fsbad);
BLOB_DECL(fsiso);
BLOB_DECL(fszero);
BLOB_DECL(vecsse);
BLOB_DECL(vecavx);
BLOB_DECL(nmispin);
BLOB_DECL(threads);
BLOB_DECL(lastexit);
BLOB_DECL(lastchild);
BLOB_DECL(groupexit);
BLOB_DECL(threadfault);
BLOB_DECL(spinner);
BLOB_DECL(joinhang);
BLOB_DECL(threadlimit);
BLOB_DECL(chanecho);
BLOB_DECL(chanwait);
BLOB_DECL(chanlimits);
BLOB_DECL(peer_a);
BLOB_DECL(peer_b);

namespace userland {

namespace {

constexpr u64 WAIT_MS = 30000;
constexpr u32 WORKERS = 6;
constexpr u32 SNAPSHOT_MAX = 64;

struct Case {
    const char* name;
    const u8* begin;
    const u8* end;
    u32 data_pages;
    i32 expect;
};

#define CASE(blob, data, expect) { #blob, ublob_##blob, ublob_##blob##_end, data, expect }

const Case g_cases[] = {
    CASE(hello, 0, 0),
    CASE(exit42, 0, 42),
    CASE(null, 0, userabi::fault_status(14)),
    CASE(readkernel, 0, userabi::fault_status(14)),
    CASE(jumpkernel, 0, userabi::fault_status(14)),
    CASE(privileged, 0, userabi::fault_status(13)),
    CASE(divzero, 0, userabi::fault_status(0)),
    CASE(badptr, 0, 0),
    CASE(registers, 0, 0),
    CASE(datapage, 1, userabi::fault_status(14)),
    CASE(gsbase, 1, 0),
    CASE(gsbad, 0, 0),
    CASE(gszero, 0, 0),
    CASE(fsbase, 1, 0),
    CASE(fsbad, 0, 0),
    CASE(threads, 2, 0),
    CASE(lastexit, 2, 44),
    CASE(lastchild, 2, 66),
    CASE(groupexit, 2, 33),
    CASE(threadfault, 2, userabi::fault_status(6)),
    CASE(threadlimit, 2, 0),
    CASE(chanlimits, 2, 0),
};

Image image_of(const u8* begin, const u8* end, u32 data_pages) {
    return Image{ begin, static_cast<u64>(end - begin), 0, data_pages };
}

bool run_case(const Case& c) {
    Process* p = launch(c.name, image_of(c.begin, c.end, c.data_pages));
    if (!p) {
        serial::printf("[userland] case '%s' could not be launched\n", c.name);
        return false;
    }
    i32 status = 0;
    if (!wait(p, &status, WAIT_MS)) {
        serial::printf("[userland] case '%s' did not finish within %lu ms\n", c.name, WAIT_MS);
        return false;
    }
    bool reaped = reap(p);
    bool match = status == c.expect;
    serial::printf("[userland] case '%s': status %d, expected %d%s\n", c.name, status, c.expect,
                    match ? "" : "  <-- wrong status");
    if (!reaped) serial::printf("[userland] case '%s' could not be torn down\n", c.name);
    return match && reaped;
}

bool run_workers() {
    Process* procs[WORKERS];
    const Image img = image_of(ublob_worker, ublob_worker_end, 0);
    bool ok = true;
    for (u32 i = 0; i < WORKERS; ++i) {
        procs[i] = launch("worker", img);
        if (!procs[i]) ok = false;
    }
    u32 cores = 0;
    for (u32 i = 0; i < WORKERS; ++i) {
        if (!procs[i]) continue;
        i32 status = -1;
        bool finished = wait(procs[i], &status, WAIT_MS);
        if (!finished || status != 0) ok = false;
        if (finished) {
            u32 used = cores_used(procs[i]);
            cores = cores > used ? cores : used;
            if (!reap(procs[i])) ok = false;
        }
    }
    serial::printf("[userland] %u concurrent processes: %s (a single process ran on up to %u core(s))\n",
                    WORKERS, ok ? "all exited cleanly" : "failed", cores);
    return ok;
}

bool run_base_mix() {
    struct Mix {
        const char* name;
        const u8* begin;
        const u8* end;
        u32 data_pages;
    };
    const Mix mix[] = {
        { "gsiso", ublob_gsiso, ublob_gsiso_end, 1 },
        { "fsiso", ublob_fsiso, ublob_fsiso_end, 1 },
        { "gszero", ublob_gszero, ublob_gszero_end, 0 },
        { "fszero", ublob_fszero, ublob_fszero_end, 0 },
        { "fsiso", ublob_fsiso, ublob_fsiso_end, 1 },
        { "gsiso", ublob_gsiso, ublob_gsiso_end, 1 },
        { "fszero", ublob_fszero, ublob_fszero_end, 0 },
        { "gszero", ublob_gszero, ublob_gszero_end, 0 },
    };
    constexpr u32 COUNT = sizeof(mix) / sizeof(mix[0]);
    Process* procs[COUNT];
    bool ok = true;
    for (u32 i = 0; i < COUNT; ++i) {
        procs[i] = launch(mix[i].name, image_of(mix[i].begin, mix[i].end, mix[i].data_pages));
        if (!procs[i]) ok = false;
    }
    for (u32 i = 0; i < COUNT; ++i) {
        if (!procs[i]) continue;
        i32 status = -1;
        bool finished = wait(procs[i], &status, WAIT_MS);
        if (!finished || status != 0) {
            serial::printf("[userland] base process %u ('%s'): %s, status %d\n", i, mix[i].name, finished ? "wrong status" : "did not finish", status);
            ok = false;
        }
        if (finished && !reap(procs[i])) ok = false;
    }
    serial::printf("[userland] user GS and FS bases kept per thread across %u concurrent processes: %s\n", COUNT, ok ? "ok" : "failed");
    return ok;
}

bool run_vector_state() {
    if (!fpu::usable()) {
        serial::writeln("[userland] user vector state: skipped (the kernel has no FPU support on this boot)");
        return true;
    }
    constexpr i32 AVX_ABSENT = 78;
    const bool avx = fpu::avx_state();
    struct Job {
        const char* name;
        const u8* begin;
        const u8* end;
        i32 expect;
    };
    const Job jobs[] = {
        { "vecsse", ublob_vecsse, ublob_vecsse_end, 0 },
        { "vecavx", ublob_vecavx, ublob_vecavx_end, avx ? 0 : AVX_ABSENT },
        { "vecsse", ublob_vecsse, ublob_vecsse_end, 0 },
        { "vecavx", ublob_vecavx, ublob_vecavx_end, avx ? 0 : AVX_ABSENT },
        { "vecsse", ublob_vecsse, ublob_vecsse_end, 0 },
        { "vecavx", ublob_vecavx, ublob_vecavx_end, avx ? 0 : AVX_ABSENT },
        { "vecsse", ublob_vecsse, ublob_vecsse_end, 0 },
        { "vecavx", ublob_vecavx, ublob_vecavx_end, avx ? 0 : AVX_ABSENT },
    };
    constexpr u32 COUNT = sizeof(jobs) / sizeof(jobs[0]);
    Process* procs[COUNT];
    bool ok = true;
    for (u32 i = 0; i < COUNT; ++i) {
        procs[i] = launch(jobs[i].name, image_of(jobs[i].begin, jobs[i].end, 0));
        if (!procs[i]) ok = false;
    }
    for (u32 i = 0; i < COUNT; ++i) {
        if (!procs[i]) continue;
        i32 status = -1;
        bool finished = wait(procs[i], &status, WAIT_MS);
        if (!finished || status != jobs[i].expect) {
            serial::printf("[userland] vector process %u ('%s'): %s, status %d, expected %d\n", i, jobs[i].name,
                            finished ? "wrong status" : "did not finish", status, jobs[i].expect);
            ok = false;
        }
        if (finished && !reap(procs[i])) ok = false;
    }
    serial::printf("[userland] user x87, SSE%s and MXCSR state kept per thread across %u concurrent processes: %s\n",
                    avx ? ", AVX" : " (this boot has no AVX state)", COUNT, ok ? "ok" : "failed");
    return ok;
}

bool run_kill() {
    struct Target {
        const char* name;
        const u8* begin;
        const u8* end;
        u32 threads;
        u32 settle_ms;
    };
    const Target targets[] = {
        { "spinner", ublob_spinner, ublob_spinner_end, 3, 40 },
        { "joinhang", ublob_joinhang, ublob_joinhang_end, 2, 40 },
        { "spinner", ublob_spinner, ublob_spinner_end, 3, 0 },
        { "joinhang", ublob_joinhang, ublob_joinhang_end, 2, 0 },
        { "spinner", ublob_spinner, ublob_spinner_end, 3, 10 },
    };
    bool ok = true;
    for (const Target& t : targets) {
        Process* p = launch(t.name, image_of(t.begin, t.end, 2));
        if (!p) {
            serial::printf("[userland] kill case '%s' could not be launched\n", t.name);
            ok = false;
            continue;
        }
        if (t.settle_ms) orbital::sleep_ms(t.settle_ms);
        u32 running = live_threads(p);
        u64 started = clock::uptime_ms();
        bool requested = kill(p);
        i32 status = 0;
        bool finished = wait(p, &status, WAIT_MS);
        u64 took = clock::uptime_ms() - started;
        bool second = kill(p);
        u32 created = threads_created(p);
        bool reaped = finished && reap(p);
        bool good = requested && finished && status == userabi::STATUS_KILLED && !second && reaped;
        if (t.settle_ms >= 40 && (running != t.threads || created != t.threads)) good = false;
        serial::printf("[userland] kill '%s': %u of %u thread(s) were running, process ended with status %d after %lu ms%s\n",
                        t.name, running, created, status, took, good ? "" : "  <-- wrong");
        ok = ok && good;
    }
    Process* done = launch("exit42", image_of(ublob_exit42, ublob_exit42_end, 0));
    if (!done) {
        serial::writeln("[userland] kill-after-exit case could not be launched");
        return false;
    }
    i32 status = 0;
    bool finished = wait(done, &status, WAIT_MS);
    bool late = finished && kill(done);
    bool reaped = finished && reap(done);
    bool good = finished && status == 42 && !late && reaped;
    serial::printf("[userland] kill of a process that already exited: %s, status stays %d%s\n",
                    late ? "wrongly accepted" : "refused", status, good ? "" : "  <-- wrong");
    return ok && good;
}

lens::Table g_kernel_handles;

bool kernel_receive(lens::Lens end, char* out, u32 cap, lens::Lens* handles, u32 hcap, lens::channel::Received* got, lens::Status* why) {
    lens::Result<lens::channel::Received> r = lens::channel::receive(g_kernel_handles, end, lens::Bytes(reinterpret_cast<u8*>(out), cap),
                                                                      handles, hcap, false, nullptr, nullptr);
    if (why) *why = r.status();
    if (!r.ok()) return false;
    *got = r.value();
    return true;
}

bool text_is(const char* got, u32 n, const char* want) {
    u32 w = 0;
    while (want[w]) ++w;
    if (w != n) return false;
    for (u32 i = 0; i < n; ++i) if (got[i] != want[i]) return false;
    return true;
}

bool run_handles() {
    bool ok = true;
    lens::Table& K = g_kernel_handles;
    lens::Lens a, b, c, d;
    ok = lens::channel::create(K, &a, &b).ok() && lens::channel::create(K, &c, &d).ok() && ok;

    Process* echo = launch_with("chanecho", image_of(ublob_chanecho, ublob_chanecho_end, 2), K, b);
    bool echo_ok = echo != nullptr;
    if (echo) {
        lens::Done sent = lens::channel::send(K, a, lens::text_bytes("carry"), &c, 1);
        echo_ok = sent.ok() && !lens::is_live(K.domain, c);
        i32 status = -1;
        bool finished = wait(echo, &status, WAIT_MS);
        echo_ok = echo_ok && finished && status == 0;
        char buf[lens::channel::MAX_BYTES];
        lens::channel::Received got{};
        lens::Lens moved[lens::channel::MAX_HANDLES];
        echo_ok = echo_ok && kernel_receive(d, buf, sizeof(buf), nullptr, 0, &got, nullptr) && text_is(buf, got.bytes, "via!");
        echo_ok = echo_ok && kernel_receive(a, buf, sizeof(buf), moved, lens::channel::MAX_HANDLES, &got, nullptr) &&
                  text_is(buf, got.bytes, "back") && got.handles == 1;
        if (echo_ok) {
            echo_ok = lens::channel::send(K, moved[0], lens::text_bytes("zzz"), nullptr, 0).ok() &&
                      kernel_receive(d, buf, sizeof(buf), nullptr, 0, &got, nullptr) && text_is(buf, got.bytes, "zzz");
            (void)lens::close(K.domain, moved[0]);
        }
        lens::Status why = lens::Status::Ok;
        bool hung_up = !kernel_receive(a, buf, sizeof(buf), nullptr, 0, &got, &why) && why == lens::Status::Closed;
        echo_ok = echo_ok && hung_up && live_handles(echo) == 0;
        echo_ok = (finished && reap(echo)) && echo_ok;
        serial::printf("[userland] handle passing kernel -> ring 3 -> kernel: status %d, handle moved out of the sender, used by the receiver, returned%s\n",
                        status, echo_ok ? "" : "  <-- wrong");
    } else {
        serial::writeln("[userland] handle passing case could not be launched");
    }
    ok = ok && echo_ok;
    (void)lens::close(K.domain, a);
    (void)lens::close(K.domain, d);

    lens::Lens wa, wb;
    ok = lens::channel::create(K, &wa, &wb).ok() && ok;
    Process* waiter = launch_with("chanwait", image_of(ublob_chanwait, ublob_chanwait_end, 2), K, wb);
    bool wait_ok = waiter != nullptr;
    if (waiter) {
        orbital::sleep_ms(50);
        u32 running = live_threads(waiter);
        u64 started = clock::uptime_ms();
        bool requested = kill(waiter);
        i32 status = 0;
        bool finished = wait(waiter, &status, WAIT_MS);
        u64 took = clock::uptime_ms() - started;
        char buf[lens::channel::MAX_BYTES];
        lens::channel::Received got{};
        lens::Status why = lens::Status::Ok;
        bool hung_up = !kernel_receive(wa, buf, sizeof(buf), nullptr, 0, &got, &why) && why == lens::Status::Closed;
        wait_ok = requested && finished && status == userabi::STATUS_KILLED && running == 2 && hung_up && live_handles(waiter) == 0;
        wait_ok = (finished && reap(waiter)) && wait_ok;
        serial::printf("[userland] kill of a process blocked in channel receive and handle wait: %u threads were blocked, status %d after %lu ms, its channel hung up%s\n",
                        running, status, took, wait_ok ? "" : "  <-- wrong");
    } else {
        serial::writeln("[userland] blocked-receiver case could not be launched");
    }
    ok = ok && wait_ok;
    (void)lens::close(K.domain, wa);

    lens::Lens k1, p1, k2, p2, x1, x2;
    bool made = lens::channel::create(K, &k1, &p1).ok() && lens::channel::create(K, &k2, &p2).ok() && lens::channel::create(K, &x1, &x2).ok();
    Process* pa = made ? launch_with("peer_a", image_of(ublob_peer_a, ublob_peer_a_end, 2), K, p1) : nullptr;
    Process* pb = made ? launch_with("peer_b", image_of(ublob_peer_b, ublob_peer_b_end, 2), K, p2) : nullptr;
    bool peers_ok = pa && pb;
    if (peers_ok) {
        peers_ok = lens::channel::send(K, k1, lens::text_bytes("go"), &x1, 1).ok() && lens::channel::send(K, k2, lens::text_bytes("go"), &x2, 1).ok();
        i32 sa = -1, sb = -1;
        bool fa = wait(pa, &sa, WAIT_MS);
        bool fb = wait(pb, &sb, WAIT_MS);
        peers_ok = peers_ok && fa && fb && sa == 0 && sb == 0;
        serial::printf("[userland] two ring-3 processes talk over a channel the kernel handed each one an end of: statuses %d and %d%s\n",
                        sa, sb, peers_ok ? "" : "  <-- wrong");
        peers_ok = (fa && reap(pa)) && peers_ok;
        peers_ok = (fb && reap(pb)) && peers_ok;
    } else {
        serial::writeln("[userland] peer case could not be launched");
        if (pa) { kill(pa); i32 st; wait(pa, &st, WAIT_MS); reap(pa); }
        if (pb) { kill(pb); i32 st; wait(pb, &st, WAIT_MS); reap(pb); }
    }
    ok = ok && peers_ok;
    lens::close_all(K.domain);
    ok = ok && lens::live_count(K.domain) == 0;
    return ok;
}

bool entry_paths_fenced() {
    constexpr u32 WINDOW = 128;
    bool common = entrycheck::first_swapgs(isr_common, WINDOW) == entrycheck::Verdict::Fenced;
    bool paranoid = entrycheck::first_swapgs(isr_paranoid, WINDOW) == entrycheck::Verdict::Fenced;
    serial::printf("[userland] lfence after the conditional swapgs: ordinary entry %s, paranoid entry %s\n",
                    common ? "yes" : "MISSING", paranoid ? "yes" : "MISSING");
    return common && paranoid;
}

bool run_nmi() {
    orbital::CoreInfo cores[16];
    u32 core_count = orbital::cores(cores, 16);
    if (core_count < 2) {
        serial::writeln("[userland] NMI from ring 3: skipped (needs at least two cores, an NMI to the sender's own core would land in ring 0)");
        return true;
    }
    Process* p = launch("nmispin", image_of(ublob_nmispin, ublob_nmispin_end, 0));
    if (!p) {
        serial::writeln("[userland] NMI case could not be launched");
        return false;
    }
    u32 task = task_of(p);
    u64 user_before = nmis_from_user();
    u64 absorbed_before = nmis_absorbed();
    u32 sent = 0;
    expect_nmis(0);
    for (u32 attempt = 0; attempt < 400; ++attempt) {
        i32 status;
        if (wait(p, &status, 0)) break;
        orbital::SatelliteInfo infos[SNAPSHOT_MAX];
        u32 n = orbital::snapshot(infos, SNAPSHOT_MAX);
        for (u32 i = 0; i < n; ++i) {
            if (infos[i].id != task || infos[i].state != orbital::State::Running || infos[i].core == apic::id()) continue;
            expect_nmis(1);
            apic::send_nmi(infos[i].core);
            ++sent;
            break;
        }
        orbital::sleep_ms(3);
    }
    i32 status = -1;
    bool finished = wait(p, &status, WAIT_MS);
    expect_nmis(0);
    bool reaped = finished && reap(p);
    u64 in_ring3 = nmis_from_user() - user_before;
    u64 in_ring0 = nmis_absorbed() - absorbed_before;
    bool ok = finished && status == 0 && reaped;
    serial::printf("[userland] NMI from ring 3: %u sent, %lu landed in ring 3, %lu landed in ring 0 and were absorbed, process status %d%s\n",
                    sent, in_ring3, in_ring0, status, ok ? "" : "  <-- wrong");
    if (ok && in_ring3 == 0)
        serial::writeln("[userland] NMI from ring 3: inconclusive, no NMI happened to land in ring 3 on this run");
    return ok;
}

void orchestrator(void*) {
    if (!supported()) {
        serial::writeln("[selftest] userland: skipped");
        return;
    }
    bool ok = entry_paths_fenced();
    for (const Case& c : g_cases) ok = run_case(c) && ok;
    ok = run_workers() && ok;
    ok = run_base_mix() && ok;
    ok = run_vector_state() && ok;
    ok = run_kill() && ok;
    ok = run_handles() && ok;
    ok = run_nmi() && ok;
    ok = ok && frames_live() == 0 && live_processes() == 0;
    ok = ok && frames_quarantined() == 0;
    serial::printf("[userland] frames still held: %lu, processes still live: %u\n", frames_live(), live_processes());
    serial::printf("[selftest] userland: %s\n", ok ? "ok" : "MISMATCH");
}

}

void spawn_selftest() {
    if (!orbital::spawn("userland-selftest", &orchestrator, nullptr))
        serial::writeln("[userland] could not start the ring-3 self-test");
}

}
