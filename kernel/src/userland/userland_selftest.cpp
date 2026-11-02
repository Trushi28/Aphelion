#include <cosmos/userland.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/serial.hpp>

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
BLOB_DECL(nmispin);

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

bool run_gs_mix() {
    constexpr u32 COUNT = 6;
    Process* procs[COUNT];
    bool ok = true;
    for (u32 i = 0; i < COUNT; ++i) {
        bool isolated = (i & 1) == 0;
        const Image img = isolated ? image_of(ublob_gsiso, ublob_gsiso_end, 1) : image_of(ublob_gszero, ublob_gszero_end, 0);
        procs[i] = launch(isolated ? "gsiso" : "gszero", img);
        if (!procs[i]) ok = false;
    }
    for (u32 i = 0; i < COUNT; ++i) {
        if (!procs[i]) continue;
        i32 status = -1;
        bool finished = wait(procs[i], &status, WAIT_MS);
        if (!finished || status != 0) {
            serial::printf("[userland] gs process %u: %s, status %d\n", i, finished ? "wrong status" : "did not finish", status);
            ok = false;
        }
        if (finished && !reap(procs[i])) ok = false;
    }
    serial::printf("[userland] user GS base kept per thread across %u concurrent processes: %s\n", COUNT, ok ? "ok" : "failed");
    return ok;
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
    bool ok = true;
    for (const Case& c : g_cases) ok = run_case(c) && ok;
    ok = run_workers() && ok;
    ok = run_gs_mix() && ok;
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
