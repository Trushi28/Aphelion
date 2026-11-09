#include <cosmos/userland.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/entrycheck.hpp>
#include <cosmos/fpu.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/lens_channel.hpp>
#include <cosmos/lens_text.hpp>
#include <cosmos/apx.hpp>
#include <cosmos/fileabi.hpp>
#include <cosmos/handleabi.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/memobj.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>

extern "C" const u8 isr_common[];
extern "C" const u8 isr_paranoid[];
extern "C" const u8 apx_hello_start[];
extern "C" const u8 apx_hello_end[];
extern "C" const u8 apx_pehost_start[];
extern "C" const u8 apx_pehost_end[];
extern "C" const u8 pe_console_start[];
extern "C" const u8 pe_console_end[];
extern "C" const u8 pe_k32_start[];
extern "C" const u8 pe_k32_end[];
extern "C" const u8 pe_teb_start[];
extern "C" const u8 pe_teb_end[];
extern "C" const u8 pe_tlscb_start[];
extern "C" const u8 pe_tlscb_end[];
extern "C" const u8 pe_stdio_start[];
extern "C" const u8 pe_stdio_end[];
extern "C" const u8 pe_crt_start[];
extern "C" const u8 pe_crt_end[];
extern "C" const u8 pe_ucrt_start[];
extern "C" const u8 pe_ucrt_end[];

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
BLOB_DECL(vmbasic);
BLOB_DECL(vmnx);
BLOB_DECL(vmwro);
BLOB_DECL(vmwrx);
BLOB_DECL(vmgone);
BLOB_DECL(vmlimits);
BLOB_DECL(vmstack);
BLOB_DECL(vmshoot);
BLOB_DECL(vmrace);
BLOB_DECL(vmshare);
BLOB_DECL(vmspin);
BLOB_DECL(fileio);
BLOB_DECL(apxchild);
BLOB_DECL(apxspin);
BLOB_DECL(apxdata);
BLOB_DECL(spawner);

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
    CASE(vmbasic, 2, 0),
    CASE(vmnx, 2, userabi::fault_status(14)),
    CASE(vmwro, 2, userabi::fault_status(14)),
    CASE(vmwrx, 2, userabi::fault_status(14)),
    CASE(vmgone, 2, userabi::fault_status(14)),
    CASE(vmlimits, 2, 0),
    CASE(vmstack, 2, 0),
    CASE(vmshoot, 2, userabi::fault_status(14)),
    CASE(vmrace, 2, 0),
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

