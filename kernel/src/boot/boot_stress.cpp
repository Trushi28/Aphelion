#include <cosmos/types.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/framebuffer.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/percore.hpp>
#include <cosmos/fpu.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/tlb.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/acpi.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/bootcfg.hpp>
#include <cosmos/timesrc.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/ioapic.hpp>
#include <cosmos/keyboard.hpp>
#include <cosmos/input.hpp>
#include <cosmos/power.hpp>
#include <cosmos/helm.hpp>
#include <cosmos/nebula_realms.hpp>
#include <cosmos/lens_text.hpp>
#include <cosmos/console_stream.hpp>
#include <cosmos/corona.hpp>
#include <cosmos/pci.hpp>
#include <cosmos/virtio_blk.hpp>
#include <cosmos/ahci.hpp>
#include <cosmos/nvme.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/stellar.hpp>
#include <cosmos/xhci.hpp>
#include <cosmos/usbstor.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/civil.hpp>
#include <cosmos/boot_tests.hpp>

extern volatile u64 g_cpus_online;

namespace boottest {

static void demo_cooperative(void* arg) {
    const char* label = static_cast<const char*>(arg);
    for (u32 i = 0; i < 8; ++i) {
        serial::printf("[demo] %s (cooperative) round %u on core %d\n",
                        label, i, static_cast<int>(apic::id()));
        orbital::yield();
    }
    serial::printf("[demo] %s done (returning -- implicit exit)\n", label);
}

static void demo_cooperative_explicit_exit(void* arg) {
    const char* label = static_cast<const char*>(arg);
    for (u32 i = 0; i < 8; ++i) {
        serial::printf("[demo] %s (cooperative) round %u on core %d\n",
                        label, i, static_cast<int>(apic::id()));
        orbital::yield();
    }
    serial::printf("[demo] %s done (calling exit_current explicitly)\n", label);
    orbital::exit_current();
}

static void demo_spinner(void* arg) {
    const char* label = static_cast<const char*>(arg);
    u64 spins = 0;
    u32 prints = 0;
    for (;;) {
        ++spins;
        if ((spins & 0xFFFFF) == 0 && prints < 8) {
            serial::printf("[demo] %s (never yields -- only timer-preempted) core %d, spins=%lu\n",
                            label, static_cast<int>(apic::id()), spins);
            ++prints;
        }
    }
}

constexpr u64 CLOCK_TEST_MS = 50;
constexpr u64 CLOCK_TEST_SPINS = 300000000ull;

static void demo_clock(void*) {
    u64 start = clock::uptime_ms();
    u64 prev = start;
    bool monotonic = true;
    u64 spins = 0;
    while (clock::uptime_ms() - start < CLOCK_TEST_MS && spins < CLOCK_TEST_SPINS) {
        u64 now = clock::uptime_ms();
        if (now < prev) monotonic = false;
        prev = now;
        ++spins;
    }
    u64 advanced = clock::uptime_ms() - start;
    bool ok = monotonic && advanced >= CLOCK_TEST_MS;
    serial::printf("[clock] BSP tick counter advanced %lu ms in the test window, %s\n",
                    advanced, monotonic ? "monotonic" : "went backwards");
    serial::printf("[selftest] clock ticks: %s\n", ok ? "ok" : "MISMATCH");
    orbital::exit_current();
}

constexpr u64 WORKER_COUNT = 8;
constexpr u64 WORKER_ITERATIONS_SINGLE = 50000;
constexpr u64 WORKER_ITERATIONS_MIN = 2000000;
constexpr u64 WORKER_ITERATIONS_CAP = 600000000;
static volatile u64 g_worker_cores_mask = 0;
static volatile u32 g_workers_remaining = WORKER_COUNT;

static u32 popcount64(u64 m) {
    u32 n = 0;
    while (m) { n += static_cast<u32>(m & 1); m >>= 1; }
    return n;
}

static void note_core(u32 core) {
    __atomic_fetch_or(&g_worker_cores_mask, 1ull << (core < 64 ? core : 63), __ATOMIC_SEQ_CST);
}

static void demo_worker(void* arg) {
    u64 idx = reinterpret_cast<u64>(arg);
    serial::printf("[demo] worker %lu started on core %d\n", idx, static_cast<int>(apic::id()));
    const bool expect_migration = g_cpus_online > 1;
    const u64 cap = expect_migration ? WORKER_ITERATIONS_CAP : WORKER_ITERATIONS_SINGLE;
    for (u64 i = 0; i < cap; ++i) {
        if ((i & 0x7FF) == 0) {
            note_core(apic::id());
            if (expect_migration && i >= WORKER_ITERATIONS_MIN && popcount64(g_worker_cores_mask) > 1) break;
            orbital::yield();
        }
    }
    u32 core = apic::id();
    note_core(core);
    serial::printf("[demo] worker %lu finished on core %d\n", idx, static_cast<int>(core));
    if (__atomic_sub_fetch(&g_workers_remaining, 1u, __ATOMIC_SEQ_CST) == 0) {
        u32 distinct = popcount64(g_worker_cores_mask);
        bool ok = distinct > 1;
        serial::printf("[orbital] work-stealing check: %lu worker(s) ran across %u distinct core(s) -> %s\n",
                        WORKER_COUNT, distinct, ok ? "stealing confirmed" : "no migration observed");
    }
    orbital::exit_current();
}

constexpr u64 FS_STRESS_WORKERS = 4;
constexpr u64 FS_STRESS_FILES = 25;
static bool g_fs_stress_enabled = false;
static u64 g_fs_stress_dir = stellar::INVALID_STAR;
static u64 g_fs_stress_alt = stellar::INVALID_STAR;
static volatile u32 g_fs_stress_remaining = FS_STRESS_WORKERS;
static volatile u32 g_fs_stress_errors = 0;

static void fs_stress_name(char* out, u64 w, u64 j) {
    u32 n = 0;
    out[n++] = 's';
    n += itoa_dec(w, out + n);
    out[n++] = '_';
    n += itoa_dec(j, out + n);
    out[n] = 0;
}

static void fs_stress_fail(const char* why, u64 w, u64 j) {
    __atomic_fetch_add(&g_fs_stress_errors, 1u, __ATOMIC_SEQ_CST);
    serial::printf("[smp-fs] FAIL: %s (worker %lu, file %lu)\n", why, w, j);
}

static void fs_stress_finish() {
    const u64 NONE = stellar::INVALID_STAR;
    u64 total = FS_STRESS_WORKERS * FS_STRESS_FILES;
    u64 shared_in_dir = stellar::find(g_fs_stress_dir, "shared") != NONE ? 1 : 0;
    u64 shared_in_alt = stellar::find(g_fs_stress_alt, "shared") != NONE ? 1 : 0;
    if (shared_in_dir + shared_in_alt != 1) {
        serial::printf("[smp-fs] FAIL: the contended file exists in %lu directories, expected exactly one\n", shared_in_dir + shared_in_alt);
        __atomic_fetch_add(&g_fs_stress_errors, 1u, __ATOMIC_SEQ_CST);
    }
    u64 listed = 0, listed_alt = 0;
    stellar::list(g_fs_stress_dir, &count_entry, &listed);
    stellar::list(g_fs_stress_alt, &count_entry, &listed_alt);
    if (listed != total + 1 + shared_in_dir || listed_alt != shared_in_alt) {
        serial::printf("[smp-fs] FAIL: directories hold %lu and %lu entries, expected %lu and %lu\n",
                        listed, listed_alt, total + 1 + shared_in_dir, shared_in_alt);
        __atomic_fetch_add(&g_fs_stress_errors, 1u, __ATOMIC_SEQ_CST);
    }
    for (u64 w = 0; w < FS_STRESS_WORKERS; ++w) {
        for (u64 j = 0; j < FS_STRESS_FILES; ++j) {
            char name[24];
            fs_stress_name(name, w, j);
            u64 star = stellar::find(g_fs_stress_dir, name);
            if (!file_equals(star, name) || !stellar::verify_file(star)) fs_stress_fail("file missing or corrupt", w, j);
            if (stellar::find(g_fs_stress_alt, name) != NONE) fs_stress_fail("file also present in the second directory", w, j);
        }
    }
    u64 shared_home = shared_in_dir ? g_fs_stress_dir : g_fs_stress_alt;
    u64 shared_star = stellar::find(shared_home, "shared");
    if (!file_equals(shared_star, "shared") || !stellar::verify_file(shared_star)) fs_stress_fail("contended file damaged", 0, 0);
    if (!stellar::unlink(shared_home, "shared")) fs_stress_fail("could not remove the contended file", 0, 0);
    for (u64 w = 0; w < FS_STRESS_WORKERS; ++w) {
        for (u64 j = 0; j < FS_STRESS_FILES; ++j) {
            char name[24];
            fs_stress_name(name, w, j);
            if (!stellar::unlink(g_fs_stress_dir, name)) fs_stress_fail("unlink failed", w, j);
        }
    }
    if (!stellar::unlink(g_fs_stress_dir, "alt")) fs_stress_fail("could not remove the second directory", 0, 0);
    if (!stellar::unlink(stellar::ROOT_STAR, "smpstress")) fs_stress_fail("could not remove the directory", 0, 0);
    stellar::CheckReport report{};
    if (!stellar::check(&report, false) || !report.ok()) {
        serial::writeln("[smp-fs] FAIL: check() found damage after the concurrent renames");
        __atomic_fetch_add(&g_fs_stress_errors, 1u, __ATOMIC_SEQ_CST);
    }
    u32 errors = g_fs_stress_errors;
    serial::printf("[smp-fs] %lu workers created %lu files in one directory, moved each to a second directory and back, contended on one file, then verified and removed them: %u error(s)\n",
                    FS_STRESS_WORKERS, total, errors);
    serial::printf("[selftest] smp-fs: %s\n", errors == 0 ? "ok" : "MISMATCH");
}

static void demo_fs_stress(void* arg) {
    u64 w = reinterpret_cast<u64>(arg);
    for (u64 j = 0; j < FS_STRESS_FILES; ++j) {
        char name[24];
        fs_stress_name(name, w, j);
        u64 star = stellar::find(g_fs_stress_dir, name);
        if (star == stellar::INVALID_STAR)
            star = stellar::create_file(g_fs_stress_dir, name, name, cstr_len(name));
        if (star == stellar::INVALID_STAR) fs_stress_fail("create failed", w, j);
        else if (!file_equals(star, name)) fs_stress_fail("readback mismatch", w, j);
        if ((j % 5) == 0) orbital::yield();
    }
    for (u64 j = 0; j < FS_STRESS_FILES; ++j) {
        using stellar::Status;
        char name[24];
        fs_stress_name(name, w, j);
        u64 star = stellar::find(g_fs_stress_dir, name);
        if (star == stellar::INVALID_STAR) { fs_stress_fail("file missing before the rename phase", w, j); continue; }
        Status why = Status::Ok;
        if (!stellar::rename(g_fs_stress_dir, name, g_fs_stress_alt, name, stellar::RENAME_NOREPLACE, &why)) {
            fs_stress_fail("move to the second directory failed", w, j);
            continue;
        }
        if (stellar::find(g_fs_stress_alt, name) != star || stellar::find(g_fs_stress_dir, name) != stellar::INVALID_STAR)
            fs_stress_fail("moved file is not where the rename left it", w, j);
        if (!stellar::rename(g_fs_stress_alt, name, g_fs_stress_dir, name, stellar::RENAME_NOREPLACE, &why))
            fs_stress_fail("move back failed", w, j);
        else if (stellar::find(g_fs_stress_dir, name) != star)
            fs_stress_fail("file did not come back", w, j);
        if ((j % 5) == 0) {
            u64 from = ((w + j) & 1) ? g_fs_stress_dir : g_fs_stress_alt;
            u64 to = from == g_fs_stress_dir ? g_fs_stress_alt : g_fs_stress_dir;
            if (!stellar::rename(from, "shared", to, "shared", stellar::RENAME_NOREPLACE, &why) && why != Status::NotFound)
                fs_stress_fail("contended rename returned an unexpected status", w, j);
        }
        if ((j % 5) == 2) orbital::yield();
    }
    if (__atomic_sub_fetch(&g_fs_stress_remaining, 1u, __ATOMIC_SEQ_CST) == 0) fs_stress_finish();
    orbital::exit_current();
}

constexpr u64 MM_WORKERS = 4;
constexpr u32 MM_OPERATIONS = 20000;
constexpr u32 MM_LIVE = 24;
constexpr u32 MM_PAGE_BLOCKS = 8;
static volatile u32 g_mm_remaining = MM_WORKERS;
static volatile u32 g_mm_errors = 0;
static volatile u64 g_mm_operations = 0;
static u64 g_mm_baseline_objects = 0;

struct MmLive { u8* p; u32 size; u8 tag; };
struct MmPages { u64 phys; int order; u8 tag; };

static u64 mm_next(u64& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

static void mm_fail(const char* why, u64 w, u32 op) {
    __atomic_fetch_add(&g_mm_errors, 1u, __ATOMIC_SEQ_CST);
    serial::printf("[smp-mm] FAIL: %s (worker %lu, operation %u)\n", why, w, op);
}

static u32 mm_size(u64& rng) {
    u64 r = mm_next(rng) % 1000;
    if (r < 600) return 1 + static_cast<u32>(mm_next(rng) % 128);
    if (r < 900) return 129 + static_cast<u32>(mm_next(rng) % 1920);
    if (r < 980) return 2049 + static_cast<u32>(mm_next(rng) % 18000);
    return 20001 + static_cast<u32>(mm_next(rng) % 110000);
}

static void mm_fill(const MmLive& l) {
    u32 n = l.size < 192 ? l.size : 192;
    for (u32 i = 0; i < n; ++i) l.p[i] = static_cast<u8>(l.tag + i);
    if (l.size > 192) l.p[l.size - 1] = l.tag;
}

static bool mm_intact(const MmLive& l) {
    u32 n = l.size < 192 ? l.size : 192;
    for (u32 i = 0; i < n; ++i) if (l.p[i] != static_cast<u8>(l.tag + i)) return false;
    return l.size <= 192 || l.p[l.size - 1] == l.tag;
}

static void mm_stamp(const MmPages& b) {
    u64 pages = 1ull << b.order;
    for (u64 i = 0; i < pages; ++i) {
        auto* page = static_cast<volatile u8*>(universe::to_virt(b.phys + i * universe::PAGE_SIZE));
        for (u32 k = 0; k < 64; ++k) page[k] = static_cast<u8>(b.tag + k + i);
    }
}

static bool mm_stamp_intact(const MmPages& b) {
    u64 pages = 1ull << b.order;
    for (u64 i = 0; i < pages; ++i) {
        auto* page = static_cast<volatile u8*>(universe::to_virt(b.phys + i * universe::PAGE_SIZE));
        for (u32 k = 0; k < 64; ++k) if (page[k] != static_cast<u8>(b.tag + k + i)) return false;
    }
    return true;
}

static void demo_mm_stress(void* arg) {
    u64 w = reinterpret_cast<u64>(arg);
    u64 rng = 0x9E3779B97F4A7C15ull * (w + 1);
    MmLive live[MM_LIVE];
    MmPages blocks[MM_PAGE_BLOCKS];
    u32 n = 0, pn = 0;
    for (u32 op = 0; op < MM_OPERATIONS; ++op) {
        u64 r = mm_next(rng) % 100;
        if (r < 12) {
            if (pn < MM_PAGE_BLOCKS) {
                int order = static_cast<int>(mm_next(rng) % 4);
                u64 phys = universe::alloc(order);
                if (phys) {
                    MmPages b{ phys, order, static_cast<u8>(w * 67 + op) };
                    mm_stamp(b);
                    blocks[pn++] = b;
                }
            }
        } else if (r < 24) {
            if (pn) {
                u32 j = static_cast<u32>(mm_next(rng) % pn);
                if (!mm_stamp_intact(blocks[j])) mm_fail("a physical block was overwritten by another owner", w, op);
                universe::free(blocks[j].phys, blocks[j].order);
                blocks[j] = blocks[--pn];
            }
        } else if (n == 0 || (n < MM_LIVE && r < 62)) {
            MmLive l;
            l.size = mm_size(rng);
            l.p = static_cast<u8*>(kheap::alloc(l.size));
            if (l.p) {
                l.tag = static_cast<u8>(w * 91 + op);
                if (reinterpret_cast<u64>(l.p) % kheap::GUARANTEED_ALIGNMENT) mm_fail("misaligned heap pointer", w, op);
                mm_fill(l);
                live[n++] = l;
            }
        } else {
            u32 j = static_cast<u32>(mm_next(rng) % n);
            if (!mm_intact(live[j])) mm_fail("a heap object was overwritten by another owner", w, op);
            kheap::free(live[j].p);
            live[j] = live[--n];
        }
        if ((op & 63) == 0) orbital::yield();
    }
    for (u32 i = 0; i < n; ++i) {
        if (!mm_intact(live[i])) mm_fail("a heap object was overwritten before release", w, MM_OPERATIONS);
        kheap::free(live[i].p);
    }
    for (u32 i = 0; i < pn; ++i) {
        if (!mm_stamp_intact(blocks[i])) mm_fail("a physical block was overwritten before release", w, MM_OPERATIONS);
        universe::free(blocks[i].phys, blocks[i].order);
    }
    __atomic_fetch_add(&g_mm_operations, static_cast<u64>(MM_OPERATIONS), __ATOMIC_SEQ_CST);
    if (__atomic_sub_fetch(&g_mm_remaining, 1u, __ATOMIC_SEQ_CST) == 0) {
        bool balanced = kheap::stats().live_objects == g_mm_baseline_objects && kheap::check();
        if (!balanced) __atomic_fetch_add(&g_mm_errors, 1u, __ATOMIC_SEQ_CST);
        u32 errors = g_mm_errors;
        serial::printf("[smp-mm] %lu workers ran %lu heap and page operations concurrently, heap %s: %u error(s)\n",
                        MM_WORKERS, g_mm_operations, balanced ? "balanced" : "UNBALANCED", errors);
        serial::printf("[selftest] smp-mm: %s\n", errors == 0 ? "ok" : "MISMATCH");
    }
    orbital::exit_current();
}

constexpr u64 BD_WORKERS = 4;
constexpr u64 BD_SECTORS = 4;
constexpr u32 BD_ROUNDS = 40;
static bool g_bd_enabled = false;
static u64 g_bd_base = 0;
static volatile u32 g_bd_remaining = BD_WORKERS;
static volatile u32 g_bd_errors = 0;

static void bd_fail(const char* why, u64 w, u32 round) {
    __atomic_fetch_add(&g_bd_errors, 1u, __ATOMIC_SEQ_CST);
    serial::printf("[smp-blockdev] FAIL: %s (worker %lu, round %u)\n", why, w, round);
}

static void demo_blockdev_stress(void* arg) {
    u64 w = reinterpret_cast<u64>(arg);
    u8 out[BD_SECTORS * 512];
    u8 in[BD_SECTORS * 512];
    u64 start = g_bd_base + w * BD_SECTORS;
    for (u32 round = 0; round < BD_ROUNDS; ++round) {
        for (u64 i = 0; i < sizeof(out); ++i)
            out[i] = static_cast<u8>(((i * 2654435761u) >> 24) ^ (w * 37) ^ (round * 11));
        if (!blockdev::write_sectors(start, BD_SECTORS, out)) { bd_fail("batched write failed", w, round); continue; }
        for (u64 i = 0; i < sizeof(in); ++i) in[i] = 0xA5;
        if (!blockdev::read_sectors(start, BD_SECTORS, in)) { bd_fail("batched read failed", w, round); continue; }
        bool same = true;
        for (u64 i = 0; same && i < sizeof(in); ++i) same = in[i] == out[i];
        if (!same) bd_fail("batched readback mismatch", w, round);

        u8 one_out[512], one_in[512];
        for (u64 i = 0; i < 512; ++i) one_out[i] = static_cast<u8>(i + w * 53 + round * 7);
        u64 sec = start + (round % BD_SECTORS);
        if (!blockdev::write_sector(sec, one_out) || !blockdev::read_sector(sec, one_in)) { bd_fail("single sector I/O failed", w, round); continue; }
        same = true;
        for (u64 i = 0; same && i < 512; ++i) same = one_in[i] == one_out[i];
        if (!same) bd_fail("single sector mismatch", w, round);
        if ((round % 8) == 0) orbital::yield();
    }
    if (__atomic_sub_fetch(&g_bd_remaining, 1u, __ATOMIC_SEQ_CST) == 0) {
        u32 errors = g_bd_errors;
        serial::printf("[smp-blockdev] %lu workers ran %u rounds of batched and single-sector I/O on private regions: %u error(s)\n",
                        BD_WORKERS, BD_ROUNDS, errors);
        serial::printf("[selftest] smp-blockdev: %s\n", errors == 0 ? "ok" : "MISMATCH");
    }
    orbital::exit_current();
}

void arm_fs_stress(u64 dir, u64 alt) {
    g_fs_stress_dir = dir;
    g_fs_stress_alt = alt;
    g_fs_stress_enabled = true;
}

void spawn_test_tasks() {
    orbital::spawn("alpha", &demo_cooperative, const_cast<char*>("alpha"));
    orbital::spawn("beta", &demo_cooperative_explicit_exit, const_cast<char*>("beta"));
    orbital::spawn("gamma-spinner", &demo_spinner, const_cast<char*>("gamma-spinner"));
    orbital::spawn("clock-test", &demo_clock, nullptr);
    for (u64 i = 0; i < WORKER_COUNT; ++i)
        orbital::spawn("worker", &demo_worker, reinterpret_cast<void*>(i));
    u64 fs_total = stellar::total_sectors();
    if (blockdev::present() && fs_total == 0) {
        serial::writeln("[boot] block-device stress skipped: no mounted Stellar volume, so the disk tail is not ours to write");
    } else if (blockdev::present() && blockdev::capacity_sectors() > 256) {
        if (fs_total + TEST_TAIL_SECTORS <= blockdev::capacity_sectors()) {
            g_bd_base = blockdev::capacity_sectors() - TEST_TAIL_SECTORS;
            g_bd_enabled = true;
            for (u64 i = 0; i < BD_WORKERS; ++i)
                orbital::spawn("bd-stress", &demo_blockdev_stress, reinterpret_cast<void*>(i));
        } else {
            serial::writeln("[boot] block-device stress skipped: the filesystem covers the last 64 sectors; run make reset-disk");
        }
    }
    if (g_fs_stress_enabled) {
        for (u64 i = 0; i < FS_STRESS_WORKERS; ++i)
            orbital::spawn("fs-stress", &demo_fs_stress, reinterpret_cast<void*>(i));
        fb::printf(0xC0FFC0, "[ok] %lu Satellites will hammer Stellar FS concurrently\n", FS_STRESS_WORKERS);
    }
    fb::printf(0xC0FFC0, "[ok] Orbital scheduler online -- 3 demo Satellites + %lu work-stealing workers spawned\n",
               WORKER_COUNT);
}

void spawn_mm_stress() {
    g_mm_baseline_objects = kheap::stats().live_objects;
    for (u64 i = 0; i < MM_WORKERS; ++i)
        orbital::spawn("mm-stress", &demo_mm_stress, reinterpret_cast<void*>(i));
}

}
