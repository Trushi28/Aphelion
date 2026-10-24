#include <cosmos/lens_text.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/serial.hpp>

namespace lens {

namespace {

struct Feeder {
    Domain* domain;
    Lens end;
    volatile u32 done;
};
Feeder g_feeder;

void feeder_main(void*) {
    orbital::sleep_ms(25);
    static const u8 word[2] = {'o', 'k'};
    (void)write(*g_feeder.domain, g_feeder.end, 0, ConstBytes(word, 2));
    (void)close(*g_feeder.domain, g_feeder.end);
    __atomic_store_n(&g_feeder.done, 1, __ATOMIC_SEQ_CST);
}

bool check(bool cond, const char* what, bool* ok) {
    if (!cond) {
        serial::printf("[lens] FAIL: %s\n", what);
        *ok = false;
    }
    return cond;
}

}

bool selftest() {
    bool ok = true;
    Domain* d = static_cast<Domain*>(kheap::alloc_zeroed(sizeof(Domain)));
    Domain* fd = static_cast<Domain*>(kheap::alloc_zeroed(sizeof(Domain)));
    if (!d || !fd) {
        if (d) kheap::free(d);
        if (fd) kheap::free(fd);
        serial::printf("[selftest] lens: MISMATCH\n");
        return false;
    }

    Lens boot = boot_root("ram");
    check(!boot.is_null() && sealed(), "boot roots are minted and sealed", &ok);
    check(mint(*d, "ram:/", Rights::all()).status() == Status::Denied, "no minting after seal", &ok);
    Result<Lens> root = delegate(boot_domain(), boot, *d, Rights::all());
    check(root.ok(), "delegate a root from the boot domain", &ok);
    Lens r = root.value();

    check(open_at(*d, r, "..", Right::Read).status() == Status::Escape, "'..' is refused", &ok);
    check(open_at(*d, r, "/x", Right::Read).status() == Status::Escape, "absolute path is refused", &ok);
    check(open_at(*d, r, "ram:/x", Right::Read).status() == Status::Escape, "realm prefix is refused", &ok);

    check(mkdir(*d, r, "lens-selftest").ok(), "mkdir", &ok);
    Result<Lens> file = create(*d, r, "lens-selftest/a.txt", Right::Read | Right::Write | Right::Append);
    check(file.ok(), "create", &ok);
    static const u8 hello[5] = {'h', 'e', 'l', 'l', 'o'};
    check(write(*d, file.value(), 0, ConstBytes(hello, 5)).ok() && sync(*d, file.value()).ok(), "write and sync", &ok);
    u8 back[8] = {};
    Result<usize> got = read(*d, file.value(), 0, Bytes(back, sizeof(back)));
    check(got.ok() && got.value() == 5 && back[0] == 'h' && back[4] == 'o', "read back", &ok);
    Result<Lens> ro = narrow(*d, file.value(), Right::Read);
    check(ro.ok() && write(*d, ro.value(), 0, ConstBytes(hello, 5)).status() == Status::Denied, "a narrowed lens cannot write", &ok);
    Result<Lens> dir = open_at(*d, r, "lens-selftest", Right::Read | Right::Enumerate);
    check(dir.ok() && open_at(*d, dir.value(), "a.txt", Right::Write).status() == Status::Denied, "a child cannot gain rights", &ok);
    nebula::Listing l;
    check(collect(*d, dir.value(), &l).ok() && l.count == 1, "enumerate sees one file", &ok);
    nebula::release(l);

    Lens old = file.value();
    check(close(*d, old).ok(), "close", &ok);
    check(stat(*d, old).status() == Status::StaleLens, "a closed lens is stale", &ok);
    Result<Lens> reuse = open_at(*d, r, "lens-selftest/a.txt", Right::Read);
    check(reuse.ok() && stat(*d, old).status() == Status::StaleLens, "reusing the slot does not revive it", &ok);

    Lens rd, wr;
    check(make_pipe(*d, 64, &rd, &wr).ok(), "make_pipe", &ok);
    Wait w{rd, Ready::Readable, {}};
    u64 t0 = clock::uptime_ms();
    Result<u32> quiet = await(*d, &w, 1, 30);
    u64 dt = clock::uptime_ms() - t0;
    check(quiet.status() == Status::TimedOut && dt >= 30 && dt < 500, "an idle pipe times out on schedule", &ok);

    Result<Lens> given = delegate(*d, wr, *fd, Right::Write | Right::Append);
    check(given.ok(), "delegate the write end", &ok);
    (void)close(*d, wr);
    g_feeder = Feeder{fd, given.value(), 0};
    check(orbital::spawn("lens-feeder", &feeder_main, nullptr) != nullptr, "spawn the writer", &ok);
    t0 = clock::uptime_ms();
    Result<u32> woke = await(*d, &w, 1, 3000);
    dt = clock::uptime_ms() - t0;
    check(woke.ok() && w.got.has(Ready::Readable) && dt < 1500, "a write from another task wakes the wait", &ok);
    u8 word[4] = {};
    Result<usize> pipe_got = read(*d, rd, 0, Bytes(word, sizeof(word)));
    check(pipe_got.ok() && pipe_got.value() == 2 && word[0] == 'o' && word[1] == 'k', "the bytes arrive in order", &ok);
    u64 end = clock::uptime_ms() + 2000;
    while (!__atomic_load_n(&g_feeder.done, __ATOMIC_SEQ_CST) && clock::uptime_ms() < end) orbital::sleep_ms(2);
    w.got = Readiness();
    Result<u32> gone = await(*d, &w, 1, 500);
    check(gone.ok() && w.got.has(Ready::Hangup), "the last writer leaving is a hang-up", &ok);

    check(remove_tree(*d, r, "lens-selftest").ok(), "clean up", &ok);
    close_all(*d);
    close_all(*fd);
    kheap::free(d);
    kheap::free(fd);
    serial::printf("[selftest] lens: %s\n", ok ? "ok" : "MISMATCH");
    return ok;
}

}