bool run_vm() {
    bool ok = true;
    lens::Table& K = g_kernel_handles;
    u64 base_pages = memobj::live_pages();
    u64 base_objects = memobj::live_objects();

    memobj::Object* obj = memobj::create(2);
    bool share_ok = obj != nullptr;
    lens::Lens keep, rw, ro, ca, cb;
    if (share_ok) {
        u64* page0 = static_cast<u64*>(universe::to_virt(memobj::frame(obj, 0)));
        *page0 = 0xA9E110FA9E110FA1ull;
        lens::Result<lens::Lens> made = lens::alloc_slot(K.domain, nullptr, nebula::Node{0, 0}, nebula::Node{0, 0}, memobj::HANDLE_RIGHTS,
                                                         false, 0, "memobj", obj);
        share_ok = made.ok();
        if (made.ok()) {
            lens::Result<lens::Lens> a = lens::narrow(K.domain, made.value(), memobj::HANDLE_RIGHTS);
            lens::Result<lens::Lens> b = lens::narrow(K.domain, made.value(), lens::Right::Read | lens::Right::Delegate);
            keep = made.value();
            share_ok = a.ok() && b.ok();
            if (a.ok()) rw = a.value();
            if (b.ok()) ro = b.value();
        } else {
            memobj::discard(obj);
        }
    }
    share_ok = share_ok && lens::channel::create(K, &ca, &cb).ok();
    Process* sharer = share_ok ? launch_with("vmshare", image_of(ublob_vmshare, ublob_vmshare_end, 2), K, cb) : nullptr;
    if (sharer) {
        lens::Lens both[2] = { rw, ro };
        share_ok = lens::channel::send(K, ca, lens::text_bytes("vmo"), both, 2).ok();
        i32 status = -1;
        bool finished = wait(sharer, &status, WAIT_MS);
        u64 seen = 0;
        if (finished) {
            const u64* page1 = static_cast<const u64*>(universe::to_virt(memobj::frame(obj, 1)));
            seen = page1[1];
        }
        share_ok = share_ok && finished && status == 0 && seen == 0x5EED5EED5EED5EEDull && live_handles(sharer) == 0;
        share_ok = (finished && reap(sharer)) && share_ok;
        serial::printf("[userland] shared memory object: kernel-made, handed to ring 3 as read-write and read-only handles: status %d, the process's write is visible to the kernel%s\n",
                        status, share_ok ? "" : "  <-- wrong");
    } else {
        serial::writeln("[userland] shared memory case could not be launched");
        share_ok = false;
    }
    lens::close_all(K.domain);
    ok = share_ok && lens::live_count(K.domain) == 0 && ok;
    bool freed = memobj::live_pages() == base_pages && memobj::live_objects() == base_objects;
    serial::printf("[userland] memory object freed once the last handle and mapping are gone: %s\n", freed ? "yes" : "NO  <-- wrong");
    ok = freed && ok;

    Process* spin = launch("vmspin", image_of(ublob_vmspin, ublob_vmspin_end, 2));
    bool spin_ok = spin != nullptr;
    if (spin) {
        orbital::sleep_ms(50);
        u64 held = memobj::live_pages();
        bool requested = kill(spin);
        i32 status = 0;
        bool finished = wait(spin, &status, WAIT_MS);
        bool reaped = finished && reap(spin);
        spin_ok = requested && finished && reaped && status == userabi::STATUS_KILLED && held == base_pages + 16 && memobj::live_pages() == base_pages &&
                  memobj::live_objects() == base_objects;
        serial::printf("[userland] kill of a process whose only reference to 16 pages is a mapping: status %d, %lu pages held, %lu left after teardown%s\n",
                        status, held - base_pages, memobj::live_pages() - base_pages, spin_ok ? "" : "  <-- wrong");
    } else {
        serial::writeln("[userland] mapping-kill case could not be launched");
    }
    return spin_ok && ok;
}

bool put_apx(lens::Domain& d, lens::Lens dir, const char* name, const u8* begin, const u8* end, u32 data_pages, const u8* data, u32 data_bytes) {
    u32 code = static_cast<u32>(end - begin);
    usize cap = static_cast<usize>(apx::pack_size(code, data_bytes));
    u8* buf = static_cast<u8*>(kheap::alloc(cap));
    if (!buf) return false;
    usize written = 0;
    bool ok = apx::pack(begin, code, 0, data_pages, data, data_bytes, buf, cap, &written) &&
              lens::put(d, dir, name, lens::ConstBytes(buf, written)).ok();
    kheap::free(buf);
    return ok;
}

bool file_equals(lens::Domain& d, lens::Lens dir, const char* path, const char* want) {
    lens::Result<lens::Lens> f = lens::open_at(d, dir, path, lens::Right::Read);
    if (!f.ok()) return false;
    u8 got[32] = {};
    usize n = 0;
    while (want[n]) ++n;
    lens::Result<usize> r = lens::read(d, f.value(), 0, lens::Bytes(got, sizeof(got)));
    bool same = r.ok() && r.value() == n;
    for (usize i = 0; same && i < n; ++i) same = got[i] == static_cast<u8>(want[i]);
    (void)lens::close(d, f.value());
    return same;
}

bool refused(lens::Table& K, lens::Lens file, i64 want, const char* what) {
    u32 before = live_processes();
    i64 error = 0;
    Process* p = launch_file("refused", K, file, nullptr, lens::Lens(), &error);
    bool ok = p == nullptr && error == want && live_processes() == before && frames_live() == 0;
    if (p) {
        kill(p);
        i32 st;
        (void)wait(p, &st, WAIT_MS);
        (void)reap(p);
    }
    serial::printf("[userland] launching %s: error %ld, expected %ld%s\n", what, error, want, ok ? "" : "  <-- wrong");
    return ok;
}

