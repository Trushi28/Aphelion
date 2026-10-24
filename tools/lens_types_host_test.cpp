#include <cosmos/lens_types.hpp>

extern "C" int printf(const char*, ...);

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, msg) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL: %s  (line %d)\n", msg, __LINE__); } } while (0)

using namespace lens;

static bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static void test_rights() {
    constexpr Rights rw = Right::Read | Right::Write;
    static_assert(rw.has(Right::Read) && rw.has(Right::Write) && !rw.has(Right::Append), "has");
    static_assert(Rights::none().empty(), "none is empty");
    static_assert(Rights::all().bits == 0x1FF, "nine rights");
    static_assert(Rights().bits == 0, "default is zero memory");
    static_assert(sizeof(Rights) == 2, "rights is two bytes");

    CHECK(rw.has_all(Right::Read), "has_all single");
    CHECK(rw.has_all(rw), "has_all self");
    CHECK(!rw.has_all(Right::Read | Right::Append), "has_all missing one");
    CHECK(rw.has_any(Right::Write | Right::Delegate), "has_any");
    CHECK(!rw.has_any(Right::Delegate), "has_any none");

    Rights r = Right::Read;
    CHECK(r.subset_of(rw), "read in read+write");
    CHECK(!rw.subset_of(r), "read+write not in read");
    CHECK(Rights::none().subset_of(Rights::none()), "empty in empty");
    CHECK(Rights::none().subset_of(r), "empty in anything");
    CHECK(rw.subset_of(Rights::all()), "everything in all");
    CHECK(Rights::all().subset_of(Rights::all()), "all in all");
    CHECK(!Rights::all().subset_of(rw), "all not in read+write");

    Rights a = Right::Read | Right::Write | Right::Append;
    Rights b = Right::Write | Right::Append | Right::Unlink;
    CHECK((a & b).subset_of(a) && (a & b).subset_of(b), "and is within both");
    CHECK(a.subset_of(a | b) && b.subset_of(a | b), "or contains both");
    CHECK((a & b) == (Right::Write | Right::Append), "and value");

    CHECK((~Rights::none()) == Rights::all(), "complement of none is all");
    CHECK((~Rights::all()).empty(), "complement of all is none");
    CHECK((a & ~a).empty(), "x and not x");
    CHECK(((~a).bits & ~RIGHTS_ALL_BITS) == 0, "complement masked");

    CHECK(Rights::from_bits(0xFFFF) == Rights::all(), "from_bits masks high bits");
    CHECK(Rights::from_bits(0xFE00).empty(), "only undefined bits gives none");

    u16 seen = 0;
    const Right every[RIGHT_COUNT] = {Right::Read, Right::Write, Right::Append, Right::Enumerate, Right::Create,
                                      Right::Unlink, Right::Rename, Right::Freeze, Right::Delegate};
    bool distinct = true;
    for (u32 i = 0; i < RIGHT_COUNT; ++i) {
        u16 v = static_cast<u16>(every[i]);
        if ((v & (v - 1)) != 0 || (seen & v)) distinct = false;
        seen |= v;
    }
    CHECK(distinct && seen == RIGHTS_ALL_BITS, "nine distinct single-bit rights covering the mask");

    Rights acc;
    acc |= Right::Read;
    acc |= Right::Delegate;
    CHECK(acc.has(Right::Read) && acc.has(Right::Delegate) && !acc.has(Right::Write), "|= accumulates");
    acc &= Right::Read;
    CHECK(acc == Rights(Right::Read), "&= narrows");

    char t[16];
    CHECK(rights_text(Rights::all(), t, sizeof(t)) == 9 && same(t, "rwaecunfd"), "text of all");
    CHECK(rights_text(Rights::none(), t, sizeof(t)) == 9 && same(t, "---------"), "text of none");
    rights_text(Right::Read | Right::Enumerate, t, sizeof(t));
    CHECK(same(t, "r--e-----"), "text of read+enumerate");
    char small[4];
    CHECK(rights_text(Rights::all(), small, sizeof(small)) == 9 && same(small, "rwa"), "text truncates and terminates");
    CHECK(RIGHTS_READ_ONLY.subset_of(RIGHTS_READ_WRITE), "read-only bundle inside read-write bundle");
    CHECK(!RIGHTS_READ_WRITE.has(Right::Delegate) && !RIGHTS_READ_WRITE.has(Right::Freeze), "bundles never carry delegate or freeze");
}

static Result<int> half(int v) {
    if (v & 1) return Result<int>::fail(Status::InvalidName);
    return Result<int>::success(v / 2);
}

static Done maybe(bool good) { return good ? Done::success() : Done::fail(Status::Denied); }

struct Big { u64 a, b; };

