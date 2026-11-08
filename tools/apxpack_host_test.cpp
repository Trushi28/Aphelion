#include "apxpack.hpp"
#include <stdio.h>
#include <stdlib.h>

using S = apxpack::Status;

static int g_failures = 0;

static void expect(bool cond, const char* what) {
    printf("%-84s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

static void w16(u8* p, u32 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
static void w32(u8* p, u32 v) { w16(p, v); w16(p + 2, v >> 16); }
static void w64(u8* p, u64 v) { w32(p, static_cast<u32>(v)); w32(p + 4, static_cast<u32>(v >> 32)); }

static void zero(void* p, usize n) {
    u8* b = static_cast<u8*>(p);
    for (usize i = 0; i < n; ++i) b[i] = 0;
}

static void copy(void* d, const void* s, usize n) {
    u8* x = static_cast<u8*>(d);
    const u8* y = static_cast<const u8*>(s);
    for (usize i = 0; i < n; ++i) x[i] = y[i];
}

static bool same(const void* a, const void* b, usize n) {
    const u8* x = static_cast<const u8*>(a);
    const u8* y = static_cast<const u8*>(b);
    for (usize i = 0; i < n; ++i) if (x[i] != y[i]) return false;
    return true;
}

constexpr u32 PH0 = 64, PH1 = 64 + 56, PH2 = 64 + 112;
constexpr u32 CODE_OFF = 0x1000, CODE_LEN = 0x120, DATA_OFF = 0x2000, DATA_LEN = 0x30, DATA_MEM = 0x1234;
constexpr u32 ELF_BYTES = DATA_OFF + DATA_LEN;

struct Elf {
    u8 b[0x4000];
    u32 size;
};

static void phdr(u8* p, u32 type, u32 flags, u64 off, u64 vaddr, u64 filesz, u64 memsz) {
    w32(p, type);
    w32(p + 4, flags);
    w64(p + 8, off);
    w64(p + 16, vaddr);
    w64(p + 24, vaddr);
    w64(p + 32, filesz);
    w64(p + 40, memsz);
    w64(p + 48, 0x1000);
}

static void build(Elf* e) {
    zero(e, sizeof(*e));
    u8* f = e->b;
    f[0] = 0x7F; f[1] = 'E'; f[2] = 'L'; f[3] = 'F';
    f[4] = 2; f[5] = 1; f[6] = 1;
    w16(f + 16, 2);
    w16(f + 18, 62);
    w32(f + 20, 1);
    w64(f + 24, userabi::CODE_BASE + 0x10);
    w64(f + 32, 64);
    w16(f + 52, 64);
    w16(f + 54, 56);
    w16(f + 56, 3);
    phdr(f + PH0, 1, 5, CODE_OFF, userabi::CODE_BASE, CODE_LEN, CODE_LEN);
    phdr(f + PH1, 1, 6, DATA_OFF, userabi::DATA_BASE, DATA_LEN, DATA_MEM);
    phdr(f + PH2, 0x6474E551, 6, 0, 0, 0, 0);
    for (u32 i = 0; i < CODE_LEN; ++i) f[CODE_OFF + i] = static_cast<u8>(0x90 + i * 7);
    for (u32 i = 0; i < DATA_LEN; ++i) f[DATA_OFF + i] = static_cast<u8>(0x40 + i * 3);
    e->size = ELF_BYTES;
}

static u8 g_out[apx::MAX_FILE_BYTES];

static S run(const Elf& e, usize size, usize* written, usize cap = sizeof(g_out)) { return apxpack::convert(e.b, size, g_out, cap, written); }

static void valid_tests() {
    static Elf e;
    build(&e);
    usize n = 0;
    expect(run(e, e.size, &n) == S::Ok, "a well-formed ELF converts");
    apx::Parsed p = apx::parse(g_out, n);
    expect(p.status == apx::Status::Ok, "the result is a valid APX image");
    expect(p.header.code_bytes == CODE_LEN && p.header.entry_offset == 0x10 && p.header.data_bytes == DATA_LEN && p.header.data_pages == 2,
           "code size, entry offset, data size and data pages come from the program headers");
    expect(same(g_out + p.header.code_offset, e.b + CODE_OFF, CODE_LEN) && same(g_out + p.header.data_offset, e.b + DATA_OFF, DATA_LEN),
           "code and initial data bytes are copied unchanged");
    expect(n == apx::pack_size(CODE_LEN, DATA_LEN), "the image is exactly as large as pack_size says");

    build(&e);
    w64(e.b + PH1 + 32, 0);
    w64(e.b + PH1 + 40, 0);
    expect(run(e, e.size, &n) == S::Ok && apx::parse(g_out, n).header.data_pages == 0 && apx::parse(g_out, n).header.data_bytes == 0,
           "a program with no data segment converts with no data pages");

    build(&e);
    w64(e.b + PH1 + 32, 0);
    expect(run(e, e.size, &n) == S::Ok && apx::parse(g_out, n).header.data_bytes == 0 && apx::parse(g_out, n).header.data_pages == 2,
           "a data segment that is all zero-fill keeps its pages and carries no bytes");

    build(&e);
    w64(e.b + PH1 + 40, 0x1000);
    expect(run(e, e.size, &n) == S::Ok && apx::parse(g_out, n).header.data_pages == 1, "data of exactly one page needs one page");
    w64(e.b + PH1 + 40, 0x1001);
    expect(run(e, e.size, &n) == S::Ok && apx::parse(g_out, n).header.data_pages == 2, "data of one page and a byte needs two");
    w64(e.b + PH1 + 40, 64ull * 4096);
    expect(run(e, e.size, &n) == S::Ok && apx::parse(g_out, n).header.data_pages == 64, "data of exactly 64 pages is accepted");
    w64(e.b + PH1 + 40, 64ull * 4096 + 1);
    expect(run(e, e.size, &n) == S::DataTooLarge, "data of 64 pages and a byte is refused");

    build(&e);
    w64(e.b + PH0 + 32, apx::MAX_CODE_BYTES);
    w64(e.b + PH0 + 40, apx::MAX_CODE_BYTES);
    w64(e.b + PH0 + 8, 0x1000);
    static Elf big;
    build(&big);
    expect(run(big, big.size, &n, 100) == S::PackFailed, "an output buffer that is too small is reported");
}

static void reject_tests() {
    static Elf e;
    struct Case { const char* what; u32 at; u64 value; u32 width; S want; };
    const Case cases[] = {
        { "bad magic", 0, 0x7E, 1, S::NotElf },
        { "ELF32", 4, 1, 1, S::NotElf64 },
        { "big-endian", 5, 2, 1, S::NotLittleEndian },
        { "relocatable object", 16, 1, 2, S::NotExecutable },
        { "shared object", 16, 3, 2, S::NotExecutable },
        { "ARM machine", 18, 40, 2, S::NotX86_64 },
        { "no program headers", 56, 0, 2, S::BadProgramHeaders },
        { "program header entry too small", 54, 40, 2, S::BadProgramHeaders },
        { "program headers outside the file", 32, 0x7FFFFFFF0000ull, 8, S::BadProgramHeaders },
        { "program header table that wraps", 32, 0xFFFFFFFFFFFFFFF0ull, 8, S::BadProgramHeaders },
        { "65 program headers", 56, 65, 2, S::BadProgramHeaders },
        { "a read-only segment", PH1 + 4, 4, 4, S::UnsupportedSegment },
        { "a read-write-execute segment", PH1 + 4, 7, 4, S::UnsupportedSegment },
        { "an execute-only segment", PH0 + 4, 1, 4, S::UnsupportedSegment },
        { "a second code segment", PH1 + 4, 5, 4, S::TwoCodeSegments },
        { "code at the wrong address", PH0 + 16, 0x401000, 8, S::CodeAddress },
        { "data at the wrong address", PH1 + 16, 0x801000, 8, S::DataAddress },
        { "code with zero-fill", PH0 + 40, CODE_LEN + 1, 8, S::CodeHasBss },
        { "code bytes outside the file", PH0 + 8, 0x3F00, 8, S::SegmentOutsideFile },
        { "code size that wraps", PH0 + 32, 0xFFFFFFFFFFFFFFF0ull, 8, S::CodeHasBss },
        { "data bytes outside the file", PH1 + 8, 0x3F00, 8, S::SegmentOutsideFile },
        { "data file size above memory size", PH1 + 32, DATA_MEM + 1, 8, S::BadProgramHeaders },
        { "entry below the code", 24, 0x3FFFFF, 8, S::EntryOutsideCode },
        { "entry at the end of the code", 24, 0x400000 + CODE_LEN, 8, S::EntryOutsideCode },
        { "entry in the data", 24, 0x800000, 8, S::EntryOutsideCode },
        { "an empty code segment", PH0 + 32, 0, 8, S::CodeHasBss },
    };
    bool all = true;
    for (const Case& c : cases) {
        build(&e);
        if (c.width == 1) e.b[c.at] = static_cast<u8>(c.value);
        else if (c.width == 2) w16(e.b + c.at, static_cast<u32>(c.value));
        else if (c.width == 4) w32(e.b + c.at, static_cast<u32>(c.value));
        else w64(e.b + c.at, c.value);
        usize n = 0;
        S got = run(e, e.size, &n);
        bool ok = got == c.want && n == 0;
        if (!ok) printf("    %s: got %s, wanted %s\n", c.what, apxpack::status_name(got), apxpack::status_name(c.want));
        all = all && ok;
    }
    expect(all, "twenty-six kinds of malformed ELF each produce the status that names the problem");

    build(&e);
    phdr(e.b + PH2, 1, 6, DATA_OFF, userabi::DATA_BASE, DATA_LEN, DATA_MEM);
    usize n = 0;
    expect(run(e, e.size, &n) == S::TwoDataSegments, "a second writable segment is refused");

    build(&e);
    w64(e.b + PH0 + 32, 0);
    w64(e.b + PH0 + 40, 0);
    expect(run(e, e.size, &n) == S::NoCode, "a file whose only code segment is empty has no code");

    build(&e);
    w32(e.b + PH0, 6);
    expect(run(e, e.size, &n) == S::NoCode, "a file with no loadable code segment has no code");

    build(&e);
    w64(e.b + PH0 + 32, apx::MAX_CODE_BYTES + 16);
    w64(e.b + PH0 + 40, apx::MAX_CODE_BYTES + 16);
    expect(run(e, e.size, &n) == S::CodeTooLarge || run(e, e.size, &n) == S::SegmentOutsideFile, "code above 64 pages is refused");
}

static void truncation_tests() {
    static Elf e;
    build(&e);
    bool none = true;
    usize n = 0;
    for (u32 len = 0; len < e.size; ++len) {
        if (run(e, len, &n) == S::Ok) none = false;
    }
    expect(none, "no prefix of the ELF is accepted");
    expect(run(e, e.size, &n) == S::Ok, "the whole file is accepted");
    static Elf padded;
    build(&padded);
    expect(run(padded, padded.size + 0x500, &n) == S::Ok, "bytes after the last segment are ignored");
}

static bool code_matches(const Elf& m, const apx::Header& h) {
    for (u32 i = 0; i < 3; ++i) {
        const u8* ph = m.b + PH0 + 56 * i;
        if (apxpack::rd32(ph) != 1 || (apxpack::rd32(ph + 4) & 7) != 5) continue;
        u64 off = apxpack::rd64(ph + 8);
        u64 filesz = apxpack::rd64(ph + 32);
        return filesz == h.code_bytes && off + filesz <= sizeof(m.b) && same(g_out + h.code_offset, m.b + off, filesz);
    }
    return false;
}

static void sweep_tests() {
    static Elf base;
    build(&base);
    static Elf m;
    u64 runs = 0, accepted = 0, violations = 0;
    for (u32 at = 0; at < PH2 + 56; ++at) {
        for (u32 v = 0; v < 256; ++v) {
            copy(&m, &base, sizeof(m));
            if (m.b[at] == v) continue;
            m.b[at] = static_cast<u8>(v);
            usize n = 0;
            S st = run(m, m.size, &n);
            ++runs;
            if (st != S::Ok) {
                if (n != 0) ++violations;
                continue;
            }
            ++accepted;
            apx::Parsed p = apx::parse(g_out, n);
            bool good = p.status == apx::Status::Ok && p.header.entry_offset < p.header.code_bytes && p.header.code_bytes <= apx::MAX_CODE_BYTES &&
                        p.header.data_pages <= userabi::MAX_DATA_PAGES && p.header.data_bytes <= p.header.data_pages * apx::PAGE &&
                        code_matches(m, p.header);
            if (!good) ++violations;
        }
    }
    expect(violations == 0, "every single-byte change to the headers is refused or yields a valid APX image");
    printf("    (%llu runs, %llu accepted)\n", static_cast<unsigned long long>(runs), static_cast<unsigned long long>(accepted));

    u64 rng = 0x9E3779B97F4A7C15ull;
    auto next = [&]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    violations = 0;
    u64 ok_runs = 0;
    for (u32 round = 0; round < 200000; ++round) {
        copy(&m, &base, sizeof(m));
        u32 edits = 1 + static_cast<u32>(next() % 4);
        for (u32 k = 0; k < edits; ++k) {
            u32 at = static_cast<u32>(next() % (PH2 + 56));
            m.b[at] = static_cast<u8>(next());
        }
        usize n = 0;
        S st = run(m, m.size, &n);
        if (st != S::Ok) {
            if (n != 0) ++violations;
            continue;
        }
        ++ok_runs;
        apx::Parsed p = apx::parse(g_out, n);
        if (p.status != apx::Status::Ok || p.header.entry_offset >= p.header.code_bytes) ++violations;
    }
    expect(violations == 0, "200,000 random multi-byte header rewrites never produce an invalid image");
    printf("    (%llu accepted)\n", static_cast<unsigned long long>(ok_runs));
}

static void real_tests(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        expect(false, "the real program's ELF file can be opened");
        return;
    }
    static u8 elf[1 << 20];
    usize n = fread(elf, 1, sizeof(elf), f);
    fclose(f);
    usize w = 0;
    S st = apxpack::convert(elf, n, g_out, sizeof(g_out), &w);
    expect(st == S::Ok, "a program built by the userland toolchain converts");
    if (st != S::Ok) return;
    apx::Parsed p = apx::parse(g_out, w);
    expect(p.status == apx::Status::Ok && p.header.entry_offset == 0, "the image is valid and starts at the first instruction of the code");
    u64 phoff = apxpack::rd64(elf + 32);
    u32 phnum = apxpack::rd16(elf + 56);
    bool code_same = false, data_same = false;
    for (u32 i = 0; i < phnum; ++i) {
        const u8* ph = elf + phoff + 56ull * i;
        if (apxpack::rd32(ph) != 1) continue;
        u64 off = apxpack::rd64(ph + 8);
        u64 filesz = apxpack::rd64(ph + 32);
        if ((apxpack::rd32(ph + 4) & 1) != 0) code_same = filesz == p.header.code_bytes && same(g_out + p.header.code_offset, elf + off, filesz);
        else data_same = filesz == p.header.data_bytes && same(g_out + p.header.data_offset, elf + off, filesz);
    }
    expect(code_same && data_same, "the real program's code and initial data are copied unchanged");
    expect(p.header.data_pages >= 1, "the real program's zero-initialised data has pages");
}

static u8 g_large[0x1000 + apx::MAX_CODE_BYTES + 0x2000];

static void boundary_tests() {
    static Elf e;
    usize n = 0;

    build(&e);
    bool magic = true;
    for (u32 i = 0; i < 4; ++i) {
        build(&e);
        e.b[i] ^= 0x20;
        if (run(e, e.size, &n) != S::NotElf) magic = false;
    }
    expect(magic, "each of the four magic bytes is checked on its own");

    build(&e);
    bool sizes = true;
    for (u32 len = 0; len < 64; ++len) sizes = sizes && run(e, len, &n) == S::TooSmall;
    expect(sizes && run(e, 64, &n) == S::BadProgramHeaders, "files under 64 bytes are too small and 64 bytes reaches the program header check");

    build(&e);
    w16(e.b + 56, 64);
    expect(run(e, e.size, &n) == S::Ok, "64 program headers are accepted");

    build(&e);
    phdr(e.b + 64 + 56 * 3, 1, 6, DATA_OFF, userabi::DATA_BASE, DATA_LEN, DATA_MEM);
    expect(run(e, e.size, &n) == S::Ok, "the entry after the last counted program header is not read");

    build(&e);
    phdr(e.b + PH2, 1, 6, 0, 0, 0, 0);
    expect(run(e, e.size, &n) == S::Ok, "an empty read-write load segment is skipped wherever it points");
    phdr(e.b + PH2, 1, 4, 0, 0x123000, 0, 0);
    expect(run(e, e.size, &n) == S::Ok, "an empty read-only load segment is skipped too");

    build(&e);
    w64(e.b + PH1 + 32, DATA_MEM);
    w64(e.b + PH1 + 8, 0x2000);
    w64(e.b + PH1 + 40, DATA_MEM);
    expect(run(e, e.size, &n) == S::SegmentOutsideFile || run(e, e.size, &n) == S::Ok, "data whose file size equals its memory size is not a size error");
    build(&e);
    w64(e.b + PH1 + 32, DATA_LEN);
    w64(e.b + PH1 + 40, DATA_LEN);
    expect(run(e, e.size, &n) == S::Ok && apx::parse(g_out, n).header.data_bytes == DATA_LEN, "data with no zero-fill converts");

    build(&e);
    w64(e.b + PH1 + 32, 0);
    w64(e.b + PH1 + 8, e.size);
    expect(run(e, e.size, &n) == S::Ok, "a zero-length data segment may start exactly at the end of the file");

    build(&e);
    usize full = 0;
    expect(run(e, e.size, &full) == S::Ok, "reference conversion");
    expect(run(e, e.size, &n, full) == S::Ok && n == full, "an output buffer of exactly the needed size is enough");
    expect(run(e, e.size, &n, full - 1) == S::PackFailed && n == 0, "one byte less is not");

    static Elf* none = nullptr;
    (void)none;
    for (u32 i = 0; i < sizeof(g_large); ++i) g_large[i] = 0;
    build(&e);
    for (u32 i = 0; i < 0x1000; ++i) g_large[i] = e.b[i];
    w64(g_large + PH0 + 8, 0x1000);
    w64(g_large + PH0 + 32, apx::MAX_CODE_BYTES);
    w64(g_large + PH0 + 40, apx::MAX_CODE_BYTES);
    w64(g_large + PH1 + 32, 0);
    w64(g_large + PH1 + 8, 0);
    expect(apxpack::convert(g_large, sizeof(g_large), g_out, sizeof(g_out), &n) == S::Ok && n == apx::pack_size(apx::MAX_CODE_BYTES, 0),
           "code of exactly 64 pages is accepted");
    w64(g_large + PH0 + 32, apx::MAX_CODE_BYTES + 16);
    w64(g_large + PH0 + 40, apx::MAX_CODE_BYTES + 16);
    expect(apxpack::convert(g_large, sizeof(g_large), g_out, sizeof(g_out), &n) == S::CodeTooLarge, "code of 64 pages and 16 bytes is refused as too large");
}

int main(int argc, char** argv) {
    valid_tests();
    reject_tests();
    truncation_tests();
    boundary_tests();
    sweep_tests();
    if (argc > 1) real_tests(argv[1]);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