bool put_blob(lens::Domain& d, lens::Lens dir, const char* name, const u8* begin, const u8* end) {
    return lens::put(d, dir, name, lens::ConstBytes(begin, static_cast<usize>(end - begin))).ok();
}

bool run_pe(lens::Table& K, lens::Lens apps, const char* name, i32 want) {
    lens::Result<lens::Lens> host = lens::open_at(K.domain, apps, "pehost", lens::Right::Read | lens::Right::Execute);
    lens::Result<lens::Lens> prog = lens::open_at(K.domain, apps, name, lens::Right::Read | lens::Right::Delegate);
    i64 error = -1;
    Process* p = host.ok() && prog.ok() ? launch_file("pehost", K, host.value(), &K, prog.value(), &error) : nullptr;
    i32 status = -1;
    bool good = p && wait(p, &status, WAIT_MS) && status == want;
    good = (p && reap(p)) && good;
    serial::printf("[userland] %s through the PE host: status %d, expected %d%s\n", name, status, want, good ? "" : "  <-- wrong");
    return good;
}

bool run_files() {
    lens::Table& K = g_kernel_handles;
    bool ok = true;
    lens::Result<lens::Lens> rootr = lens::delegate(lens::boot_domain(), lens::boot_root("ram"), K.domain, lens::Rights::all());
    if (!rootr.ok()) {
        serial::writeln("[userland] file cases: could not obtain the RAM root");
        return false;
    }
    lens::Lens root = rootr.value();
    (void)lens::remove_tree(K.domain, root, "h3-selftest");
    ok = lens::mkdir(K.domain, root, "h3-selftest").ok() && lens::mkdir(K.domain, root, "h3-selftest/apps").ok() &&
         lens::mkdir(K.domain, root, "h3-selftest/work").ok();
    lens::Result<lens::Lens> appsr = lens::open_at(K.domain, root, "h3-selftest/apps", lens::Rights::all());
    if (!ok || !appsr.ok()) {
        serial::writeln("[userland] file cases: could not prepare the test directories");
        lens::close_all(K.domain);
        return false;
    }
    lens::Lens apps = appsr.value();

    const u8 ab[2] = { 'A', 'B' };
    const u8 apxd[4] = { 'A', 'P', 'X', 'D' };
    bool put = put_apx(K.domain, apps, "exit42", ublob_exit42, ublob_exit42_end, 0, nullptr, 0) &&
               put_apx(K.domain, apps, "datainit", ublob_apxdata, ublob_apxdata_end, 1, ab, 2) &&
               put_apx(K.domain, apps, "child", ublob_apxchild, ublob_apxchild_end, 2, apxd, 4) &&
               put_apx(K.domain, apps, "spin", ublob_apxspin, ublob_apxspin_end, 0, nullptr, 0) &&
               put_apx(K.domain, apps, "bad", ublob_apxspin, ublob_apxspin_end, 0, nullptr, 0) &&
               lens::put(K.domain, apps, "hello", lens::ConstBytes(apx_hello_start, static_cast<usize>(apx_hello_end - apx_hello_start))).ok() &&
               put_blob(K.domain, apps, "pehost", apx_pehost_start, apx_pehost_end) &&
               put_blob(K.domain, apps, "console.exe", pe_console_start, pe_console_end) &&
               put_blob(K.domain, apps, "k32.exe", pe_k32_start, pe_k32_end) &&
               put_blob(K.domain, apps, "teb.exe", pe_teb_start, pe_teb_end) &&
               put_blob(K.domain, apps, "tlscb.exe", pe_tlscb_start, pe_tlscb_end) &&
               put_blob(K.domain, apps, "stdio.exe", pe_stdio_start, pe_stdio_end) &&
               put_blob(K.domain, apps, "crt.exe", pe_crt_start, pe_crt_end) &&
               put_blob(K.domain, apps, "ucrt.exe", pe_ucrt_start, pe_ucrt_end) &&
               lens::put(K.domain, apps, "short", lens::text_bytes("APX1 is not enough")).ok() &&
               lens::put(K.domain, apps, "empty", lens::ConstBytes(nullptr, 0)).ok();
    lens::Result<lens::Lens> badw = lens::open_at(K.domain, apps, "bad", lens::Right::Read | lens::Right::Write);
    if (put && badw.ok()) {
        u8 flip = 0;
        lens::Result<usize> got = lens::read(K.domain, badw.value(), 70, lens::Bytes(&flip, 1));
        flip ^= 0x40;
        put = got.ok() && got.value() == 1 && lens::write(K.domain, badw.value(), 70, lens::ConstBytes(&flip, 1)).ok() &&
              lens::sync(K.domain, badw.value()).ok();
        (void)lens::close(K.domain, badw.value());
    } else {
        put = false;
    }
    if (!put) {
        serial::writeln("[userland] file cases: could not write the test images");
        lens::close_all(K.domain);
        return false;
    }

    const lens::Rights run = lens::Right::Read | lens::Right::Execute;
    {
        lens::Result<lens::Lens> f = lens::open_at(K.domain, apps, "exit42", run);
        i64 error = -1;
        Process* p = f.ok() ? launch_file(nullptr, K, f.value(), nullptr, lens::Lens(), &error) : nullptr;
        i32 status = -1;
        bool good = p && wait(p, &status, WAIT_MS) && status == 42;
        good = (p && reap(p)) && good;
        serial::printf("[userland] process started from an APX file in the RAM realm: status %d%s\n", status, good ? "" : "  <-- wrong");
        ok = good && ok;
    }
    {
        lens::Result<lens::Lens> f = lens::open_at(K.domain, apps, "hello", run);
        i64 error = -1;
        Process* p = f.ok() ? launch_file(nullptr, K, f.value(), nullptr, lens::Lens(), &error) : nullptr;
        i32 status = -1;
        bool good = p && wait(p, &status, WAIT_MS) && status == 42;
        good = (p && reap(p)) && good;
        serial::printf("[userland] a C++ program built by the userland toolchain, with initialised data, zeroed data, floating point and a syscall: status %d%s\n",
                        status, good ? "" : "  <-- wrong");
        ok = good && ok;
    }
    {
        bool pe = run_pe(K, apps, "console.exe", 9);
        pe = run_pe(K, apps, "k32.exe", 0) && pe;
        pe = run_pe(K, apps, "teb.exe", 0) && pe;
        pe = run_pe(K, apps, "tlscb.exe", 0) && pe;
        pe = run_pe(K, apps, "stdio.exe", 0) && pe;
        pe = run_pe(K, apps, "crt.exe", 7) && pe;
        pe = run_pe(K, apps, "ucrt.exe", 7) && pe;
        pe = run_pe(K, apps, "short", static_cast<i32>(0xC000007Bu)) && pe;
        pe = run_pe(K, apps, "hello", static_cast<i32>(0xC000007Bu)) && pe;
        ok = pe && ok;
    }
    {
        lens::Result<lens::Lens> f = lens::open_at(K.domain, apps, "datainit", run);
        i64 error = -1;
        Process* p = f.ok() ? launch_file(nullptr, K, f.value(), nullptr, lens::Lens(), &error) : nullptr;
        i32 status = -1;
        bool good = p && wait(p, &status, WAIT_MS) && status == 0x42;
        good = (p && reap(p)) && good;
        serial::printf("[userland] initial data from the image is in place and the rest of the data page is zero: status %d%s\n", status,
                        good ? "" : "  <-- wrong");
        ok = good && ok;
    }
    {
        lens::Result<lens::Lens> noexec = lens::open_at(K.domain, apps, "exit42", lens::Right::Read);
        lens::Result<lens::Lens> bad = lens::open_at(K.domain, apps, "bad", run);
        lens::Result<lens::Lens> shortf = lens::open_at(K.domain, apps, "short", run);
        lens::Result<lens::Lens> empty = lens::open_at(K.domain, apps, "empty", run);
        bool opened = noexec.ok() && bad.ok() && shortf.ok() && empty.ok();
        bool r = opened;
        if (opened) {
            r = refused(K, noexec.value(), handleabi::ERR_DENIED, "a file without the execute right") && r;
            r = refused(K, bad.value(), handleabi::ERR_BADIMAGE, "an image with one flipped byte") && r;
            r = refused(K, shortf.value(), handleabi::ERR_BADIMAGE, "a file shorter than an APX header") && r;
            r = refused(K, empty.value(), handleabi::ERR_BADIMAGE, "an empty file") && r;
            r = refused(K, apps, fileabi::ERR_WRONGTYPE, "a directory") && r;
            r = refused(K, lens::Lens(0x7777777700000099ull), handleabi::ERR_BADHANDLE, "a handle that does not exist") && r;
        } else {
            serial::writeln("[userland] file cases: could not open the refusal targets");
        }
        ok = r && ok;
    }
    {
        lens::Result<lens::Lens> work = lens::open_at(K.domain, root, "h3-selftest/work", lens::Rights::all());
        lens::Result<lens::Lens> given = work.ok() ? lens::narrow(K.domain, work.value(), lens::RIGHTS_READ_WRITE | lens::Right::Delegate) : work;
        Process* p = given.ok() ? launch_with("fileio", image_of(ublob_fileio, ublob_fileio_end, 2), K, given.value()) : nullptr;
        i32 status = -1;
        bool good = p && wait(p, &status, WAIT_MS) && status == 0;
        good = (p && reap(p)) && good;
        bool hello = good && file_equals(K.domain, work.value(), "f.txt", "hello");
        bool nested = good && lens::open_at(K.domain, work.value(), "sub/g.txt", lens::Right::Read).ok();
        serial::printf("[userland] file and pipe syscalls from ring 3 on a directory handle: status %d, the file holds the data: %s, the nested file exists: %s%s\n",
                        status, hello ? "yes" : "NO", nested ? "yes" : "NO", good && hello && nested ? "" : "  <-- wrong");
        ok = good && hello && nested && ok;
    }
    {
        u64 reaped_before = detached_reaped();
        lens::Result<lens::Lens> given = lens::narrow(K.domain, apps, lens::Right::Read | lens::Right::Enumerate | lens::Right::Execute | lens::Right::Delegate);
        Process* p = given.ok() ? launch_with("spawner", image_of(ublob_spawner, ublob_spawner_end, 2), K, given.value()) : nullptr;
        i32 status = -1;
        bool good = p && wait(p, &status, WAIT_MS) && status == 0;
        good = (p && reap(p)) && good;
        for (u32 i = 0; i < 300 && live_processes() != 0; ++i) {
            (void)sweep_detached();
            if (live_processes() != 0) orbital::sleep_ms(10);
        }
        u64 swept = detached_reaped() - reaped_before;
        bool clean = live_processes() == 0 && frames_live() == 0;
        serial::printf("[userland] a process that spawns children from files, waits on them, kills one, and leaves two running: status %d, %lu children were reaped by the sweeper, nothing left: %s%s\n",
                        status, swept, clean ? "yes" : "NO", good && clean && swept == 4 ? "" : "  <-- wrong");
        ok = good && clean && swept == 4 && ok;
    }
    (void)lens::remove_tree(K.domain, root, "h3-selftest");
    lens::close_all(K.domain);
    bool closed = lens::live_count(K.domain) == 0;
    ok = closed && ok;
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
    ok = run_vm() && ok;
    ok = run_files() && ok;
    ok = run_nmi() && ok;
    ok = ok && frames_live() == 0 && live_processes() == 0;
    ok = ok && frames_quarantined() == 0;
    ok = ok && memobj::live_pages() == 0 && memobj::live_objects() == 0 && memobj::quarantined_pages() == 0;
    serial::printf("[userland] memory objects still alive: %lu, pages %lu\n", memobj::live_objects(), memobj::live_pages());
    serial::printf("[userland] frames still held: %lu, processes still live: %u\n", frames_live(), live_processes());
    serial::printf("[selftest] userland: %s\n", ok ? "ok" : "MISMATCH");
}

}

void spawn_selftest() {
    if (!orbital::spawn("userland-selftest", &orchestrator, nullptr))
        serial::writeln("[userland] could not start the ring-3 self-test");
}

}