static void test_result() {
    Result<int> d;
    CHECK(d.ok() && d.value() == 0 && d.status() == Status::Ok, "default result is ok and zero");

    Result<int> ok = half(10);
    CHECK(ok.ok() && static_cast<bool>(ok) && ok.value() == 5, "success carries value");
    Result<int> bad = half(7);
    CHECK(!bad.ok() && !static_cast<bool>(bad) && bad.status() == Status::InvalidName, "failure carries status");
    CHECK(bad.value() == 0, "failed result holds zero value");
    CHECK(bad.value_or(42) == 42 && ok.value_or(42) == 5, "value_or");

    CHECK(maybe(true).ok(), "void success");
    Done f = maybe(false);
    CHECK(!f.ok() && f.status() == Status::Denied, "void failure");

    Result<Big> big = Result<Big>::success(Big{1, 2});
    CHECK(big.ok() && big.value().a == 1 && big.value().b == 2, "struct payload");
    big.value().a = 9;
    CHECK(big.value().a == 9, "mutable value");

    Result<const char*> s = Result<const char*>::success("hi");
    CHECK(s.ok() && same(s.value(), "hi"), "pointer payload");

    CHECK(static_cast<i32>(Status::Ok) == 0, "Ok is zero");
    CHECK(same(status_name(Status::Denied), "lens lacks the right"), "status name");
    CHECK(same(status_name(Status::StaleLens), "stale lens"), "stale name");
    CHECK(same(status_name(Status::Cancelled), "cancelled"), "last status has a name");

    bool named = true;
    for (i32 i = 0; i <= static_cast<i32>(Status::Cancelled); ++i) {
        const char* n = status_name(static_cast<Status>(i));
        if (same(n, "unknown")) named = false;
        for (i32 j = 0; j < i; ++j)
            if (same(n, status_name(static_cast<Status>(j)))) named = false;
    }
    CHECK(named, "all statuses named, none repeated");
    CHECK(same(status_name(static_cast<Status>(999)), "unknown"), "out of range is unknown");

    static_assert(sizeof(Result<void>) == sizeof(Status), "void result is just a status");
}

static void test_span() {
    u8 buf[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    Bytes s(buf, 8);
    CHECK(s.size() == 8 && !s.empty() && s[3] == 3, "basic span");
    CHECK(Bytes().empty() && Bytes().data() == nullptr, "default span is empty");

    Bytes mid = s.subspan(2, 3);
    CHECK(mid.size() == 3 && mid[0] == 2 && mid[2] == 4, "subspan");
    CHECK(s.subspan(6).size() == 2 && s.subspan(6)[0] == 6, "subspan to end");
    CHECK(s.subspan(8).size() == 0, "subspan at end is empty");
    CHECK(s.subspan(100).size() == 0, "subspan past end is empty and clamped");
    CHECK(s.subspan(100).data() == buf + 8, "past-end subspan points at end, not beyond");
    CHECK(s.subspan(5, 100).size() == 3, "length clamps to remainder");
    CHECK(s.subspan(0, ~static_cast<usize>(0)).size() == 8, "huge length does not overflow");
    CHECK(s.first(3).size() == 3 && s.first(99).size() == 8, "first clamps");

    u32 sum = 0;
    for (u8 b : s) sum += b;
    CHECK(sum == 28, "range-for");

    ConstBytes c = s;
    CHECK(c.size() == 8 && c.data() == buf, "mutable to const");
    static_assert(__is_convertible(Bytes, ConstBytes), "Bytes -> ConstBytes");
    static_assert(!__is_convertible(ConstBytes, Bytes), "ConstBytes -/-> Bytes");

    u8 dst[4] = {};
    usize n = copy_bytes(Bytes(dst, 4), ConstBytes(buf, 8));
    CHECK(n == 4 && dst[3] == 3, "copy clamps to destination");
    u8 dst2[16] = {};
    n = copy_bytes(Bytes(dst2, 16), ConstBytes(buf, 8));
    CHECK(n == 8 && dst2[7] == 7 && dst2[8] == 0, "copy clamps to source");
    CHECK(copy_bytes(Bytes(), c) == 0, "copy into empty");

    ConstBytes t = text_bytes("abc");
    CHECK(t.size() == 3 && t[0] == 'a' && t[2] == 'c', "text bytes");
    CHECK(text_bytes("").size() == 0, "empty text");

    u32 words[2] = {0x04030201, 0x08070605};
    ConstBytes raw = as_bytes(words, sizeof(words));
    CHECK(raw.size() == 8 && raw[0] == 1 && raw[7] == 8, "as_bytes over typed memory");

    Span<u32> ws = span_of(words, 2);
    CHECK(ws.size() == 2 && ws[1] == 0x08070605, "typed span");
    static_assert(sizeof(Bytes) == 2 * sizeof(void*), "span is pointer plus length");
}

static void test_epoch() {
    Epoch none;
    CHECK(!none.valid() && none.raw() == 0 && none == Epoch::none(), "default epoch is none");
    Epoch e(7);
    CHECK(e.valid() && e.raw() == 7, "epoch carries snapshot id");
    CHECK(e == Epoch(7) && !(e == Epoch(8)), "equality");
    CHECK(Epoch(3) < Epoch(4) && !(Epoch(4) < Epoch(4)), "ordering");

    static_assert(!__is_convertible(u64, Epoch), "integer -/-> Epoch");
    static_assert(!__is_convertible(Epoch, u64), "Epoch -/-> integer");
    static_assert(!__is_convertible(Epoch, bool), "Epoch -/-> bool");
    static_assert(sizeof(Epoch) == sizeof(u64), "epoch is one word");
}

static void test_zero_memory() {

    alignas(8) u8 raw[64] = {};
    Rights* r = reinterpret_cast<Rights*>(raw);
    CHECK(r->empty(), "zero memory is no rights");
    Epoch* e = reinterpret_cast<Epoch*>(raw);
    CHECK(!e->valid(), "zero memory is no epoch");
    Result<u64>* res = reinterpret_cast<Result<u64>*>(raw);
    CHECK(res->ok() && res->value() == 0, "zero memory is ok zero");
    Bytes* sp = reinterpret_cast<Bytes*>(raw);
    CHECK(sp->empty() && sp->data() == nullptr, "zero memory is empty span");
}

int main() {
    printf("-- rights\n");
    test_rights();
    printf("-- result\n");
    test_result();
    printf("-- span\n");
    test_span();
    printf("-- epoch\n");
    test_epoch();
    printf("-- zero memory\n");
    test_zero_memory();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
