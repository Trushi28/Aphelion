#include <cosmos/types.hpp>
#include <cosmos/timemath.hpp>
#include <cosmos/runq.hpp>
#include <cosmos/mlfq.hpp>
#include "../kernel/src/boot/bootcfg.cpp"
#include <stdio.h>

static int g_failures = 0;

static void expect(bool cond, const char* what) {
    printf("%-62s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

static void bootcfg_tests() {
    using namespace bootcfg;
    Config c = parse(nullptr);
    expect(c.mode == Mode::Test && c.storage == Storage::Auto && c.timer == TimerPref::Auto && c.rejected == 0,
           "bootcfg: no command line means test mode with storage on");
    c = parse("");
    expect(c.mode == Mode::Test && c.storage == Storage::Auto, "bootcfg: an empty command line is the same");
    c = parse("mode=shell");
    expect(c.mode == Mode::Shell && c.storage == Storage::Off, "bootcfg: shell mode switches storage off by default");
    c = parse("mode=shell storage=auto");
    expect(c.mode == Mode::Shell && c.storage == Storage::Auto, "bootcfg: storage=auto turns it back on in shell mode");
    c = parse("storage=auto mode=shell");
    expect(c.mode == Mode::Shell && c.storage == Storage::Auto, "bootcfg: word order does not matter");
    c = parse("mode=test storage=off");
    expect(c.mode == Mode::Test && c.storage == Storage::Off, "bootcfg: test mode can run without storage");
    c = parse("  mode=shell \t storage=off\ttimer=pit \n");
    expect(c.mode == Mode::Shell && c.storage == Storage::Off && c.timer == TimerPref::Pit && c.rejected == 0,
           "bootcfg: any run of blanks separates words");
    c = parse("mode=shell storage=usb");
    expect(c.mode == Mode::Shell && c.storage == Storage::Usb && c.rejected == 0, "bootcfg: storage=usb selects the boot-stick window");
    c = parse("storage=usb mode=test");
    expect(c.mode == Mode::Test && c.storage == Storage::Usb, "bootcfg: storage=usb is accepted in either mode");
    c = parse("mode=shell storage=usbx");
    expect(c.storage == Storage::Off && c.rejected == 1, "bootcfg: usb must match exactly and falls back to the safe default");
    expect(__builtin_strcmp(name(Storage::Usb), "usb") == 0 && __builtin_strcmp(name(Storage::Off), "off") == 0 && __builtin_strcmp(name(Storage::Auto), "auto") == 0,
           "bootcfg: storage names round-trip");
    c = parse("timer=hpet");
    expect(c.timer == TimerPref::Hpet, "bootcfg: timer=hpet");
    c = parse("timer=pm");
    expect(c.timer == TimerPref::Pm, "bootcfg: timer=pm");
    c = parse("mode=bogus");
    expect(c.mode == Mode::Test && c.rejected == 1, "bootcfg: an unknown mode is rejected and the default stays");
    c = parse("mode=shell storage=maybe");
    expect(c.storage == Storage::Off && c.rejected == 1, "bootcfg: an invalid storage value leaves the safe shell default");
    c = parse("quiet splash mode=shell");
    expect(c.mode == Mode::Shell && c.rejected == 2, "bootcfg: words without a value are counted and skipped");
    c = parse("modeshell=1 mode=shell=2 =x mode=");
    expect(c.mode == Mode::Test && c.rejected == 4, "bootcfg: malformed words never change the configuration");
    c = parse("mode=shellx");
    expect(c.mode == Mode::Test && c.rejected == 1, "bootcfg: a value must match exactly, not as a prefix");
    char longword[200];
    for (int i = 0; i < 199; ++i) longword[i] = 'a';
    longword[199] = 0;
    c = parse(longword);
    expect(c.rejected == 1, "bootcfg: an over-long word is rejected without overflowing the message buffer");
    expect(__builtin_strcmp(name(Mode::Shell), "shell") == 0 && __builtin_strcmp(name(Storage::Off), "off") == 0 &&
               __builtin_strcmp(name(TimerPref::Pm), "pm") == 0,
           "bootcfg: names round-trip");
}

static void timemath_tests() {
    using namespace timemath;
    expect(hpet_ticks(10, 10000000) == 1000000, "timemath: a 100 MHz HPET counts 1,000,000 ticks in 10 ms");
    expect(hpet_ticks(10, 69841279) == 143181, "timemath: a 14.318 MHz HPET counts about 143,181 ticks in 10 ms");
    expect(hpet_ticks(10, 0) == 0, "timemath: a zero period gives no ticks instead of dividing by zero");
    expect(pm_ticks(10) == 35795, "timemath: the ACPI PM timer counts 35,795 ticks in 10 ms");
    expect(pit_ticks(10) == 11931, "timemath: the PIT counts 11,931 ticks in 10 ms");
    expect(elapsed(0xFFFFF0, 0x10, 0xFFFFFF) == 0x20, "timemath: a 24-bit counter wrap is handled");
    expect(elapsed(0xFFFFFFF0u, 0x10, 0xFFFFFFFFu) == 0x20, "timemath: a 32-bit counter wrap is handled");
    expect(elapsed(~0ull - 5, 4, ~0ull) == 10, "timemath: a 64-bit counter wrap is handled");
    expect(elapsed(100, 100, 0xFFFF) == 0, "timemath: no time passed is zero");
    expect(counter_mask(true, 32) == ~0ull && counter_mask(false, 32) == 0xFFFFFFFFull && counter_mask(false, 24) == 0xFFFFFF,
           "timemath: counter masks");
    expect(within_percent(100, 110, 10) && !within_percent(100, 112, 10), "timemath: the agreement test measures against the larger value");
    expect(within_percent(110, 100, 10) && !within_percent(112, 100, 10), "timemath: the agreement test does not depend on order");
    expect(within_percent(0, 0, 25), "timemath: two zeroes agree");
    expect(plausible_hpet_period(10000000) && !plausible_hpet_period(0) && !plausible_hpet_period(100000001),
           "timemath: HPET periods outside the specification are rejected");
    expect(khz_from(30000000, 10) == 3000000 && khz_from(5, 0) == 0, "timemath: kHz from ticks and window");
}


struct Task : mlfq::Account {
    Task* next = nullptr;
    int id = 0;
};

using Queue = runq::Queue<Task, mlfq::RINGS>;

static Task make_task(int id, int ring) {
    Task t;
    t.id = id;
    mlfq::enter(t, ring);
    return t;
}

static void runq_tests() {
    Queue q;
    expect(q.empty() && q.pop_highest() == nullptr && q.top_ring() == mlfq::RINGS, "runq: a new queue is empty");
    Task a = make_task(1, 2), b = make_task(2, 0), c = make_task(3, 2), d = make_task(4, 3), e = make_task(5, 0);
    q.push(&a); q.push(&b); q.push(&c); q.push(&d); q.push(&e);
    expect(q.size() == 5 && q.top_ring() == 0 && q.size_of(0) == 2 && q.size_of(2) == 2 && q.size_of(3) == 1, "runq: counts per ring");
    expect(q.pop_highest() == &b && q.pop_highest() == &e, "runq: the highest ring first, first in first out within a ring");
    expect(q.pop_highest() == &a && q.pop_highest() == &c && q.pop_highest() == &d && q.empty(), "runq: lower rings follow in order");

    Task x = make_task(6, 1), y = make_task(7, 1), z = make_task(8, 1);
    q.push(&x); q.push(&y); q.push(&z);
    expect(q.remove(&y) && q.size() == 2 && !q.contains(&y), "runq: remove from the middle");
    expect(q.remove(&z) && q.size() == 1 && q.pop_highest() == &x, "runq: remove the tail keeps the list intact");
    expect(!q.remove(&x) && q.empty(), "runq: removing something that is not queued does nothing");
    q.push(&x); q.push(&y);
    expect(q.remove(&x) && q.pop_highest() == &y && q.empty(), "runq: remove the head");
    q.push(&x);
    q.push(&z);
    expect(q.pop_highest() == &x && q.pop_highest() == &z, "runq: reuse after removals keeps the order");

    Task out_of_range = make_task(9, 0);
    out_of_range.ring = 99;
    q.push(&out_of_range);
    expect(out_of_range.ring == mlfq::RINGS - 1 && q.top_ring() == mlfq::RINGS - 1, "runq: an invalid ring is clamped to the last one");
    q.pop_highest();
    out_of_range.ring = -4;
    q.push(&out_of_range);
    expect(out_of_range.ring == 0, "runq: a negative ring is clamped to the first one");
    q.pop_highest();

    Task t1 = make_task(10, 0), t2 = make_task(11, 1), t3 = make_task(12, 2), t4 = make_task(13, 3), t5 = make_task(14, 3);
    q.push(&t1); q.push(&t2); q.push(&t3); q.push(&t4); q.push(&t5);
    int moved = 0;
    u32 n = q.boost([&](Task* t) { mlfq::on_boost(*t); ++moved; });
    expect(n == 4 && moved == 4 && q.size() == 5 && q.size_of(0) == 5, "runq: boost moves every lower ring to the first");
    expect(q.pop_highest() == &t1 && q.pop_highest() == &t2 && q.pop_highest() == &t3 && q.pop_highest() == &t4 && q.pop_highest() == &t5,
           "runq: boosted tasks queue behind the ones already on the first ring, in ring order");
    expect(t4.ring == 0 && t4.allot_left == mlfq::ALLOTMENT[0] && t4.slice_left == mlfq::SLICE[0], "runq: boost restores the first ring's slice and allotment");

    Task only = make_task(15, 3);
    q.push(&only);
    expect(q.boost([](Task*) {}) == 1 && only.ring == 0 && q.size_of(0) == 1, "runq: boost with only the last ring populated");
    q.pop_highest();
    expect(q.boost([](Task*) {}) == 0, "runq: boost of an empty queue moves nothing");

    static Task many[1000];
    Queue big;
    unsigned long long seed = 12345;
    for (int i = 0; i < 1000; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        many[i] = make_task(i, static_cast<int>((seed >> 33) % mlfq::RINGS));
        big.push(&many[i]);
    }
    bool sorted = true;
    int last_ring = 0;
    u32 popped = 0;
    while (Task* t = big.pop_highest()) {
        if (t->ring < last_ring) sorted = false;
        last_ring = t->ring;
        ++popped;
    }
    expect(sorted && popped == 1000 && big.empty(), "runq: 1000 tasks drain in ring order");
}

static void mlfq_tests() {
    using namespace mlfq;
    Account a;
    enter(a, 0);
    u32 ticks = 0;
    int demotions = 0;
    while (a.ring == 0 && ticks < 100) {
        Verdict v = on_tick(a);
        ++ticks;
        if (v == Verdict::Demote) ++demotions;
    }
    expect(ticks == ALLOTMENT[0] && demotions == 1 && a.ring == 1, "mlfq: a task that never blocks is demoted after exactly its allotment");
    expect(a.slice_left == SLICE[1] && a.allot_left == ALLOTMENT[1], "mlfq: demotion starts a fresh slice and allotment");

    enter(a, 0);
    u32 rotations = 0;
    for (u32 i = 0; i < ALLOTMENT[0] - 1; ++i)
        if (on_tick(a) == Verdict::Rotate) ++rotations;
    expect(a.ring == 0 && rotations == ALLOTMENT[0] / SLICE[0] - 1, "mlfq: slices rotate within a ring until the allotment is gone");

    enter(a, 0);
    u32 total = 0;
    while (a.ring < RINGS - 1 && total < 1000) { on_tick(a); ++total; }
    expect(total == ALLOTMENT[0] + ALLOTMENT[1] + ALLOTMENT[2], "mlfq: the path down costs the sum of the allotments");
    Verdict last = Verdict::Continue;
    int rotates = 0, demotes = 0;
    for (int i = 0; i < 1000; ++i) {
        last = on_tick(a);
        if (last == Verdict::Rotate) ++rotates;
        if (last == Verdict::Demote) ++demotes;
    }
    expect(a.ring == RINGS - 1 && demotes == 0 && rotates == 1000 / static_cast<int>(SLICE[RINGS - 1]), "mlfq: the last ring only rotates");

    enter(a, 0);
    for (int i = 0; i < 5; ++i) on_tick(a);
    u32 spent = ALLOTMENT[0] - a.allot_left;
    dispatch(a);
    expect(spent == 5 && a.slice_left == SLICE[0] && a.allot_left == ALLOTMENT[0] - 5, "mlfq: being dispatched again refreshes the slice but not the allotment");

    enter(a, 2);
    on_wake(a, WAKE_PROMOTE_TICKS - 1);
    expect(a.ring == 2, "mlfq: a short sleep earns nothing");
    on_wake(a, WAKE_PROMOTE_TICKS);
    expect(a.ring == 1 && a.allot_left == ALLOTMENT[1], "mlfq: a longer sleep promotes one ring only");
    enter(a, 0);
    on_wake(a, 1000);
    expect(a.ring == 0, "mlfq: nothing is above the first ring");

    int gamer_ring = 0;
    Account g;
    enter(g, 0);
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (int i = 0; i < 3 && g.ring < RINGS - 1; ++i) on_tick(g);
        on_wake(g, 1);
        gamer_ring = g.ring;
    }
    expect(gamer_ring == RINGS - 1, "mlfq: briefly sleeping between bursts does not keep a hog on the top ring");

    expect(preempts(0, 1) && preempts(1, 3) && !preempts(2, 2) && !preempts(3, 0), "mlfq: only a strictly higher ring preempts");
    pin_lowest(a);
    expect(a.ring == RINGS - 1 && a.allot_left == 0, "mlfq: pinning to the lowest ring");
    expect(clamp_ring(-1) == 0 && clamp_ring(RINGS) == RINGS - 1 && clamp_ring(2) == 2, "mlfq: ring clamping");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bootcfg_tests();
    timemath_tests();
    runq_tests();
    mlfq_tests();
    return g_failures ? 1 : 0;
}
