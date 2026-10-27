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

namespace boottest {

static constexpr const char* HELLO_MESSAGE = "Aphelion Stellar FS -- real file, real disk, real bytes.\n";
static constexpr const char* HELLO_REWRITTEN = "Aphelion Stellar FS -- rewritten after snapshot.\n";

struct PurgeList {
    char names[8][stellar::NAME_MAX_LEN + 1];
    u64 stars[8];
    u32 types[8];
    u32 n;
};

static void purge_collect(const char* name, u64 star, u32 type, void* ctx) {
    auto* p = static_cast<PurgeList*>(ctx);
    if (p->n >= 8) return;
    u32 i = 0;
    for (; name[i] && i < stellar::NAME_MAX_LEN; ++i) p->names[p->n][i] = name[i];
    p->names[p->n][i] = 0;
    p->stars[p->n] = star;
    p->types[p->n] = type;
    ++p->n;
}

static void purge_dir(u64 dir, u32 depth) {
    if (depth > 4) return;
    for (u32 pass = 0; pass < 64; ++pass) {
        PurgeList pl;
        pl.n = 0;
        stellar::list(dir, &purge_collect, &pl);
        if (pl.n == 0) return;
        for (u32 i = 0; i < pl.n; ++i) {
            if (pl.types[i] == stellar::TYPE_CONSTELLATION) purge_dir(pl.stars[i], depth + 1);
            stellar::unlink(dir, pl.names[i]);
        }
    }
}

static bool selftest_rename() {
    using stellar::Status;
    const u64 NONE = stellar::INVALID_STAR;
    const u64 root = stellar::ROOT_STAR;
    u64 rn = stellar::find(root, "rn-selftest");
    if (rn != NONE) purge_dir(rn, 0);
    else rn = stellar::create_constellation(root, "rn-selftest");
    if (rn == NONE) return false;

    Status why = Status::Ok;
    u8 sink[8];
    u64 f = stellar::create_file(rn, "a", "one", 3);
    u64 d1 = stellar::create_constellation(rn, "d1");
    u64 d2 = stellar::create_constellation(d1, "d2");
    bool ok = f != NONE && d1 != NONE && d2 != NONE;

    ok = ok && stellar::rename(rn, "a", d1, "b");
    ok = ok && stellar::find(d1, "b") == f && stellar::find(rn, "a") == NONE && file_equals(f, "one");

    u64 g = stellar::create_file(rn, "c", "two", 3);
    ok = ok && g != NONE && stellar::rename(d1, "b", rn, "c");
    ok = ok && stellar::find(rn, "c") == f && file_equals(f, "one") && stellar::read_file(g, sink, sizeof(sink)) == stellar::READ_ERROR;

    ok = ok && !stellar::rename(rn, "d1", d2, "x", 0, &why) && why == Status::WouldCycle;
    ok = ok && stellar::rename(d1, "d2", rn, "d2m") && stellar::find(rn, "d2m") == d2;
    ok = ok && !stellar::rename(rn, "c", rn, "d1", stellar::RENAME_NOREPLACE, &why) && why == Status::Exists;

    purge_dir(rn, 0);
    ok = ok && stellar::unlink(root, "rn-selftest");
    stellar::CheckReport rep{};
    return ok && stellar::check(&rep, false) && rep.ok();
}

static bool selftest_extents() {
    using stellar::Status;
    const u64 NONE = stellar::INVALID_STAR;
    const u64 root = stellar::ROOT_STAR;
    static u8 pat[40960], back[40960];
    u64 dir = stellar::find(root, "ext-selftest");
    if (dir != NONE) purge_dir(dir, 0);
    else dir = stellar::create_constellation(root, "ext-selftest");
    if (dir == NONE) return false;

    for (u64 i = 0; i < sizeof(pat); ++i) pat[i] = static_cast<u8>(i * 7 + (i >> 9));
    stellar::StatInfo si{};
    stellar::test_set_max_extent_sectors(4);
    u64 f = stellar::create_file(dir, "a", pat, sizeof(pat));
    stellar::test_set_max_extent_sectors(0);
    bool ok = f != NONE && stellar::stat(f, &si) && (si.flags & stellar::FLAG_EXTENT_TABLE) && si.extents == 20;
    ok = ok && stellar::read_file(f, back, sizeof(back)) == sizeof(pat) && mem::compare(pat, back, sizeof(pat)) == 0;
    ok = ok && stellar::verify_file(f);

    for (u64 i = 0; i < 30000; ++i) pat[i] = static_cast<u8>(i * 5 + 3);
    ok = ok && stellar::write_file(f, pat, 30000) == f && stellar::stat(f, &si) && !(si.flags & stellar::FLAG_EXTENT_TABLE) && si.extents == 1;
    ok = ok && stellar::read_file(f, back, sizeof(back)) == 30000 && mem::compare(pat, back, 30000) == 0;

    purge_dir(dir, 0);
    ok = ok && stellar::unlink(root, "ext-selftest");
    stellar::CheckReport rep{};
    return ok && stellar::check(&rep, false) && rep.ok();
}

static u64 basic_selftests(bool fresh_fs) {
    const char* msg = HELLO_MESSAGE;
    u64 msg_len = 0;
    while (msg[msg_len]) ++msg_len;

    u64 file = stellar::find(stellar::ROOT_STAR, "hello.txt");
    if (file == stellar::INVALID_STAR)
        file = stellar::create_file(stellar::ROOT_STAR, "hello.txt", msg, msg_len);

    u64 subdir = stellar::find(stellar::ROOT_STAR, "sub");
    if (subdir == stellar::INVALID_STAR)
        subdir = stellar::create_constellation(stellar::ROOT_STAR, "sub");
    const char* nested_msg = "nested constellation works\n";
    u64 nested_len = 0;
    while (nested_msg[nested_len]) ++nested_len;
    u64 nested_file = stellar::find(subdir, "nested.txt");
    if (nested_file == stellar::INVALID_STAR)
        nested_file = stellar::create_file(subdir, "nested.txt", nested_msg, nested_len);
    static u8 nested_readback[64];
    u64 nested_n = stellar::read_file(nested_file, nested_readback, sizeof(nested_readback) - 1);
    if (nested_n == stellar::READ_ERROR) nested_n = 0;
    nested_readback[nested_n] = 0;
    serial::printf("[stellar] /sub/nested.txt (%lu bytes): %s\n",
                    nested_n, reinterpret_cast<const char*>(nested_readback));

    {
        using stellar::Status;
        Status st = Status::Internal;
        u8 probe[16];
        bool resolve_ok = stellar::resolve("/sub/nested.txt", stellar::LIVE, &st) == nested_file && st == Status::Ok;
        bool missing_ok = stellar::find(stellar::ROOT_STAR, "no-such-file", stellar::LIVE, &st) == stellar::INVALID_STAR &&
                          st == Status::NotFound;
        bool dup_ok = stellar::create_file(stellar::ROOT_STAR, "hello.txt", "x", 1, &st) == stellar::INVALID_STAR &&
                      st == Status::Exists;
        bool name_ok = stellar::create_file(stellar::ROOT_STAR, "bad/name", "x", 1, &st) == stellar::INVALID_STAR &&
                       st == Status::InvalidName;
        bool dir_ok = stellar::read_file(subdir, probe, sizeof(probe), stellar::LIVE, &st) == stellar::READ_ERROR &&
                      st == Status::IsADirectory;
        bool api_ok = resolve_ok && missing_ok && dup_ok && name_ok && dir_ok;
        fb::printf(api_ok ? 0xC0FFC0 : 0xE0D080,
                   "[%s] Stellar FS API: path resolution, duplicate/invalid-name/not-found/is-a-directory status codes, %s\n",
                   api_ok ? "ok" : "--", api_ok ? "all as expected" : "MISMATCH");
        serial::printf("[selftest] stellar api: %s\n", api_ok ? "ok" : "MISMATCH");
    }

    {
        constexpr u64 PLAUSIBLE_MIN_MS = 1704067200000ull;
        u64 probe = stellar::find(subdir, "clock.probe");
        if (probe == stellar::INVALID_STAR)
            probe = stellar::create_file(subdir, "clock.probe", "t", 1);
        stellar::StatInfo probe_info{};
        bool probe_ok = probe != stellar::INVALID_STAR && stellar::write_file(probe, "t", 1) == probe &&
                        stellar::stat(probe, &probe_info);
        if (!clock::wall_valid()) {
            serial::printf("[selftest] clock mtime: skipped, no wall clock\n");
        } else {
            probe_ok = probe_ok && probe_info.mtime >= PLAUSIBLE_MIN_MS && probe_info.mtime >= clock::boot_wall_ms();
            char iso[21];
            civil::format_iso(civil::from_unix(probe_info.mtime / 1000), iso);
            serial::printf("[clock] /sub/clock.probe mtime %s (%lu ms)\n", iso, probe_info.mtime);
            serial::printf("[selftest] clock mtime: %s\n", probe_ok ? "ok" : "MISMATCH");
        }
    }

    {
        bool extents_ok = selftest_extents();
        fb::printf(extents_ok ? 0xC0FFC0 : 0xE0D080,
                   "[%s] Stellar FS extent tables: a 20-extent file written and read through the block driver, rewritten contiguous, %s\n",
                   extents_ok ? "ok" : "--", extents_ok ? "all as expected" : "MISMATCH");
        serial::printf("[selftest] extents: %s\n", extents_ok ? "ok" : "MISMATCH");
    }

    {
        bool rename_ok = selftest_rename();
        fb::printf(rename_ok ? 0xC0FFC0 : 0xE0D080,
                   "[%s] Stellar FS rename: move, replace, NOREPLACE and directory-cycle refusal, %s\n",
                   rename_ok ? "ok" : "--", rename_ok ? "all as expected" : "MISMATCH");
        serial::printf("[selftest] rename: %s\n", rename_ok ? "ok" : "MISMATCH");
    }

    stellar::BitmapSummary bs = stellar::bitmap_summary();
    serial::printf("[stellar] free-space bitmap: %lu KiB resident, %lu of %lu summary pages loaded so far (the rest load on first use), %lu sectors on the volume\n",
                    stellar::bitmap_ram_bytes() / 1024, bs.known_pages, bs.pages, stellar::total_sectors());

    static u8 readback[128];
    u64 n = stellar::read_file(file, readback, sizeof(readback) - 1);
    if (n == stellar::READ_ERROR) n = 0;
    readback[n] = 0;

    bool match = file_equals(file, msg) || file_equals(file, HELLO_REWRITTEN);

    fb::printf(match ? 0xC0FFC0 : 0xE0D080,
               "[%s] Stellar FS: %s /hello.txt, read %lu bytes back, %s\n",
               match ? "ok" : "--", fresh_fs ? "created" : "found", n, match ? "matched exactly" : "MISMATCH");
    serial::printf("[stellar] readback: %s\n", reinterpret_cast<const char*>(readback));
    serial::printf("[selftest] hello.txt: %s\n", match ? "ok" : "MISMATCH");
    if (virtioblk::present())
        serial::printf("[virtio-blk] completion mode: %s, %u interrupt(s) delivered\n",
                        virtioblk::using_msix() ? "MSI-X" : "polled", virtioblk::irq_count());
    if (ahci::present())
        serial::printf("[ahci] completion mode: %s, %u interrupt(s) delivered\n",
                        ahci::using_msi() ? "MSI" : "polled", ahci::irq_count());
    if (nvme::present())
        serial::printf("[nvme] completion mode: %s, %u interrupt(s) delivered\n",
                        nvme::completion_mode(), nvme::irq_count());

    return file;
}

static void snapshot_selftests(bool fresh_fs, u64 file) {
    const char* msg = HELLO_MESSAGE;
    if (fresh_fs) {
        const char* rewritten_msg = HELLO_REWRITTEN;
        u64 rewritten_len = cstr_len(rewritten_msg);

        bool cow_pre_ok = stellar::verify_file(file);
        stellar::IoStats io_before = stellar::io_stats();
        u64 snap = stellar::snapshot();
        stellar::IoStats io_after = stellar::io_stats();
        u64 snap_writes = io_after.writes - io_before.writes;
        u64 snap_reads = io_after.reads - io_before.reads;

        u64 rewritten_star = stellar::write_file(file, rewritten_msg, rewritten_len);
        bool cow_write_ok = (rewritten_star == file) && stellar::verify_file(file);
        bool cow_snap_ok = (snap != stellar::INVALID_STAR) && stellar::verify_file(file, snap);
        bool snap_content_ok = file_equals(file, msg, snap);
        bool new_content_ok = file_equals(file, rewritten_msg);
        bool cow_ok = cow_pre_ok && cow_write_ok && cow_snap_ok && snap_content_ok && new_content_ok;
        fb::printf(cow_ok ? 0xC0FFC0 : 0xE0D080,
                   "[%s] Stellar FS COW: snapshot %lu keeps the pre-write bytes, live view carries the rewrite, both checksums verify\n",
                   cow_ok ? "ok" : "--", snap);

        u64 live = stellar::create_constellation(stellar::ROOT_STAR, "tree");
        u64 live_a = stellar::create_file(live, "a.txt", "one\n", 4);
        u64 live_sub = stellar::create_constellation(live, "sub");
        u64 live_n = stellar::create_file(live_sub, "n.txt", "nested\n", 7);

        io_before = stellar::io_stats();
        u64 tree_snap = stellar::snapshot();
        io_after = stellar::io_stats();
        bool o1_ok = (io_after.writes - io_before.writes <= 6) && (io_after.reads - io_before.reads == 0) &&
                     snap_writes <= 6 && snap_reads == 0;

        stellar::write_file(live_a, "two two\n", 8);
        u64 live_b = stellar::create_file(live, "b.txt", "new\n", 4);

        bool tree_ok = tree_snap != stellar::INVALID_STAR &&
                       stellar::find(live, "a.txt", tree_snap) == live_a &&
                       file_equals(live_a, "one\n", tree_snap) && stellar::verify_file(live_a, tree_snap) &&
                       file_equals(live_a, "two two\n") && stellar::verify_file(live_a) &&
                       stellar::find(live, "sub", tree_snap) == live_sub &&
                       file_equals(live_n, "nested\n", tree_snap) &&
                       stellar::find(live, "b.txt", tree_snap) == stellar::INVALID_STAR &&
                       stellar::find(live, "b.txt") == live_b;
        fb::printf(tree_ok ? 0xC0FFC0 : 0xE0D080,
                   "[%s] Stellar FS tree snapshot %lu froze a.txt + sub/n.txt while the live tree diverged (rewrite + new file), %s\n",
                   tree_ok ? "ok" : "--", tree_snap, tree_ok ? "isolation confirmed" : "FAILED");
        fb::printf(o1_ok ? 0xC0FFC0 : 0xE0D080,
                   "[%s] Stellar FS snapshot cost: %lu sector write(s), %lu read(s) -- independent of tree size\n",
                   o1_ok ? "ok" : "--", io_after.writes - io_before.writes, io_after.reads - io_before.reads);
        serial::printf("[stellar] snapshots: %u in table, snapshot() cost %lu write(s) + %lu read(s) each (O(1), epoch-based)\n",
                        stellar::snapshot_count(), io_after.writes - io_before.writes, io_after.reads - io_before.reads);
        serial::printf("[selftest] COW and tree snapshots: %s\n", (cow_ok && tree_ok && o1_ok) ? "ok" : "MISMATCH");
    } else {
        serial::printf("[stellar] snapshot self-tests skipped on a mounted disk (%u/%u snapshot slots in use); delete disk.img to rerun them\n",
                        stellar::snapshot_count(), stellar::SNAPSHOT_MAX);
    }

}

static void bulk_selftest() {
    constexpr u64 BULK_BYTES = 8192;
    static u8 bulk_write[BULK_BYTES];
    static u8 bulk_read[BULK_BYTES];
    for (u64 i = 0; i < BULK_BYTES; ++i) bulk_write[i] = static_cast<u8>((i * 40503u) >> 8);
    u64 bulk = stellar::find(stellar::ROOT_STAR, "bulk.bin");
    if (bulk == stellar::INVALID_STAR)
        bulk = stellar::create_file(stellar::ROOT_STAR, "bulk.bin", bulk_write, BULK_BYTES);
    u64 bulk_n = stellar::read_file(bulk, bulk_read, BULK_BYTES);
    bool bulk_ok = (bulk_n == BULK_BYTES) && stellar::verify_file(bulk);
    for (u64 i = 0; bulk_ok && i < BULK_BYTES; ++i) bulk_ok = (bulk_read[i] == bulk_write[i]);
    fb::printf(bulk_ok ? 0xC0FFC0 : 0xE0D080,
               "[%s] Stellar FS bulk I/O: /bulk.bin %lu bytes through the batched block path, checksum verified, %s\n",
               bulk_ok ? "ok" : "--", bulk_n, bulk_ok ? "byte-for-byte match" : "MISMATCH");
    serial::printf("[selftest] bulk I/O: %s\n", bulk_ok ? "ok" : "MISMATCH");

}

static void directory_stress_selftest() {
    constexpr u64 STRESS_COUNT = 14;
    static u64 stress_ids[STRESS_COUNT];
    bool stress_create_ok = true;

    u64 stale_stress = stellar::find(stellar::ROOT_STAR, "smpstress");
    if (stale_stress != stellar::INVALID_STAR) {
        purge_dir(stale_stress, 0);
        stellar::unlink(stellar::ROOT_STAR, "smpstress");
        serial::writeln("[stellar] removed the smpstress directory left behind by an interrupted boot");
    }
    for (u64 i = 0; i < STRESS_COUNT; ++i) {
        char digits[20];
        u32 dlen = itoa_dec(i, digits);
        char name[32] = "stress";
        for (u32 j = 0; j < dlen; ++j) name[6 + j] = digits[j];
        name[6 + dlen] = 0;
        stress_ids[i] = stellar::find(stellar::ROOT_STAR, name);
        if (stress_ids[i] == stellar::INVALID_STAR)
            stress_ids[i] = stellar::create_file(stellar::ROOT_STAR, name, digits, dlen);
        if (stress_ids[i] == stellar::INVALID_STAR) stress_create_ok = false;
    }

    u64 stress_mismatches = 0;
    for (u64 i = 0; i < STRESS_COUNT; ++i) {
        char expect[20];
        u32 elen = itoa_dec(i, expect);
        u8 got[20];
        u64 n = stellar::read_file(stress_ids[i], got, sizeof(got) - 1);
        bool ok = (n == elen);
        for (u64 j = 0; ok && j < n; ++j) ok = (got[j] == static_cast<u8>(expect[j]));
        if (!ok) ++stress_mismatches;
    }

    u64 dir_count = 0;
    stellar::list(stellar::ROOT_STAR, &count_entry, &dir_count);
    u64 expected_dir_count = STRESS_COUNT;
    const char* const known_names[4] = { "hello.txt", "sub", "tree", "bulk.bin" };
    for (const char* known : known_names)
        if (stellar::find(stellar::ROOT_STAR, known) != stellar::INVALID_STAR) ++expected_dir_count;

    bool stress_ok = stress_create_ok && (stress_mismatches == 0) && (dir_count == expected_dir_count);
    if (stress_ok) {
        u64 stress_dir = stellar::find(stellar::ROOT_STAR, "smpstress");
        if (stress_dir == stellar::INVALID_STAR)
            stress_dir = stellar::create_constellation(stellar::ROOT_STAR, "smpstress");
        if (stress_dir != stellar::INVALID_STAR) {
            u64 stress_alt = stellar::create_constellation(stress_dir, "alt");
            u64 shared = stellar::create_file(stress_dir, "shared", "shared", 6);
            if (stress_alt != stellar::INVALID_STAR && shared != stellar::INVALID_STAR) arm_fs_stress(stress_dir, stress_alt);
        }
    }
    fb::printf(stress_ok ? 0xC0FFC0 : 0xE0D080,
               "[%s] Stellar FS B+tree + growable directory stress: %lu stars, %lu mismatch(es), %lu dir entries enumerated\n",
               stress_ok ? "ok" : "--", STRESS_COUNT, stress_mismatches, dir_count);
    serial::printf("[stellar] stress: %lu stars created (create_ok=%d), %lu mismatch(es), root dir enumerates %lu/%lu entries across its B+tree\n",
                    STRESS_COUNT, stress_create_ok ? 1 : 0, stress_mismatches, dir_count, expected_dir_count);
}

void run_fs_selftests(bool fresh_fs) {
    u64 file = basic_selftests(fresh_fs);
    snapshot_selftests(fresh_fs, file);
    bulk_selftest();
    directory_stress_selftest();
}

}
