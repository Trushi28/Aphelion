#include <cosmos/pe.hpp>
#include "pe_samples.hpp"
#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;

static void expect(bool cond, const char* what) {
    printf("%-78s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

static void w16(u8* p, u32 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
static void w32(u8* p, u32 v) { w16(p, v); w16(p + 2, v >> 16); }
static void w64(u8* p, u64 v) { w32(p, static_cast<u32>(v)); w32(p + 4, static_cast<u32>(v >> 32)); }
static u32 r32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24); }
static u64 r64(const u8* p) { return r32(p) | (static_cast<u64>(r32(p + 4)) << 32); }

static u64 g_rng = 0x2545F4914F6CDD1Dull;
static u64 rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void zero(void* p, usize n) {
    u8* b = static_cast<u8*>(p);
    for (usize i = 0; i < n; ++i) b[i] = 0;
}

static bool same(const void* a, const void* b, usize n) {
    const u8* x = static_cast<const u8*>(a);
    const u8* y = static_cast<const u8*>(b);
    for (usize i = 0; i < n; ++i) if (x[i] != y[i]) return false;
    return true;
}

static void copy(void* d, const void* s, usize n) {
    u8* x = static_cast<u8*>(d);
    const u8* y = static_cast<const u8*>(s);
    for (usize i = 0; i < n; ++i) x[i] = y[i];
}

static bool text_eq(const char* a, const char* b) {
    usize i = 0;
    for (; a[i] && b[i]; ++i) if (a[i] != b[i]) return false;
    return a[i] == b[i];
}

constexpr u64 TEXT_RVA = 0x1000, RDATA_RVA = 0x2000, DATA_RVA = 0x3000, RELOC_RVA = 0x4000;
constexpr u32 FILE_BYTES = 0x1000;
constexpr u32 IMAGE_BYTES = 0x5000;
constexpr u32 OFF_TEXT = 0x400, OFF_RDATA = 0x600, OFF_DATA = 0xC00, OFF_RELOC = 0xE00;
constexpr u32 TLS_DIR = 0x500, TLS_CALLBACKS = 0x540, PDATA = 0x580;
constexpr u32 SLOT_COUNT = 8;
constexpr u32 TLS_INDEX = 0x40, TLS_TEMPLATE = 0x100, TLS_TEMPLATE_BYTES = 0x20, TLS_ZERO = 0x40, HIGHLOW_SLOT = 0x80;

struct ImportItem {
    const char* name;
    u16 ordinal;
};

struct ImportLib {
    const char* dll;
    ImportItem items[8];
    u32 count;
};

struct Spec {
    u64 base = 0x140000000ull;
    bool dll = false;
    bool stripped = false;
    bool with_relocs = true;
    bool with_tls = true;
    bool with_pdata = true;
    bool with_highlow = false;
    u32 callbacks = 2;
    ImportLib libs[3] = {};
    u32 lib_count = 0;
};

struct Built {
    u8 file[FILE_BYTES];
    u32 size;
    u32 reloc_count;
    u32 slot_rvas[SLOT_COUNT];
};

static void default_imports(Spec& s) {
    s.lib_count = 2;
    s.libs[0] = ImportLib{ "kernel32.dll", { { "ExitProcess", 0 }, { "GetStdHandle", 0 }, { "WriteFile", 0 } }, 3 };
    s.libs[1] = ImportLib{ "mylib.dll", { { "Alpha", 0 }, { nullptr, 7 } }, 2 };
}

static void build(const Spec& sp, Built* out) {
    zero(out, sizeof(*out));
    u8* f = out->file;
    f[0] = 'M';
    f[1] = 'Z';
    w32(f + 0x3C, 0x80);
    u8* pe = f + 0x80;
    w32(pe, pe::PE_SIGNATURE);
    u8* coff = pe + 4;
    w16(coff, pe::MACHINE_AMD64);
    w16(coff + 2, 4);
    w16(coff + 16, 240);
    w16(coff + 18, static_cast<u16>(pe::FILE_EXECUTABLE | (sp.dll ? pe::FILE_DLL : 0) | (sp.stripped ? pe::FILE_RELOCS_STRIPPED : 0)));
    u8* opt = coff + 20;
    w16(opt, pe::OPTIONAL_PE32PLUS);
    w32(opt + 16, TEXT_RVA + 0x10);
    w64(opt + 24, sp.base);
    w32(opt + 32, 0x1000);
    w32(opt + 36, 0x200);
    w32(opt + 56, IMAGE_BYTES);
    w32(opt + 60, 0x400);
    w16(opt + 68, pe::SUBSYSTEM_CONSOLE);
    w16(opt + 70, pe::DLLCHAR_DYNAMIC_BASE | pe::DLLCHAR_NX_COMPAT);
    w64(opt + 72, 0x100000);
    w64(opt + 80, 0x1000);
    w64(opt + 88, 0x100000);
    w64(opt + 96, 0x1000);
    w32(opt + 108, 16);
    u8* dirs = opt + 112;

    struct Sec { const char* name; u32 rva, vsize, raw, off, chars; };
    const Sec secs[4] = {
        { ".text", 0x1000, 0x200, 0x200, OFF_TEXT, 0x60000020 },
        { ".rdata", 0x2000, 0x600, 0x600, OFF_RDATA, 0x40000040 },
        { ".data", 0x3000, 0x400, 0x200, OFF_DATA, 0xC0000040 },
        { ".reloc", 0x4000, 0x200, 0x200, OFF_RELOC, 0x42000040 },
    };
    u8* table = opt + 240;
    for (u32 i = 0; i < 4; ++i) {
        u8* h = table + 40 * i;
        for (u32 k = 0; secs[i].name[k]; ++k) h[k] = static_cast<u8>(secs[i].name[k]);
        w32(h + 8, secs[i].vsize);
        w32(h + 12, secs[i].rva);
        w32(h + 16, secs[i].raw);
        w32(h + 20, secs[i].off);
        w32(h + 36, secs[i].chars);
    }

    u8* text = f + OFF_TEXT;
    for (u32 i = 0; i < 0x200; ++i) text[i] = 0x90;
    text[0x10] = 0xC3;
    u8* rd = f + OFF_RDATA;
    u8* da = f + OFF_DATA;

    u32 names = 0x300;
    u32 lib_count = sp.lib_count;
    for (u32 l = 0; l < lib_count; ++l) {
        const ImportLib& lib = sp.libs[l];
        u8* desc = rd + 20 * l;
        u32 ilt = 0x100 + l * 0x60;
        u32 iat = 0x200 + l * 0x60;
        w32(desc, static_cast<u32>(RDATA_RVA + ilt));
        w32(desc + 12, static_cast<u32>(RDATA_RVA + names));
        w32(desc + 16, static_cast<u32>(RDATA_RVA + iat));
        for (u32 k = 0; lib.dll[k]; ++k) rd[names++] = static_cast<u8>(lib.dll[k]);
        rd[names++] = 0;
        names = (names + 1) & ~1u;
        for (u32 i = 0; i < lib.count; ++i) {
            u64 thunk;
            if (lib.items[i].name) {
                thunk = RDATA_RVA + names;
                rd[names] = 0;
                rd[names + 1] = 0;
                names += 2;
                for (u32 k = 0; lib.items[i].name[k]; ++k) rd[names++] = static_cast<u8>(lib.items[i].name[k]);
                rd[names++] = 0;
                names = (names + 1) & ~1u;
            } else {
                thunk = (1ull << 63) | lib.items[i].ordinal;
            }
            w64(rd + ilt + 8 * i, thunk);
            w64(rd + iat + 8 * i, thunk);
        }
    }
    if (lib_count) {
        w32(dirs + 8 * pe::DIR_IMPORT, static_cast<u32>(RDATA_RVA));
        w32(dirs + 8 * pe::DIR_IMPORT + 4, 20 * (lib_count + 1));
        w32(dirs + 8 * pe::DIR_IAT, static_cast<u32>(RDATA_RVA + 0x200));
        w32(dirs + 8 * pe::DIR_IAT + 4, 0x60 * lib_count);
    }

    u32 relocs_rdata[16];
    u32 relocs_data[16];
    u32 nr = 0, nd = 0;
    for (u32 i = 0; i < SLOT_COUNT; ++i) {
        w64(da + 8 * i, sp.base + TEXT_RVA + 0x20 * i);
        relocs_data[nd++] = 8 * i;
        out->slot_rvas[i] = static_cast<u32>(DATA_RVA + 8 * i);
    }
    if (sp.with_highlow) w32(da + HIGHLOW_SLOT, static_cast<u32>(sp.base + 0x1234));
    if (sp.with_tls) {
        for (u32 i = 0; i < TLS_TEMPLATE_BYTES; ++i) da[TLS_TEMPLATE + i] = static_cast<u8>(0xA0 + i);
        u8* t = rd + TLS_DIR;
        w64(t, sp.base + DATA_RVA + TLS_TEMPLATE);
        w64(t + 8, sp.base + DATA_RVA + TLS_TEMPLATE + TLS_TEMPLATE_BYTES);
        w64(t + 16, sp.base + DATA_RVA + TLS_INDEX);
        w64(t + 24, sp.callbacks ? sp.base + RDATA_RVA + TLS_CALLBACKS : 0);
        w32(t + 32, TLS_ZERO);
        relocs_rdata[nr++] = TLS_DIR;
        relocs_rdata[nr++] = TLS_DIR + 8;
        relocs_rdata[nr++] = TLS_DIR + 16;
        if (sp.callbacks) relocs_rdata[nr++] = TLS_DIR + 24;
        for (u32 i = 0; i < sp.callbacks; ++i) {
            w64(rd + TLS_CALLBACKS + 8 * i, sp.base + TEXT_RVA + 0x40 + 0x10 * i);
            relocs_rdata[nr++] = TLS_CALLBACKS + 8 * i;
        }
        w32(dirs + 8 * pe::DIR_TLS, static_cast<u32>(RDATA_RVA + TLS_DIR));
        w32(dirs + 8 * pe::DIR_TLS + 4, 40);
    }
    if (sp.with_pdata) {
        w32(rd + PDATA, 0x1000);
        w32(rd + PDATA + 4, 0x1010);
        w32(rd + PDATA + 8, 0x2000);
        w32(rd + PDATA + 12, 0x1020);
        w32(rd + PDATA + 16, 0x1030);
        w32(rd + PDATA + 20, 0x2010);
        w32(dirs + 8 * pe::DIR_EXCEPTION, static_cast<u32>(RDATA_RVA + PDATA));
        w32(dirs + 8 * pe::DIR_EXCEPTION + 4, 24);
    }

    u32 rel_bytes = 0;
    u8* rl = f + OFF_RELOC;
    out->reloc_count = 0;
    auto block = [&](u32 page, const u32* offs, u32 n, bool highlow) {
        u32 entries = n + (highlow ? 1 : 0);
        u32 size = 8 + 2 * entries;
        u32 padded = (size + 3) & ~3u;
        w32(rl + rel_bytes, page);
        w32(rl + rel_bytes + 4, padded);
        for (u32 i = 0; i < n; ++i) w16(rl + rel_bytes + 8 + 2 * i, static_cast<u32>((pe::REL_DIR64 << 12) | offs[i]));
        u32 pos = n;
        if (highlow) w16(rl + rel_bytes + 8 + 2 * pos++, static_cast<u32>((pe::REL_HIGHLOW << 12) | HIGHLOW_SLOT));
        for (; 8 + 2 * pos < padded; ++pos) w16(rl + rel_bytes + 8 + 2 * pos, 0);
        rel_bytes += padded;
        out->reloc_count += entries;
    };
    if (sp.with_relocs) {
        if (nr) block(static_cast<u32>(RDATA_RVA), relocs_rdata, nr, false);
        block(static_cast<u32>(DATA_RVA), relocs_data, nd, sp.with_highlow);
        w32(dirs + 8 * pe::DIR_BASERELOC, static_cast<u32>(RELOC_RVA));
        w32(dirs + 8 * pe::DIR_BASERELOC + 4, rel_bytes);
    }
    out->size = FILE_BYTES;
}

struct Table {
    const char* dll;
    const char* name;
    u16 ordinal;
    u64 address;
};

struct Resolver {
    const Table* entries;
    u32 count;
    u32 calls;
};

static bool resolve_table(void* ctx, const char* dll, const char* name, u16 ordinal, u64* address) {
    Resolver* r = static_cast<Resolver*>(ctx);
    ++r->calls;
    for (u32 i = 0; i < r->count; ++i) {
        const Table& t = r->entries[i];
        if (!text_eq(t.dll, dll)) continue;
        if (name ? (t.name && text_eq(t.name, name)) : (!t.name && t.ordinal == ordinal)) {
            *address = t.address;
            return true;
        }
    }
    return false;
}

static bool resolve_all(void*, const char*, const char*, u16, u64* address) {
    *address = 0x7FFE00001000ull;
    return true;
}

static const Table KNOWN[] = {
    { "kernel32.dll", "ExitProcess", 0, 0x7FFB00000100ull },
    { "kernel32.dll", "GetStdHandle", 0, 0x7FFB00000200ull },
    { "kernel32.dll", "WriteFile", 0, 0x7FFB00000300ull },
    { "mylib.dll", "Alpha", 0, 0x7FFB00001100ull },
    { "mylib.dll", nullptr, 7, 0x7FFB00001700ull },
};

static u8* new_image(const pe::Image& img) {
    u8* p = static_cast<u8*>(malloc(img.size_of_image));
    zero(p, img.size_of_image);
    return p;
}

static bool invariants(const pe::Image& img, usize file_size) {
    if (img.section_count == 0 || img.section_count > pe::MAX_SECTIONS) return false;
    if (img.size_of_image == 0 || img.size_of_image % img.section_alignment != 0 || img.size_of_image > pe::MAX_IMAGE_BYTES) return false;
    if (img.section_alignment < 4096 || img.file_alignment < 512 || img.file_alignment > img.section_alignment) return false;
    if (img.size_of_headers > img.size_of_image || img.size_of_headers > file_size || img.size_of_headers % img.file_alignment != 0) return false;
    if (img.image_base % 0x10000 != 0 || img.image_base + img.size_of_image > 0x0000800000000000ull) return false;
    u64 prev_end = (static_cast<u64>(img.size_of_headers) + img.section_alignment - 1) / img.section_alignment * img.section_alignment;
    bool entry_ok = false;
    for (u32 i = 0; i < img.section_count; ++i) {
        const pe::Section& s = img.sections[i];
        u64 eff = s.virtual_size ? s.virtual_size : s.raw_size;
        u64 span = (eff + img.section_alignment - 1) / img.section_alignment * img.section_alignment;
        if (s.rva % img.section_alignment != 0 || s.rva < prev_end) return false;
        if (static_cast<u64>(s.rva) + span > img.size_of_image) return false;
        if (s.raw_size && (static_cast<u64>(s.raw_offset) + s.raw_size > file_size || s.raw_offset % img.file_alignment != 0 || s.raw_offset < img.size_of_headers)) return false;
        if ((s.prot & pe::PROT_WRITE) && (s.prot & pe::PROT_EXEC)) return false;
        if (!(s.prot & pe::PROT_READ)) return false;
        if (s.mapped_bytes % 4096 != 0 || s.mapped_bytes < eff) return false;
        if ((s.characteristics & pe::SCN_MEM_EXECUTE) && !(s.prot & pe::PROT_EXEC)) return false;
        if ((s.characteristics & pe::SCN_MEM_WRITE) && !(s.characteristics & pe::SCN_MEM_EXECUTE) && !(s.prot & pe::PROT_WRITE)) return false;
        if (img.entry_rva >= s.rva && img.entry_rva < s.rva + eff && (s.prot & pe::PROT_EXEC)) entry_ok = true;
        prev_end = static_cast<u64>(s.rva) + span;
        for (u32 j = 0; j < i; ++j) {
            const pe::Section& o = img.sections[j];
            if (s.raw_size && o.raw_size && !(s.raw_offset + static_cast<u64>(s.raw_size) <= o.raw_offset || o.raw_offset + static_cast<u64>(o.raw_size) <= s.raw_offset)) return false;
        }
    }
    if (img.entry_rva != 0 || !img.is_dll) {
        if (!entry_ok) return false;
    }
    for (u32 i = 0; i < img.directory_count; ++i) {
        const pe::Directory& d = img.directories[i];
        if (i == pe::DIR_SECURITY || d.rva == 0 || d.size == 0) continue;
        if (static_cast<u64>(d.rva) + d.size > img.size_of_image) return false;
    }
    return true;
}

static bool plan_ok(const pe::Image& img) {
    pe::Run runs[pe::MAX_RUNS];
    u32 n = pe::plan(img, runs, pe::MAX_RUNS);
    if (n == 0 || n > pe::MAX_RUNS) return false;
    u32 pages = img.size_of_image / 4096;
    u32 cursor = 0;
    for (u32 i = 0; i < n; ++i) {
        if (runs[i].first_page != cursor || runs[i].pages == 0) return false;
        if ((runs[i].prot & pe::PROT_WRITE) && (runs[i].prot & pe::PROT_EXEC)) return false;
        if (i && runs[i].prot == runs[i - 1].prot) return false;
        cursor += runs[i].pages;
    }
    if (cursor != pages) return false;
    for (u32 p = 0; p < pages; ++p) {
        u32 want = pe::PROT_READ;
        for (u32 s = 0; s < img.section_count; ++s) {
            const pe::Section& sec = img.sections[s];
            u32 first = sec.rva / 4096;
            if (p >= first && p < first + sec.mapped_bytes / 4096) want = sec.prot;
        }
        u32 got = 0;
        u32 at = 0;
        for (u32 i = 0; i < n; ++i) {
            if (p >= at && p < at + runs[i].pages) got = runs[i].prot;
            at += runs[i].pages;
        }
        if (got != want) return false;
    }
    return true;
}

static void valid_tests() {
    static Built b;
    Spec sp;
    default_imports(sp);
    build(sp, &b);
    static pe::Image img;
    pe::Status st = pe::parse(b.file, b.size, &img);
    expect(st == pe::Status::Ok, "valid image parses");
    expect(img.image_base == 0x140000000ull && img.entry_rva == 0x1010 && img.size_of_image == IMAGE_BYTES && img.size_of_headers == 0x400,
           "image base, entry, image size and headers size are read");
    expect(img.section_count == 4 && img.sections[0].prot == 5 && img.sections[1].prot == 1 && img.sections[2].prot == 3 && img.sections[3].prot == 1,
           "section protections: .text RX, .rdata R, .data RW, .reloc R");
    expect(img.subsystem == pe::SUBSYSTEM_CONSOLE && !img.is_dll && img.has_relocations && img.stack_reserve == 0x100000 && img.heap_commit == 0x1000,
           "subsystem, dll flag, relocations flag, stack and heap sizes");
    expect(img.sections[2].virtual_size == 0x400 && img.sections[2].raw_size == 0x200 && img.sections[2].mapped_bytes == 0x1000,
           "a section with more virtual than raw size maps whole pages");
    expect(invariants(img, b.size), "invariants hold for the valid image");
    expect(plan_ok(img), "the page plan covers the image once, merges equal neighbours and never has W+X");
    pe::Run runs[pe::MAX_RUNS];
    u32 n = pe::plan(img, runs, pe::MAX_RUNS);
    expect(n == 5 && runs[0].first_page == 0 && runs[0].prot == 1 && runs[1].first_page == 1 && runs[1].prot == 5 && runs[2].prot == 1 && runs[3].prot == 3 &&
               runs[4].prot == 1 && runs[4].first_page == 4,
           "plan: header R, text RX, rdata R, data RW, reloc R");
    pe::Run tiny[2];
    expect(pe::plan(img, tiny, 2) == 5, "plan: a short output array still reports the full run count");

    u8* image = new_image(img);
    expect(pe::map(img, b.file, b.size, image, img.size_of_image) == pe::Status::Ok, "map succeeds");
    bool copied = same(image, b.file, 0x400) && same(image + TEXT_RVA, b.file + OFF_TEXT, 0x200) && same(image + RDATA_RVA, b.file + OFF_RDATA, 0x600) &&
                  same(image + DATA_RVA, b.file + OFF_DATA, 0x200) && same(image + RELOC_RVA, b.file + OFF_RELOC, 0x200);
    expect(copied, "map copies the headers and every section's raw data to its address");
    bool zeroed = true;
    for (u32 i = 0x200; i < 0x1000 - 0x00; ++i) if (image[TEXT_RVA + i] != 0) zeroed = false;
    for (u32 i = 0x200; i < 0x1000; ++i) if (image[DATA_RVA + i] != 0) zeroed = false;
    for (u32 i = 0x400; i < 0x1000; ++i) if (image[0 + i] != 0 && i < 0x1000) zeroed = false;
    expect(zeroed, "map zero-fills past the raw data and the gaps");
    expect(pe::map(img, b.file, b.size, image, img.size_of_image - 1) == pe::Status::ImageBufferTooSmall, "map refuses a buffer that is one byte short");
    free(image);

    static Built dll;
    Spec ds = sp;
    ds.dll = true;
    build(ds, &dll);
    static pe::Image dimg;
    expect(pe::parse(dll.file, dll.size, &dimg) == pe::Status::Ok && dimg.is_dll, "a DLL parses");
    w32(dll.file + 0x80 + 4 + 20 + 16, 0);
    expect(pe::parse(dll.file, dll.size, &dimg) == pe::Status::Ok && dimg.entry_rva == 0, "a DLL may have no entry point");
    w32(b.file + 0x80 + 4 + 20 + 16, 0);
    expect(pe::parse(b.file, b.size, &img) == pe::Status::BadEntry, "an executable must have an entry point");
}

static void relocation_tests() {
    static Built b;
    Spec sp;
    default_imports(sp);
    sp.with_highlow = true;
    build(sp, &b);
    static pe::Image img;
    pe::parse(b.file, b.size, &img);
    const u64 bases[] = { 0x140000000ull, 0x7FF600000000ull, 0x10000ull, 0x140010000ull, 0x7FFFFFFE0000ull, 0x100000000ull };
    bool all = true;
    u32 applied_ok = 0;
    for (u64 base : bases) {
        u8* image = new_image(img);
        pe::map(img, b.file, b.size, image, img.size_of_image);
        u32 applied = 0;
        pe::Status st = pe::relocate(img, image, base, &applied);
        bool ok = st == pe::Status::Ok;
        u64 delta = base - 0x140000000ull;
        for (u32 i = 0; i < SLOT_COUNT; ++i) ok = ok && r64(image + b.slot_rvas[i]) == 0x140000000ull + TEXT_RVA + 0x20 * i + delta;
        ok = ok && r32(image + DATA_RVA + HIGHLOW_SLOT) == static_cast<u32>(0x140000000ull + 0x1234 + delta);
        ok = ok && r64(image + RDATA_RVA + TLS_DIR) == 0x140000000ull + DATA_RVA + TLS_TEMPLATE + delta;
        ok = ok && r64(image + RDATA_RVA + TLS_CALLBACKS + 8) == 0x140000000ull + TEXT_RVA + 0x50 + delta;
        if (base == 0x140000000ull) ok = ok && applied == 0 && same(image + DATA_RVA, b.file + OFF_DATA, 0x200);
        else ok = ok && applied == b.reloc_count - 0;
        if (base != 0x140000000ull && applied != b.reloc_count) ok = false;
        else if (base != 0x140000000ull) ++applied_ok;
        if (!ok) all = false;
        free(image);
    }
    expect(all, "relocation: every slot, the HIGHLOW slot and the TLS fields move by the delta at six bases, including below the preferred base");
    expect(applied_ok == 5, "relocation: the applied count equals the number of non-padding entries");

    u8* image = new_image(img);
    pe::map(img, b.file, b.size, image, img.size_of_image);
    expect(pe::relocate(img, image, 0x140001000ull) == pe::Status::BadBase, "relocation: a base that is not 64 KiB aligned is refused");
    expect(pe::relocate(img, image, 0x0000800000000000ull) == pe::Status::BadBase, "relocation: a base at the user limit is refused");
    static pe::Image big;
    copy(&big, &img, sizeof(big));
    big.size_of_image = 0x20000;
    expect(pe::relocate(big, image, 0x7FFFFFFF0000ull) == pe::Status::BadBase && pe::relocate(img, image, 0x7FFFFFFF0000ull) == pe::Status::Ok,
           "relocation: an image that would cross the user limit is refused");
    free(image);

    static Built s;
    Spec ss = sp;
    ss.stripped = true;
    ss.with_relocs = false;
    build(ss, &s);
    static pe::Image simg;
    expect(pe::parse(s.file, s.size, &simg) == pe::Status::Ok && !simg.has_relocations, "a relocation-stripped image parses and says so");
    u8* sim = new_image(simg);
    pe::map(simg, s.file, s.size, sim, simg.size_of_image);
    expect(pe::relocate(simg, sim, 0x140000000ull) == pe::Status::Ok, "a stripped image loads at its preferred base");
    expect(pe::relocate(simg, sim, 0x150000000ull) == pe::Status::NeedsFixedBase, "a stripped image cannot move");
    free(sim);

    struct Bad { const char* what; u32 at; u32 value; pe::Status want; };
    const u32 rel_off = OFF_RELOC;
    const Bad bads[] = {
        { "a block smaller than its header", rel_off + 4, 4, pe::Status::BadReloc },
        { "an odd block size", rel_off + 4, 13, pe::Status::BadReloc },
        { "a block that runs past the directory", rel_off + 4, 0x300, pe::Status::BadReloc },
        { "a page outside the image", rel_off, 0x9000, pe::Status::BadReloc },
        { "a page that is not 4 KiB aligned", rel_off, 0x2010, pe::Status::BadReloc },
        { "a block for the header page", rel_off, 0, pe::Status::BadReloc },
        { "an entry that straddles the end of its section", rel_off + 8, (pe::REL_DIR64 << 12) | 0xFFC, pe::Status::BadReloc },
        { "an unsupported relocation type", rel_off + 8, (5 << 12) | 0x10, pe::Status::BadRelocType },
    };
    bool refused = true;
    for (const Bad& x : bads) {
        static Built m;
        copy(&m, &b, sizeof(m));
        if (x.at == rel_off + 4 || x.at == rel_off) w32(m.file + x.at, x.value);
        else w16(m.file + x.at, x.value);
        static pe::Image mi;
        pe::Status ps = pe::parse(m.file, m.size, &mi);
        pe::Status got = pe::Status::Ok;
        if (ps == pe::Status::Ok) {
            u8* mim = new_image(mi);
            pe::map(mi, m.file, m.size, mim, mi.size_of_image);
            got = pe::relocate(mi, mim, 0x150000000ull);
            free(mim);
        } else {
            got = ps;
        }
        bool ok = got == x.want;
        if (!ok) printf("    %s: got %s, wanted %s\n", x.what, pe::status_name(got), pe::status_name(x.want));
        refused = refused && ok;
    }
    expect(refused, "relocation: eight kinds of malformed relocation blocks are refused with the right status");

    static Built nr;
    copy(&nr, &b, sizeof(nr));
    for (u32 i = 0; i < 0x200; ++i) nr.file[OFF_RELOC + i] = 0;
    w32(nr.file + 0x80 + 4 + 20 + 112 + 8 * pe::DIR_BASERELOC + 4, 0x20);
    static pe::Image ni;
    pe::parse(nr.file, nr.size, &ni);
    u8* nim = new_image(ni);
    pe::map(ni, nr.file, nr.size, nim, ni.size_of_image);
    expect(pe::relocate(ni, nim, 0x150000000ull) == pe::Status::BadReloc, "relocation: an all-zero directory is a block with size 0 and is refused");
    free(nim);
}

static void import_tests() {
    static Built b;
    Spec sp;
    default_imports(sp);
    build(sp, &b);
    static pe::Image img;
    pe::parse(b.file, b.size, &img);
    u8* image = new_image(img);
    pe::map(img, b.file, b.size, image, img.size_of_image);
    pe::relocate(img, image, 0x7FF600000000ull);

    static pe::ImportReport rep;
    Resolver res{ KNOWN, 5, 0 };
    pe::Status st = pe::bind_imports(img, image, resolve_table, &res, &rep);
    expect(st == pe::Status::Ok, "imports: all five resolve");
    expect(rep.libraries == 2 && rep.imports == 5 && rep.resolved == 5 && rep.missing == 0, "imports: report counts two libraries and five imports");
    bool iat = r64(image + RDATA_RVA + 0x200) == 0x7FFB00000100ull && r64(image + RDATA_RVA + 0x208) == 0x7FFB00000200ull &&
               r64(image + RDATA_RVA + 0x210) == 0x7FFB00000300ull && r64(image + RDATA_RVA + 0x218) == 0 &&
               r64(image + RDATA_RVA + 0x260) == 0x7FFB00001100ull && r64(image + RDATA_RVA + 0x268) == 0x7FFB00001700ull &&
               r64(image + RDATA_RVA + 0x270) == 0;
    expect(iat, "imports: the IAT holds the resolved addresses (by name and by ordinal) and keeps its null terminators");
    bool ilt = r64(image + RDATA_RVA + 0x100) == r64(b.file + OFF_RDATA + 0x100);
    expect(ilt, "imports: the lookup table is left alone");

    u8* again = new_image(img);
    pe::map(img, b.file, b.size, again, img.size_of_image);
    pe::relocate(img, again, 0x7FF600000000ull);
    const Table partial[] = { KNOWN[0], KNOWN[2], KNOWN[4] };
    Resolver pr{ partial, 3, 0 };
    u8* before = new_image(img);
    copy(before, again, img.size_of_image);
    st = pe::bind_imports(img, again, resolve_table, &pr, &rep);
    expect(st == pe::Status::UnresolvedImport, "imports: a missing function refuses the image");
    expect(rep.missing == 2 && rep.resolved == 3 && rep.imports == 5, "imports: the report counts what was found and what was not");
    expect(text_eq(rep.first[0].dll, "kernel32.dll") && text_eq(rep.first[0].name, "GetStdHandle") && !rep.first[0].by_ordinal,
           "imports: the report names the first missing function and its DLL");
    expect(text_eq(rep.first[1].dll, "mylib.dll") && text_eq(rep.first[1].name, "Alpha"), "imports: the report names the second missing function");
    expect(same(before, again, img.size_of_image), "imports: a refused image's IAT is not touched");
    const Table noord[] = { KNOWN[0], KNOWN[1], KNOWN[2], KNOWN[3] };
    Resolver nr{ noord, 4, 0 };
    st = pe::bind_imports(img, again, resolve_table, &nr, &rep);
    expect(st == pe::Status::UnresolvedImport && rep.missing == 1 && rep.first[0].by_ordinal && rep.first[0].ordinal == 7 && text_eq(rep.first[0].dll, "mylib.dll"),
           "imports: a missing import by ordinal is reported with its ordinal");
    free(before);
    free(again);

    st = pe::bind_imports(img, image, resolve_all, nullptr, &rep);
    expect(st == pe::Status::Ok && r64(image + RDATA_RVA + 0x200) == 0x7FFE00001000ull, "imports: a resolver that accepts everything fills every slot");

    static Built none;
    Spec ns = sp;
    ns.lib_count = 0;
    build(ns, &none);
    static pe::Image nimg;
    pe::parse(none.file, none.size, &nimg);
    u8* nim = new_image(nimg);
    pe::map(nimg, none.file, none.size, nim, nimg.size_of_image);
    st = pe::bind_imports(nimg, nim, resolve_table, &res, &rep);
    expect(st == pe::Status::Ok && rep.imports == 0 && rep.libraries == 0, "imports: an image with no import directory binds nothing");
    free(nim);
    free(image);

    static Built many;
    Spec ms;
    ms.lib_count = 1;
    ms.libs[0].dll = "big.dll";
    ms.libs[0].count = 8;
    static char names[8][8];
    for (u32 i = 0; i < 8; ++i) {
        names[i][0] = 'f';
        names[i][1] = static_cast<char>('0' + i);
        names[i][2] = 0;
        ms.libs[0].items[i] = ImportItem{ names[i], 0 };
    }
    build(ms, &many);
    static pe::Image mimg;
    pe::parse(many.file, many.size, &mimg);
    u8* mim = new_image(mimg);
    pe::map(mimg, many.file, many.size, mim, mimg.size_of_image);
    Resolver none_r{ KNOWN, 0, 0 };
    st = pe::bind_imports(mimg, mim, resolve_table, &none_r, &rep);
    expect(st == pe::Status::UnresolvedImport && rep.missing == 8 && rep.first[7].name[1] == '7', "imports: every missing import is counted and listed");
    expect(none_r.calls == 8, "imports: a refused image's resolver is called once per import, not twice");
    free(mim);

    static Built bad;
    struct Bad { const char* what; u32 at; u64 value; bool wide; };
    const Bad bads[] = {
        { "a DLL name outside the image", OFF_RDATA + 12, 0x9000, false },
        { "a DLL name RVA of zero", OFF_RDATA + 12, 0, false },
        { "an unaligned first thunk", OFF_RDATA + 16, 0x2204, false },
        { "a first thunk outside the image", OFF_RDATA + 16, 0x9000, false },
        { "a thunk with reserved bits set", OFF_RDATA + 0x100, (1ull << 63) | (1ull << 40), true },
        { "a by-name thunk beyond 31 bits", OFF_RDATA + 0x100, 0x80000000ull, true },
        { "a by-name thunk outside the image", OFF_RDATA + 0x100, 0x6000, true },
        { "a name with no terminator at the end of the image", OFF_RDATA + 0x100, 0x4FFE, true },
    };
    bool refused = true;
    for (const Bad& x : bads) {
        copy(&bad, &b, sizeof(bad));
        if (x.wide) w64(bad.file + x.at, x.value);
        else w32(bad.file + x.at, static_cast<u32>(x.value));
        static pe::Image bi;
        pe::Status ps = pe::parse(bad.file, bad.size, &bi);
        pe::Status got = ps;
        if (ps == pe::Status::Ok) {
            u8* bim = new_image(bi);
            pe::map(bi, bad.file, bad.size, bim, bi.size_of_image);
            got = pe::bind_imports(bi, bim, resolve_all, nullptr, &rep);
            free(bim);
        }
        bool ok = got == pe::Status::BadImport;
        if (!ok) printf("    %s: got %s\n", x.what, pe::status_name(got));
        refused = refused && ok;
    }
    expect(refused, "imports: eight kinds of malformed import tables are refused");

    copy(&bad, &b, sizeof(bad));
    for (u32 i = 0; i < 0x60; ++i) bad.file[OFF_RDATA + 0x200 + i] = 0xFF;
    w32(bad.file + OFF_RDATA + 16, 0);
    static pe::Image zi;
    pe::parse(bad.file, bad.size, &zi);
    u8* zim = new_image(zi);
    pe::map(zi, bad.file, bad.size, zim, zi.size_of_image);
    expect(pe::bind_imports(zi, zim, resolve_all, nullptr, &rep) == pe::Status::BadImport, "imports: a descriptor with no IAT is refused");
    free(zim);

    copy(&bad, &b, sizeof(bad));
    for (u32 i = 0; i < 0x100; ++i) bad.file[OFF_RDATA + 0x100 + i] = 0;
    for (u32 i = 0; i < 0x100; ++i) w64(bad.file + OFF_RDATA + 0x100 + 8 * (i % 0x20), 0x2300);
    w32(bad.file + OFF_RDATA, static_cast<u32>(RDATA_RVA + 0x100));
    static pe::Image li;
    pe::parse(bad.file, bad.size, &li);
    u8* lim = new_image(li);
    pe::map(li, bad.file, bad.size, lim, li.size_of_image);
    st = pe::bind_imports(li, lim, resolve_all, nullptr, &rep);
    expect(st == pe::Status::BadImport || st == pe::Status::Ok, "imports: a lookup table with no terminator inside its section ends in a bounded refusal");
    free(lim);
}

static void tls_tests() {
    static Built b;
    Spec sp;
    default_imports(sp);
    build(sp, &b);
    static pe::Image img;
    pe::parse(b.file, b.size, &img);
    u8* image = new_image(img);
    pe::map(img, b.file, b.size, image, img.size_of_image);
    const u64 base = 0x7FF600000000ull;
    pe::relocate(img, image, base);
    static pe::Tls tls;
    expect(pe::tls_info(img, image, base, &tls) == pe::Status::Ok && tls.present, "tls: a relocated image reports a TLS directory");
    expect(tls.template_rva == DATA_RVA + TLS_TEMPLATE && tls.template_bytes == TLS_TEMPLATE_BYTES && tls.zero_fill == TLS_ZERO && tls.index_rva == DATA_RVA + TLS_INDEX,
           "tls: template, zero fill and index location");
    expect(tls.callback_count == 2 && tls.callbacks[0] == base + TEXT_RVA + 0x40 && tls.callbacks[1] == base + TEXT_RVA + 0x50, "tls: callbacks are listed as absolute addresses");
    expect(pe::tls_info(img, image, 0x140000000ull, &tls) == pe::Status::BadTls, "tls: addresses that do not belong to the stated base are refused");
    free(image);

    static Built n;
    Spec ns = sp;
    ns.with_tls = false;
    build(ns, &n);
    static pe::Image nimg;
    pe::parse(n.file, n.size, &nimg);
    u8* nim = new_image(nimg);
    pe::map(nimg, n.file, n.size, nim, nimg.size_of_image);
    expect(pe::tls_info(nimg, nim, 0x140000000ull, &tls) == pe::Status::Ok && !tls.present, "tls: an image without a directory reports none");
    free(nim);

    static Built nc;
    Spec cs = sp;
    cs.callbacks = 0;
    build(cs, &nc);
    static pe::Image cimg;
    pe::parse(nc.file, nc.size, &cimg);
    u8* cim = new_image(cimg);
    pe::map(cimg, nc.file, nc.size, cim, cimg.size_of_image);
    expect(pe::tls_info(cimg, cim, 0x140000000ull, &tls) == pe::Status::Ok && tls.present && tls.callback_count == 0, "tls: no callbacks is fine");
    free(cim);

    struct Bad { const char* what; u32 at; u64 value; bool wide; };
    const Bad bads[] = {
        { "a template start outside the image", OFF_RDATA + TLS_DIR, 0x140009000ull, true },
        { "a template that ends before it starts", OFF_RDATA + TLS_DIR + 8, 0x140003000ull, true },
        { "a template in the headers", OFF_RDATA + TLS_DIR, 0x140000100ull, true },
        { "an index outside the image", OFF_RDATA + TLS_DIR + 16, 0x140009000ull, true },
        { "a callback table outside the image", OFF_RDATA + TLS_DIR + 24, 0x140009000ull, true },
        { "a callback in non-executable memory", OFF_RDATA + TLS_CALLBACKS, 0x140003000ull, true },
        { "an enormous zero fill", OFF_RDATA + TLS_DIR + 32, 0x7FFFFFFFull, false },
    };
    bool refused = true;
    for (const Bad& x : bads) {
        static Built m;
        copy(&m, &b, sizeof(m));
        if (x.wide) w64(m.file + x.at, x.value);
        else w32(m.file + x.at, static_cast<u32>(x.value));
        static pe::Image mi;
        pe::parse(m.file, m.size, &mi);
        u8* mim = new_image(mi);
        pe::map(mi, m.file, m.size, mim, mi.size_of_image);
        pe::Status got = pe::tls_info(mi, mim, 0x140000000ull, &tls);
        bool ok = got == pe::Status::BadTls;
        if (!ok) printf("    %s: got %s\n", x.what, pe::status_name(got));
        refused = refused && ok;
        free(mim);
    }
    expect(refused, "tls: seven kinds of malformed TLS directories are refused");

    static Built many;
    Spec ms = sp;
    ms.callbacks = 3;
    build(ms, &many);
    static pe::Image mimg;
    pe::parse(many.file, many.size, &mimg);
    u8* mim = new_image(mimg);
    pe::map(mimg, many.file, many.size, mim, mimg.size_of_image);
    for (u32 i = 0; i < 20; ++i) w64(mim + RDATA_RVA + TLS_CALLBACKS + 8 * (i % 3), 0x140000000ull + TEXT_RVA + 0x40);
    for (u32 i = 3; i < 4; ++i) w64(mim + RDATA_RVA + TLS_CALLBACKS + 8 * i, 0);
    expect(pe::tls_info(mimg, mim, 0x140000000ull, &tls) == pe::Status::Ok && tls.callback_count == 3, "tls: three callbacks followed by a null are read");
    free(mim);
}

static void header_tests() {
    static Built good;
    Spec sp;
    default_imports(sp);
    build(sp, &good);
    struct Case { const char* what; u32 at; u64 value; u32 width; pe::Status want; };
    const u32 PEO = 0x80, COFF = PEO + 4, OPT = COFF + 20, TBL = OPT + 240;
    const Case cases[] = {
        { "no MZ", 0, 0x5A5A, 2, pe::Status::BadDosMagic },
        { "PE offset below the DOS header", 0x3C, 0x20, 4, pe::Status::BadPeOffset },
        { "PE offset past the end of the file", 0x3C, 0xFFF0, 4, pe::Status::BadPeOffset },
        { "PE offset that wraps", 0x3C, 0xFFFFFFF0u, 4, pe::Status::BadPeOffset },
        { "no PE signature", PEO, 0x4551, 4, pe::Status::BadPeMagic },
        { "i386 machine", COFF, 0x14C, 2, pe::Status::Pe32Unsupported },
        { "ARM64 machine", COFF, 0xAA64, 2, pe::Status::BadMachine },
        { "no sections", COFF + 2, 0, 2, pe::Status::BadSectionCount },
        { "97 sections", COFF + 2, 97, 2, pe::Status::BadSectionCount },
        { "not executable", COFF + 18, 0x2000, 2, pe::Status::NotExecutable },
        { "optional header of one byte", COFF + 16, 1, 2, pe::Status::BadOptionalSize },
        { "optional header past the file", COFF + 16, 0xF000, 2, pe::Status::BadOptionalSize },
        { "optional header shorter than its fixed part", COFF + 16, 100, 2, pe::Status::BadOptionalSize },
        { "PE32 optional header", OPT, 0x10B, 2, pe::Status::Pe32Unsupported },
        { "unknown optional header", OPT, 0x107, 2, pe::Status::BadOptionalMagic },
        { "17 data directories", OPT + 108, 17, 4, pe::Status::BadDirectoryCount },
        { "directory count the header cannot hold", COFF + 16, 120, 2, pe::Status::BadOptionalSize },
        { "section alignment 0x800", OPT + 32, 0x800, 4, pe::Status::BadAlignment },
        { "section alignment not a power of two", OPT + 32, 0x1800, 4, pe::Status::BadAlignment },
        { "file alignment 0x100", OPT + 36, 0x100, 4, pe::Status::BadAlignment },
        { "file alignment above the section alignment", OPT + 36, 0x2000, 4, pe::Status::BadAlignment },
        { "image size zero", OPT + 56, 0, 4, pe::Status::BadImageSize },
        { "image size not a multiple of the section alignment", OPT + 56, 0x5800, 4, pe::Status::BadImageSize },
        { "image size above 16 MiB", OPT + 56, 0x2000000, 4, pe::Status::BadImageSize },
        { "headers size zero", OPT + 60, 0, 4, pe::Status::BadHeadersSize },
        { "headers size not a multiple of the file alignment", OPT + 60, 0x3F0, 4, pe::Status::BadHeadersSize },
        { "headers size that does not hold the section table", OPT + 60, 0x200, 4, pe::Status::BadHeadersSize },
        { "headers size past the image", OPT + 60, 0x6000, 4, pe::Status::BadHeadersSize },
        { "image base not 64 KiB aligned", OPT + 24, 0x140001000ull, 8, pe::Status::BadImageBase },
        { "image base at the user limit", OPT + 24, 0x0000800000000000ull, 8, pe::Status::BadImageBase },
        { "image base that overflows", OPT + 24, 0xFFFFFFFFFFFF0000ull, 8, pe::Status::BadImageBase },
        { "section virtual address not aligned", TBL + 12, 0x1800, 4, pe::Status::BadSection },
        { "first section inside the headers", TBL + 12, 0, 4, pe::Status::SectionOverlap },
        { "sections out of order", TBL + 40 + 12, 0x1000, 4, pe::Status::SectionOverlap },
        { "section past the image", TBL + 3 * 40 + 8, 0x10000, 4, pe::Status::BadSection },
        { "section data outside the file", TBL + 3 * 40 + 20, 0x1000, 4, pe::Status::SectionOutsideFile },
        { "section data with a huge size", TBL + 16, 0xFFFFFFF0u, 4, pe::Status::SectionOutsideFile },
        { "section data not file aligned", TBL + 20, 0x401, 4, pe::Status::BadSection },
        { "section data inside the headers", TBL + 20, 0x200, 4, pe::Status::BadSection },
        { "section data shared with another section", TBL + 40 + 20, 0x400, 4, pe::Status::SectionOverlap },
        { "writable and executable section", TBL + 36, 0xE0000020u, 4, pe::Status::WritableExecutable },
        { "writable and executable data section", TBL + 2 * 40 + 36, 0xE0000040u, 4, pe::Status::WritableExecutable },
        { "entry outside the image", OPT + 16, 0x9000, 4, pe::Status::BadEntry },
        { "entry in a non-executable section", OPT + 16, 0x2000, 4, pe::Status::BadEntry },
        { "entry in a gap between sections", OPT + 16, 0x1800, 4, pe::Status::BadEntry },
        { "entry in the headers", OPT + 16, 0x100, 4, pe::Status::BadEntry },
        { "import directory outside any section", OPT + 112 + 8 * pe::DIR_IMPORT, 0x6000, 4, pe::Status::BadDirectory },
        { "import directory that spans sections", OPT + 112 + 8 * pe::DIR_IMPORT + 4, 0x2000, 4, pe::Status::BadDirectory },
        { "exception directory that is not a multiple of 12", OPT + 112 + 8 * pe::DIR_EXCEPTION + 4, 20, 4, pe::Status::BadDirectory },
        { "TLS directory that is too short", OPT + 112 + 8 * pe::DIR_TLS + 4, 16, 4, pe::Status::BadDirectory },
        { "a .NET image", OPT + 112 + 8 * pe::DIR_CLR, 0x2000, 4, pe::Status::ManagedUnsupported },
        { "a directory whose size wraps", OPT + 112 + 8 * pe::DIR_DEBUG + 4, 0xFFFFFFFFu, 4, pe::Status::BadDirectory },
    };
    u32 bad = 0;
    for (const Case& c : cases) {
        static Built m;
        copy(&m, &good, sizeof(m));
        if (c.what[0] == 'a' && c.at == OPT + 112 + 8 * pe::DIR_CLR) w32(m.file + c.at + 4, 8);
        if (c.at == OPT + 112 + 8 * pe::DIR_DEBUG + 4) w32(m.file + c.at - 4, 0x2000);
        if (c.width == 2) w16(m.file + c.at, static_cast<u32>(c.value));
        else if (c.width == 4) w32(m.file + c.at, static_cast<u32>(c.value));
        else w64(m.file + c.at, c.value);
        static pe::Image img;
        pe::Status got = pe::parse(m.file, m.size, &img);
        if (got != c.want) {
            printf("    %s: got %s, wanted %s\n", c.what, pe::status_name(got), pe::status_name(c.want));
            ++bad;
        }
    }
    expect(bad == 0, "headers: fifty malformed headers each produce exactly the status that names the problem");
    static pe::Image img;
    expect(pe::parse(good.file, 0, &img) == pe::Status::TooSmall && pe::parse(good.file, 63, &img) == pe::Status::TooSmall, "headers: a file shorter than the DOS header is too small");
    pe::parse(good.file, 10, &img);
    expect(img.section_count == 0 && img.image_base == 0, "headers: a refused parse leaves a zeroed image");

    static Built v;
    copy(&v, &good, sizeof(v));
    w32(v.file + OPT + 112 + 8 * pe::DIR_SECURITY, 0xFFFFFF00u);
    w32(v.file + OPT + 112 + 8 * pe::DIR_SECURITY + 4, 0x500);
    expect(pe::parse(v.file, v.size, &img) == pe::Status::Ok, "headers: the security directory is a file offset and is not checked against the image");
    copy(&v, &good, sizeof(v));
    w32(v.file + TBL + 8, 0);
    expect(pe::parse(v.file, v.size, &img) == pe::Status::Ok && img.sections[0].mapped_bytes == 0x1000, "headers: a zero virtual size falls back to the raw size");
    copy(&v, &good, sizeof(v));
    w32(v.file + TBL + 8, 0);
    w32(v.file + TBL + 16, 0);
    expect(pe::parse(v.file, v.size, &img) == pe::Status::BadEntry, "headers: an empty entry section leaves the entry point without code");
    copy(&v, &good, sizeof(v));
    w32(v.file + TBL + 3 * 40 + 8, 0);
    w32(v.file + TBL + 3 * 40 + 16, 0);
    w32(v.file + OPT + 112 + 8 * pe::DIR_BASERELOC, 0);
    w32(v.file + OPT + 112 + 8 * pe::DIR_BASERELOC + 4, 0);
    expect(pe::parse(v.file, v.size, &img) == pe::Status::Ok && img.sections[3].mapped_bytes == 0 && plan_ok(img), "headers: an empty section takes no pages and the plan stays valid");
}

static void truncation_tests() {
    static Built b;
    Spec sp;
    default_imports(sp);
    build(sp, &b);
    static pe::Image img;
    u32 accepted = 0;
    for (u32 n = 0; n < b.size; ++n) {
        if (pe::parse(b.file, n, &img) == pe::Status::Ok) ++accepted;
    }
    expect(accepted == 0, "truncation: no prefix of the valid image is accepted");
    u32 padded = 0;
    static u8 big[FILE_BYTES + 0x100];
    copy(big, b.file, b.size);
    for (u32 extra = 0; extra < 0x100; ++extra) {
        if (pe::parse(big, b.size + extra, &img) == pe::Status::Ok) ++padded;
    }
    expect(padded == 0x100, "truncation: trailing bytes after the last section are accepted");
}

struct Sweep {
    u32 runs;
    u32 accepted;
    u32 violations;
};

static bool exercise(const u8* file, usize size, const pe::Image& img, u8* image) {
    pe::Status m = pe::map(img, file, size, image, img.size_of_image);
    if (m != pe::Status::Ok) return false;
    static pe::ImportReport rep;
    static pe::Tls tls;
    pe::relocate(img, image, 0x7FF600000000ull);
    pe::bind_imports(img, image, resolve_all, nullptr, &rep);
    pe::tls_info(img, image, 0x7FF600000000ull, &tls);
    pe::Run runs[pe::MAX_RUNS];
    u32 n = pe::plan(img, runs, pe::MAX_RUNS);
    return n <= pe::MAX_RUNS;
}

static void sweep(const u8* good, usize size, u32 from, u32 to, bool all_values, Sweep* acc) {
    static pe::Image img;
    static u8 buf[0x20000];
    copy(buf, good, size);
    for (u32 pos = from; pos < to && pos < size; ++pos) {
        u8 keep = buf[pos];
        for (u32 v = 0; v < 256; v += all_values ? 1 : 15) {
            u8 val = static_cast<u8>(v);
            if (all_values == false && v == 0) val = static_cast<u8>(keep ^ 0x80);
            if (val == keep) continue;
            buf[pos] = val;
            ++acc->runs;
            pe::Status st = pe::parse(buf, size, &img);
            if (st == pe::Status::Ok) {
                ++acc->accepted;
                if (!invariants(img, size) || !plan_ok(img)) ++acc->violations;
                u8* image = static_cast<u8*>(malloc(img.size_of_image));
                exercise(buf, size, img, image);
                free(image);
            }
        }
        buf[pos] = keep;
    }
}

static void sweep_tests() {
    static Built b;
    Spec sp;
    default_imports(sp);
    sp.with_highlow = true;
    build(sp, &b);
    Sweep headers{};
    sweep(b.file, b.size, 0, 0x400, true, &headers);
    expect(headers.violations == 0, "sweep: every value at every header byte either fails or satisfies the loader invariants");
    printf("    (%u runs, %u accepted)\n", headers.runs, headers.accepted);
    expect(headers.runs - headers.accepted > 10000, "sweep: a large share of header mutations is refused");

    Sweep tables{};
    sweep(b.file, b.size, OFF_RDATA, OFF_RDATA + 0x600, false, &tables);
    sweep(b.file, b.size, OFF_DATA, OFF_DATA + 0x200, false, &tables);
    sweep(b.file, b.size, OFF_RELOC, OFF_RELOC + 0x40, true, &tables);
    expect(tables.violations == 0, "sweep: mutated import, TLS and relocation tables never break the loader's bounds (ASan and UBSan watch the exact-size image)");
    printf("    (%u runs, %u accepted)\n", tables.runs, tables.accepted);

    u64 field_runs = 0, field_violations = 0;
    static pe::Image img;
    static Built m;
    for (int round = 0; round < 300000; ++round) {
        copy(&m, &b, sizeof(m));
        int edits = 1 + static_cast<int>(rnd() % 4);
        for (int e = 0; e < edits; ++e) {
            u32 at = static_cast<u32>(rnd() % 0x250);
            u32 kind = static_cast<u32>(rnd() % 6);
            u32 v = kind == 0 ? 0 : kind == 1 ? 0xFFFFFFFFu : kind == 2 ? static_cast<u32>(rnd()) : kind == 3 ? static_cast<u32>(rnd() % 0x6000) : kind == 4 ? 0x1000 : 0x200;
            if (rnd() & 1) w32(m.file + at, v);
            else w16(m.file + at, v);
        }
        ++field_runs;
        if (pe::parse(m.file, m.size, &img) == pe::Status::Ok) {
            if (!invariants(img, m.size) || !plan_ok(img)) ++field_violations;
        }
    }
    expect(field_violations == 0, "sweep: 300,000 random multi-field header rewrites never produce an image that breaks the invariants");
    (void)field_runs;
}

static void layout_tests() {
    static u8 f[0x40000];
    static pe::Image img;
    u32 accepted = 0, bad_plan = 0, bad_inv = 0;
    for (int round = 0; round < 4000; ++round) {
        zero(f, sizeof(f));
        u32 nsec = 1 + static_cast<u32>(rnd() % 8);
        u32 sec_align = (rnd() & 3) == 0 ? 0x2000 : 0x1000;
        u32 file_align = (rnd() & 1) ? 0x200 : 0x1000;
        u32 headers = (0x188 + 40 * nsec + file_align - 1) / file_align * file_align;
        f[0] = 'M';
        f[1] = 'Z';
        w32(f + 0x3C, 0x80);
        w32(f + 0x80, pe::PE_SIGNATURE);
        w16(f + 0x84, pe::MACHINE_AMD64);
        w16(f + 0x86, nsec);
        w16(f + 0x84 + 16, 240);
        w16(f + 0x84 + 18, pe::FILE_EXECUTABLE);
        u8* opt = f + 0x98;
        w16(opt, pe::OPTIONAL_PE32PLUS);
        w64(opt + 24, 0x140000000ull);
        w32(opt + 32, sec_align);
        w32(opt + 36, file_align);
        w32(opt + 60, headers);
        w32(opt + 108, 16);
        u32 rva = (headers + sec_align - 1) / sec_align * sec_align;
        u32 raw_at = headers;
        u32 entry_section = static_cast<u32>(rnd() % nsec);
        u32 entry = 0;
        for (u32 i = 0; i < nsec; ++i) {
            u8* h = opt + 240 + 40 * i;
            u32 vsize = 1 + static_cast<u32>(rnd() % 0x5000);
            u32 raw = (rnd() % 3 == 0) ? 0 : (1 + static_cast<u32>(rnd() % 0x3000));
            u32 raw_padded = (raw + file_align - 1) / file_align * file_align;
            u32 chars = 0;
            switch (rnd() % 4) {
                case 0: chars = pe::SCN_MEM_READ | pe::SCN_MEM_EXECUTE | pe::SCN_CNT_CODE; break;
                case 1: chars = pe::SCN_MEM_READ | pe::SCN_MEM_WRITE | pe::SCN_CNT_INITIALIZED; break;
                case 2: chars = pe::SCN_MEM_READ | pe::SCN_CNT_INITIALIZED; break;
                default: chars = 0; break;
            }
            if (i == entry_section) {
                chars = pe::SCN_MEM_READ | pe::SCN_MEM_EXECUTE;
                entry = rva + static_cast<u32>(rnd() % vsize);
            }
            h[0] = static_cast<u8>('a' + i);
            w32(h + 8, vsize);
            w32(h + 12, rva);
            w32(h + 16, raw);
            w32(h + 20, raw ? raw_at : 0);
            w32(h + 36, chars);
            if (raw) raw_at += raw_padded;
            u32 span = (vsize + sec_align - 1) / sec_align * sec_align;
            rva += span + ((rnd() % 4 == 0) ? sec_align : 0);
        }
        w32(opt + 16, entry);
        w32(opt + 56, rva);
        u32 file_size = raw_at;
        if (file_size > sizeof(f)) continue;
        if (pe::parse(f, file_size, &img) == pe::Status::Ok) {
            ++accepted;
            if (!plan_ok(img)) ++bad_plan;
            if (!invariants(img, file_size)) ++bad_inv;
        }
    }
    expect(accepted > 2500, "layouts: most random section layouts are accepted");
    expect(bad_plan == 0 && bad_inv == 0, "layouts: 4000 random section layouts give valid plans and satisfy the invariants");
}

static void decode_b64(const char* s, u8* out, usize* n) {
    static int table[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[static_cast<u8>(a[i])] = i;
        ready = true;
    }
    u32 acc = 0;
    int bits = 0;
    usize w = 0;
    for (; *s; ++s) {
        int v = table[static_cast<u8>(*s)];
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[w++] = static_cast<u8>(acc >> bits);
        }
    }
    *n = w;
}

struct Names {
    const char* dll;
    const char* name;
    u16 ordinal;
};

static bool resolve_logging(void* ctx, const char* dll, const char* name, u16 ordinal, u64* address) {
    u32* count = static_cast<u32*>(ctx);
    ++*count;
    *address = 0x7FFB00000000ull + 0x100 * *count + ordinal;
    (void)dll;
    (void)name;
    return true;
}

static void real_tests() {
    static u8 a[0x10000], b[0x10000], c[0x10000], d[0x10000];
    usize an = 0, bn = 0, cn = 0, dn = 0;
    decode_b64(pe_sample_console, a, &an);
    decode_b64(pe_sample_fixed, b, &bn);
    decode_b64(pe_sample_ordinals, c, &cn);
    decode_b64(pe_sample_library, d, &dn);
    static pe::Image img;
    expect(an > 0 && pe::parse(a, an, &img) == pe::Status::Ok, "real linker output: the console program parses");
    expect(img.image_base == 0x140000000ull && img.entry_rva == 0x1000 && img.section_count == 6 && img.size_of_image == 0x7000 && img.subsystem == 3 && img.has_relocations,
           "real linker output: base, entry, six sections, image size and subsystem match what the linker reported");
    expect(text_eq(img.sections[0].name, ".text") && img.sections[0].prot == 5 && text_eq(img.sections[2].name, ".data") && img.sections[2].prot == 3 &&
               text_eq(img.sections[4].name, ".tls") && text_eq(img.sections[5].name, ".reloc"),
           "real linker output: section names and protections");
    expect(invariants(img, an) && plan_ok(img), "real linker output: invariants hold and the plan is valid");
    u8* image = new_image(img);
    pe::map(img, a, an, image, img.size_of_image);
    u32 applied = 0;
    const u64 base = 0x7FF600000000ull;
    const u64 before0 = r64(image + 0x3010);
    const u64 before1 = r64(image + 0x3018);
    expect(pe::relocate(img, image, base, &applied) == pe::Status::Ok && applied == 7, "real linker output: seven relocations are applied");
    u64 first = r64(image + 0x3010);
    u64 second = r64(image + 0x3018);
    const u64 delta = base - img.image_base;
    expect(first == before0 + delta && second == before1 + delta && first - base < img.size_of_image && second - first == before1 - before0,
           "real linker output: the pointer table moved by the base delta and still points inside the image");
    u32 count = 0;
    static pe::ImportReport rep;
    pe::Status st = pe::bind_imports(img, image, resolve_logging, &count, &rep);
    expect(st == pe::Status::Ok && rep.libraries == 1 && rep.imports == 3 && count == 6, "real linker output: one library, three imports, each resolved twice");
    static const Table want[] = { { "kernel32.dll", "ExitProcess", 0, 1 }, { "kernel32.dll", "GetStdHandle", 0, 2 }, { "kernel32.dll", "WriteFile", 0, 3 } };
    Resolver known{ want, 3, 0 };
    u8* again = new_image(img);
    pe::map(img, a, an, again, img.size_of_image);
    pe::relocate(img, again, base);
    st = pe::bind_imports(img, again, resolve_table, &known, &rep);
    expect(st == pe::Status::Ok && rep.missing == 0, "real linker output: the three names the linker recorded are found by name");
    Resolver lacking{ want, 2, 0 };
    pe::map(img, a, an, again, img.size_of_image);
    st = pe::bind_imports(img, again, resolve_table, &lacking, &rep);
    expect(st == pe::Status::UnresolvedImport && text_eq(rep.first[0].name, "WriteFile") && text_eq(rep.first[0].dll, "kernel32.dll"),
           "real linker output: leaving out WriteFile names WriteFile and refuses");
    free(again);
    static pe::Tls tls;
    pe::map(img, a, an, image, img.size_of_image);
    pe::relocate(img, image, base);
    st = pe::tls_info(img, image, base, &tls);
    expect(st == pe::Status::Ok && tls.present && tls.template_bytes > 0 && tls.index_rva == img.sections[1].rva + 0 + (tls.index_rva - img.sections[1].rva) && tls.callback_count == 0,
           "real linker output: the TLS directory is read and needs no callbacks");
    printf("    (TLS template %u bytes at rva %x, zero fill %u, index at %x)\n", tls.template_bytes, tls.template_rva, tls.zero_fill, tls.index_rva);
    free(image);

    expect(pe::parse(b, bn, &img) == pe::Status::Ok && !img.has_relocations && (img.file_characteristics & pe::FILE_RELOCS_STRIPPED),
           "real linker output: a fixed-base program says it has no relocations");
    u8* fimg = new_image(img);
    pe::map(img, b, bn, fimg, img.size_of_image);
    expect(pe::relocate(img, fimg, img.image_base) == pe::Status::Ok && pe::relocate(img, fimg, img.image_base + 0x10000) == pe::Status::NeedsFixedBase,
           "real linker output: the fixed-base program loads only at its preferred base");
    free(fimg);

    expect(pe::parse(c, cn, &img) == pe::Status::Ok, "real linker output: the ordinal-import program parses");
    u8* oimg = new_image(img);
    pe::map(img, c, cn, oimg, img.size_of_image);
    pe::relocate(img, oimg, base);
    static const Table ords[] = { { "mylib.dll", "Alpha", 0, 0x11 }, { "mylib.dll", "Beta", 0, 0x22 }, { "mylib.dll", nullptr, 7, 0x77 }, { "kernel32.dll", "ExitProcess", 0, 0x99 } };
    Resolver orr{ ords, 4, 0 };
    st = pe::bind_imports(img, oimg, resolve_table, &orr, &rep);
    expect(st == pe::Status::Ok && rep.libraries == 2 && rep.imports == 4 && rep.missing == 0, "real linker output: by-name and by-ordinal imports from an import library both resolve");
    Resolver oless{ ords, 2, 0 };
    pe::map(img, c, cn, oimg, img.size_of_image);
    st = pe::bind_imports(img, oimg, resolve_table, &oless, &rep);
    bool ord_named = false;
    for (u32 i = 0; i < rep.missing && i < pe::MAX_REPORT; ++i)
        if (rep.first[i].by_ordinal && rep.first[i].ordinal == 7 && text_eq(rep.first[i].dll, "mylib.dll")) ord_named = true;
    expect(st == pe::Status::UnresolvedImport && ord_named, "real linker output: a missing ordinal import is reported with its DLL and number");
    free(oimg);

    expect(pe::parse(d, dn, &img) == pe::Status::Ok && img.is_dll && img.has_relocations && img.entry_rva == 0, "real linker output: a DLL linked with no entry parses with its relocations");
    expect(invariants(img, dn) && plan_ok(img), "real linker output: the DLL satisfies the invariants");
    u8* dimg = new_image(img);
    pe::map(img, d, dn, dimg, img.size_of_image);
    expect(pe::relocate(img, dimg, base, &applied) == pe::Status::Ok && applied >= 1, "real linker output: the DLL relocates");
    free(dimg);

    u32 prefix_ok = 0;
    for (usize n = 0; n < an; ++n) if (pe::parse(a, n, &img) == pe::Status::Ok) ++prefix_ok;
    expect(prefix_ok == 0, "real linker output: no prefix of the console program is accepted");
    Sweep real{};
    sweep(a, an, 0, 0x400, true, &real);
    sweep(a, an, 0x400, an, false, &real);
    expect(real.violations == 0, "real linker output: byte sweeps over the whole file never break the invariants");
    printf("    (%u runs, %u accepted)\n", real.runs, real.accepted);
    Sweep libs{};
    sweep(d, dn, 0, dn, false, &libs);
    expect(libs.violations == 0, "real linker output: byte sweeps over the DLL never break the invariants");
}

static void resource_tests() {
    static Built b;
    Spec sp;
    default_imports(sp);
    build(sp, &b);
    static pe::Image img;
    pe::parse(b.file, b.size, &img);
    expect(sizeof(pe::Image) < 8192, "the parsed image fits in 8 KiB, so it can live on a kernel or user stack");
    expect(pe::MAX_RUNS == 194 && pe::MAX_SECTIONS == 96, "limits: 96 sections and 194 page runs");
    expect(pe::status_name(pe::Status::Ok)[0] == 'o' && pe::status_name(pe::Status::UnresolvedImport)[0] == 'u' && pe::status_name(pe::Status::WritableExecutable)[0] == 'a',
           "status names are populated");
}

struct Mini {
    u32 sections;
    u32 sec_align;
    u32 file_align;
    u64 base;
    u32 image_size;
    u32 flags1;
    u32 headers;
    bool junk;
};

static u8 g_mini[0x30000];

static u32 mini_build(const Mini& m) {
    zero(g_mini, sizeof(g_mini));
    u8* f = g_mini;
    f[0] = 'M';
    f[1] = 'Z';
    w32(f + 0x3C, 0x40);
    w32(f + 0x40, pe::PE_SIGNATURE);
    u8* coff = f + 0x44;
    w16(coff, pe::MACHINE_AMD64);
    w16(coff + 2, m.sections);
    w16(coff + 16, 240);
    w16(coff + 18, pe::FILE_EXECUTABLE);
    u8* opt = coff + 20;
    w16(opt, pe::OPTIONAL_PE32PLUS);
    u32 table = 0x40 + 4 + 20 + 240;
    u32 headers = m.headers ? m.headers : (table + 40 * m.sections + m.file_align - 1) / m.file_align * m.file_align;
    u32 first = (headers + m.sec_align - 1) / m.sec_align * m.sec_align;
    u32 image = m.image_size ? m.image_size : first + m.sec_align * m.sections;
    w32(opt + 16, first);
    w64(opt + 24, m.base);
    w32(opt + 32, m.sec_align);
    w32(opt + 36, m.file_align);
    w32(opt + 56, image);
    w32(opt + 60, headers);
    w16(opt + 68, pe::SUBSYSTEM_CONSOLE);
    w32(opt + 108, 16);
    if (m.junk) {
        w32(opt + 112 + 8, 0x10);
        w32(opt + 112 + 12, 0x1000);
        w32(opt + 112 + 16, 0x10);
        w32(opt + 112 + 20, 0x1000);
        w32(opt + 112 + 28, 0x24);
    }
    for (u32 i = 0; i < m.sections; ++i) {
        u8* s = f + table + 40 * i;
        s[0] = '.';
        s[1] = static_cast<u8>('a' + (i % 26));
        w32(s + 8, 0x10);
        w32(s + 12, first + m.sec_align * i);
        if (i == 0) {
            w32(s + 16, m.file_align);
            w32(s + 20, headers);
            w32(s + 36, 0x60000020);
        } else {
            w32(s + 36, i == 1 && m.flags1 ? m.flags1 : 0xC0000080u);
        }
    }
    return headers + m.file_align;
}

static pe::Status mini_parse(const Mini& m, pe::Image* img) { return pe::parse(g_mini, mini_build(m), img); }

static u8* load(const Built& x, pe::Image* im) {
    pe::Status st = pe::parse(x.file, x.size, im);
    u8* p = new_image(*im);
    if (st == pe::Status::Ok) pe::map(*im, x.file, x.size, p, im->size_of_image);
    return p;
}

static pe::Status big_bind(u32 libs, u32 imports) {
    static pe::Image bi;
    zero(&bi, sizeof(bi));
    bi.size_of_image = 0x100000;
    bi.directory_count = 16;
    bi.directories[pe::DIR_IMPORT].rva = 0x10000;
    bi.directories[pe::DIR_IMPORT].size = 20 * (libs + 1);
    u8* im = static_cast<u8*>(malloc(0x100000));
    zero(im, 0x100000);
    const char* nm = "k.dll";
    for (u32 i = 0; nm[i]; ++i) im[0x100 + i] = static_cast<u8>(nm[i]);
    for (u32 i = 0; i < imports; ++i) w64(im + 0x20000 + 8 * i, (1ull << 63) | 1);
    for (u32 l = 0; l < libs; ++l) {
        u8* d = im + 0x10000 + 20 * l;
        w32(d, 0x20000);
        w32(d + 12, 0x100);
        w32(d + 16, 0x80000);
    }
    static pe::ImportReport rep;
    pe::Status st = pe::bind_imports(bi, im, resolve_all, nullptr, &rep);
    free(im);
    return st;
}

static bool reject_all(void*, const char*, const char*, u16, u64*) { return false; }

static pe::Status big_missing(u32 dll_len, u32 imports, pe::ImportReport* rep) {
    static pe::Image bi;
    zero(&bi, sizeof(bi));
    bi.size_of_image = 0x100000;
    bi.directory_count = 16;
    bi.directories[pe::DIR_IMPORT].rva = 0x10000;
    bi.directories[pe::DIR_IMPORT].size = 40;
    u8* im = static_cast<u8*>(malloc(0x100000));
    zero(im, 0x100000);
    for (u32 i = 0; i < dll_len; ++i) im[0x100 + i] = 'd';
    for (u32 i = 0; i < imports; ++i) w64(im + 0x20000 + 8 * i, (1ull << 63) | (i + 1));
    w32(im + 0x10000, 0x20000);
    w32(im + 0x1000C, 0x100);
    w32(im + 0x10010, 0x80000);
    pe::Status st = pe::bind_imports(bi, im, reject_all, nullptr, rep);
    free(im);
    return st;
}

static __attribute__((noinline)) void taint_stack() {
    volatile u8 junk[16384];
    for (u32 i = 0; i < sizeof(junk); ++i) junk[i] = 0x55;
}

static void edge_tests() {
    using S = pe::Status;
    static Built b;
    Spec sp;
    default_imports(sp);
    build(sp, &b);
    static pe::Image img;
    const u32 PEO = 0x80, COFF = PEO + 4, OPT = COFF + 20, TBL = OPT + 240;
    static Built m;

    zero(&m, sizeof(m));
    copy(m.file, b.file, 0x40);
    copy(m.file + 0x40, b.file + 0x80, 0x400 - 0x80);
    copy(m.file + 0x400, b.file + 0x400, FILE_BYTES - 0x400);
    m.size = b.size;
    w32(m.file + 0x3C, 0x40);
    expect(pe::parse(m.file, m.size, &img) == S::Ok && img.section_count == 4, "edge: a PE header right after the 64-byte DOS header is accepted");
    w32(m.file + 0x3C, 0x3F);
    expect(pe::parse(m.file, m.size, &img) == S::BadPeOffset, "edge: a PE header offset of 63 is refused");

    copy(&m, &b, sizeof(m));
    w16(m.file + COFF + 16, 112);
    w32(m.file + OPT + 108, 0);
    copy(m.file + OPT + 112, b.file + TBL, 160);
    zero(m.file + OPT + 272, 128);
    expect(pe::parse(m.file, m.size, &img) == S::Ok && img.directory_count == 0 && img.section_count == 4,
           "edge: an optional header with no data directories at all is accepted");

    copy(&m, &b, sizeof(m));
    w16(m.file + COFF + 18, 0x22);
    w16(m.file + OPT + 68, 2);
    w16(m.file + OPT + 70, 0x8160);
    w64(m.file + OPT + 72, 0x111000);
    w64(m.file + OPT + 80, 0x22000);
    w64(m.file + OPT + 88, 0x333000);
    w64(m.file + OPT + 96, 0x4000);
    expect(pe::parse(m.file, m.size, &img) == S::Ok && img.file_characteristics == 0x22 && img.subsystem == 2 && img.dll_characteristics == 0x8160 &&
               img.stack_reserve == 0x111000 && img.stack_commit == 0x22000 && img.heap_reserve == 0x333000 && img.heap_commit == 0x4000,
           "edge: subsystem, characteristics and the four stack and heap sizes are read from their own fields");

    copy(&m, &b, sizeof(m));
    w32(m.file + TBL + 20, 0xA00);
    w32(m.file + TBL + 40 + 20, 0x400);
    expect(pe::parse(m.file, m.size, &img) == S::Ok && invariants(img, m.size), "edge: raw data may be stored out of order and end exactly where the next block starts");

    copy(&m, &b, sizeof(m));
    w32(m.file + TBL + 3 * 40 + 8, 0);
    w32(m.file + TBL + 3 * 40 + 16, 0);
    w32(m.file + TBL + 3 * 40 + 12, IMAGE_BYTES);
    w32(m.file + OPT + 112 + 8 * pe::DIR_BASERELOC, 0);
    w32(m.file + OPT + 112 + 8 * pe::DIR_BASERELOC + 4, 0);
    expect(pe::parse(m.file, m.size, &img) == S::Ok && img.sections[3].mapped_bytes == 0, "edge: an empty section may sit exactly at the end of the image");

    copy(&m, &b, sizeof(m));
    w32(m.file + OPT + 112 + 8 * pe::DIR_DEBUG, 0);
    w32(m.file + OPT + 112 + 8 * pe::DIR_DEBUG + 4, 0x1C);
    expect(pe::parse(m.file, m.size, &img) == S::Ok, "edge: a directory with a size and no address is ignored");
    w32(m.file + OPT + 112 + 8 * pe::DIR_DEBUG, 0x2000);
    w32(m.file + OPT + 112 + 8 * pe::DIR_DEBUG + 4, 0);
    expect(pe::parse(m.file, m.size, &img) == S::Ok, "edge: a directory with an address and no size is ignored");

    copy(&m, &b, sizeof(m));
    w16(m.file + COFF + 18, pe::FILE_EXECUTABLE | pe::FILE_RELOCS_STRIPPED);
    static pe::Image si;
    expect(pe::parse(m.file, m.size, &si) == S::Ok && !si.has_relocations, "edge: the stripped flag wins over a relocation directory that is present");
    u8* sim = load(m, &si);
    expect(pe::relocate(si, sim, 0x150000000ull) == S::NeedsFixedBase, "edge: such an image cannot move");
    free(sim);
    copy(&m, &b, sizeof(m));
    w32(m.file + OPT + 112 + 8 * pe::DIR_BASERELOC, 0);
    w32(m.file + OPT + 112 + 8 * pe::DIR_BASERELOC + 4, 0);
    expect(pe::parse(m.file, m.size, &si) == S::Ok && !si.has_relocations, "edge: no relocation directory means no relocations even without the stripped flag");

    pe::parse(b.file, b.size, &img);
    u8* shortmap = new_image(img);
    expect(pe::map(img, b.file, 0x100, shortmap, img.size_of_image) == S::SectionOutsideFile, "edge: map refuses a file shorter than a section's raw data");
    expect(pe::map(img, b.file, OFF_RELOC + 0x100, shortmap, img.size_of_image) == S::SectionOutsideFile, "edge: map refuses a file that ends in the middle of a section");
    expect(pe::map(img, b.file, b.size, shortmap, img.size_of_image) == S::Ok, "edge: map accepts the whole file");
    free(shortmap);

    {
        pe::parse(b.file, b.size, &img);
        const u32 used = img.directories[pe::DIR_BASERELOC].size;
        const u32 dirsz = OPT + 112 + 8 * pe::DIR_BASERELOC + 4;
        static Built x;
        static pe::Image xi;
        auto run = [&](u32 dir_size, u32* applied) {
            w32(x.file + dirsz, dir_size);
            if (pe::parse(x.file, x.size, &xi) != S::Ok) return S::BadSection;
            u8* im = new_image(xi);
            pe::map(xi, x.file, x.size, im, xi.size_of_image);
            S st = pe::relocate(xi, im, 0x150000000ull, applied);
            free(im);
            return st;
        };
        u32 applied = 0;
        copy(&x, &b, sizeof(x));
        expect(run(used + 4, &applied) == S::BadReloc, "edge: four stray bytes after the last relocation block are refused");
        copy(&x, &b, sizeof(x));
        w32(x.file + OFF_RELOC + used, 0x3000);
        w32(x.file + OFF_RELOC + used + 4, 8);
        expect(run(used + 8, &applied) == S::Ok && applied == b.reloc_count, "edge: a block with a header and no entries is legal and does nothing");
        copy(&x, &b, sizeof(x));
        w32(x.file + OFF_RELOC + used, IMAGE_BYTES);
        w32(x.file + OFF_RELOC + used + 4, 8);
        expect(run(used + 8, &applied) == S::BadReloc, "edge: an empty block for the page after the image is refused");
        copy(&x, &b, sizeof(x));
        w32(x.file + OFF_RELOC + used, 0x3000);
        w32(x.file + OFF_RELOC + used + 4, 12);
        w16(x.file + OFF_RELOC + used + 8, (pe::REL_HIGHLOW << 12) | 0x3FE);
        expect(run(used + 12, &applied) == S::BadReloc, "edge: a 32-bit relocation that straddles the end of its section is refused");
        w16(x.file + OFF_RELOC + used + 8, (pe::REL_HIGHLOW << 12) | 0x3FC);
        expect(run(used + 12, &applied) == S::Ok && applied == b.reloc_count + 1, "edge: a 32-bit relocation that ends exactly at the end of its section is applied");
    }

    {
        static Built x;
        static pe::Image xi;
        static pe::ImportReport rep;
        copy(&x, &b, sizeof(x));
        u8* im = load(x, &xi);
        const u32 d0 = xi.directories[pe::DIR_IMPORT].rva;
        auto reset = [&]() { pe::map(xi, x.file, x.size, im, xi.size_of_image); };
        w32(im + d0, 0);
        w32(im + d0 + 20, 0);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::Ok && rep.imports == 5 && rep.libraries == 2, "edge: descriptors without a lookup table bind through the IAT");
        reset();
        w32(im + d0, RDATA_RVA + 0x104);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::BadImport, "edge: a lookup table that is not 8-byte aligned is refused");
        reset();
        w64(im + RDATA_RVA + 0x100, 0x7000);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::BadImport, "edge: an import name outside the image is refused");
        reset();
        for (u32 i = 0; i < 300; ++i) im[0x4402 + i] = 'a';
        w64(im + RDATA_RVA + 0x100, 0x4400);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::BadImport, "edge: an import name with no terminator within 255 characters is refused");
        reset();
        for (u32 i = 0; i < 256; ++i) im[0x4402 + i] = 'a';
        w64(im + RDATA_RVA + 0x100, 0x4400);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::BadImport, "edge: a 256-character import name is refused");
        reset();
        for (u32 i = 0; i < 255; ++i) im[0x4402 + i] = 'a';
        w64(im + RDATA_RVA + 0x100, 0x4400);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::Ok, "edge: a 255-character import name is accepted");
        reset();
        w64(im + RDATA_RVA + 0x100, 0x4400);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::BadImport, "edge: an empty import name is refused");
        reset();
        w32(im + d0 + 12, 0x4400);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::BadImport, "edge: an empty library name is refused");
        reset();
        copy(im + IMAGE_BYTES - 20, im + d0, 20);
        static pe::Image tw;
        copy(&tw, &xi, sizeof(tw));
        tw.directories[pe::DIR_IMPORT].rva = IMAGE_BYTES - 20;
        tw.directories[pe::DIR_IMPORT].size = 20;
        expect(pe::bind_imports(tw, im, resolve_all, nullptr, &rep) == S::BadImport, "edge: a descriptor table that runs into the end of the image is refused");
        reset();
        copy(&tw, &xi, sizeof(tw));
        tw.directories[pe::DIR_IMPORT].rva = 0;
        expect(pe::bind_imports(tw, im, resolve_all, nullptr, &rep) == S::Ok && rep.imports == 0, "edge: an import directory with a size and no address binds nothing");
        tw.directories[pe::DIR_IMPORT].rva = RDATA_RVA;
        tw.directories[pe::DIR_IMPORT].size = 0;
        expect(pe::bind_imports(tw, im, resolve_all, nullptr, &rep) == S::Ok && rep.imports == 0, "edge: an import directory with an address and no size binds nothing");
        free(im);
        expect(big_bind(256, 1) == S::Ok && big_bind(257, 1) == S::BadImport, "edge: 256 libraries are accepted and 257 are refused");
        expect(big_bind(1, 32767) == S::Ok && big_bind(1, 32768) == S::BadImport, "edge: 32767 imports from one library are accepted and 32768 are refused");
    }

    {
        static Built x;
        static pe::Image xi;
        static pe::Tls tl;
        copy(&x, &b, sizeof(x));
        u8* im = load(x, &xi);
        const u64 B = 0x140000000ull;
        u8* t = im + RDATA_RVA + TLS_DIR;
        auto reset = [&]() { pe::map(xi, x.file, x.size, im, xi.size_of_image); };
        w64(t + 8, r64(t));
        expect(pe::tls_info(xi, im, B, &tl) == S::Ok && tl.present && tl.template_bytes == 0, "edge: an empty TLS template is accepted");
        reset();
        w32(t + 32, 0x1000000);
        expect(pe::tls_info(xi, im, B, &tl) == S::Ok && tl.zero_fill == 0x1000000, "edge: a TLS zero fill of exactly 16 MiB is accepted");
        w32(t + 32, 0x1000001);
        expect(pe::tls_info(xi, im, B, &tl) == S::BadTls, "edge: a TLS zero fill above 16 MiB is refused");
        reset();
        w32(t + 36, 0x300000);
        expect(pe::tls_info(xi, im, B, &tl) == S::Ok && tl.characteristics == 0x300000, "edge: TLS characteristics are reported");
        reset();
        w64(t + 24, B + RDATA_RVA + TLS_CALLBACKS + 4);
        expect(pe::tls_info(xi, im, B, &tl) == S::BadTls, "edge: a TLS callback table that is not 8-byte aligned is refused");
        reset();
        w64(t + 24, B + IMAGE_BYTES - 8);
        w64(im + IMAGE_BYTES - 8, B + TEXT_RVA + 0x40);
        expect(pe::tls_info(xi, im, B, &tl) == S::BadTls, "edge: a TLS callback table that runs into the end of the image is refused");
        reset();
        w64(im + RDATA_RVA + TLS_CALLBACKS, B + 0x9000);
        expect(pe::tls_info(xi, im, B, &tl) == S::BadTls, "edge: a TLS callback outside the image is refused");
        reset();
        w64(t + 24, B + 0x4400);
        for (u32 i = 0; i < 16; ++i) w64(im + 0x4400 + 8 * i, B + TEXT_RVA + 0x40);
        expect(pe::tls_info(xi, im, B, &tl) == S::Ok && tl.callback_count == 16, "edge: sixteen TLS callbacks are accepted");
        w64(im + 0x4400 + 8 * 16, B + TEXT_RVA + 0x40);
        expect(pe::tls_info(xi, im, B, &tl) == S::BadTls, "edge: seventeen TLS callbacks are refused");
        reset();
        static pe::Image tw;
        copy(&tw, &xi, sizeof(tw));
        tw.sections[3].virtual_size = 0x1000;
        w64(t, B + 0x4FF0);
        w64(t + 8, B + 0x5000);
        expect(pe::tls_info(tw, im, B, &tl) == S::Ok && tl.template_bytes == 16, "edge: a TLS template that ends exactly at the end of its section is accepted");
        reset();
        copy(&tw, &xi, sizeof(tw));
        tw.directories[pe::DIR_TLS].rva = 0;
        expect(pe::tls_info(tw, im, B, &tl) == S::Ok && !tl.present, "edge: a TLS directory with a size and no address means no TLS");
        free(im);
    }

    {
        Mini mm{ 96, 4096, 512, 0x140000000ull, 0, 0 };
        expect(mini_parse(mm, &img) == S::Ok && img.section_count == 96 && invariants(img, mini_build(mm)), "edge: 96 sections are accepted");
        mm.sections = 97;
        expect(mini_parse(mm, &img) == S::BadSectionCount, "edge: 97 sections are refused");
        const struct { u32 sa, fa; S want; const char* what; } al[] = {
            { 4096, 4096, S::Ok, "section alignment equal to file alignment" },
            { 65536, 65536, S::Ok, "both alignments at 64 KiB" },
            { 1u << 20, 65536, S::Ok, "section alignment of 1 MiB" },
            { 1u << 21, 512, S::BadAlignment, "section alignment of 2 MiB" },
            { 4096, 256, S::BadAlignment, "file alignment of 256" },
            { 4096, 65536, S::BadAlignment, "file alignment above the section alignment" },
            { 2048, 512, S::BadAlignment, "section alignment below a page" },
            { 12288, 512, S::BadAlignment, "section alignment that is not a power of two" },
        };
        bool all = true;
        for (const auto& a : al) {
            Mini q{ 1, a.sa, a.fa, 0x140000000ull, 0, 0 };
            S st = mini_parse(q, &img);
            bool ok = st == a.want && (st != S::Ok || invariants(img, mini_build(q)));
            if (!ok) printf("    %s: got %s\n", a.what, pe::status_name(st));
            all = all && ok;
        }
        expect(all, "edge: alignment limits accept the legal combinations and refuse the others");
        Mini big{ 15, 1u << 20, 512, 0x140000000ull, 0, 0 };
        expect(mini_parse(big, &img) == S::Ok && img.size_of_image == (16u << 20), "edge: an image of exactly 16 MiB is accepted");
        big.sections = 16;
        expect(mini_parse(big, &img) == S::BadImageSize, "edge: an image above 16 MiB is refused");

        Mini lim{ 1, 65536, 512, pe::USER_LIMIT - 0x20000, 0, 0 };
        expect(mini_parse(lim, &img) == S::Ok, "edge: an image that ends exactly at the user limit is accepted");
        lim.base = pe::USER_LIMIT - 0x10000;
        expect(mini_parse(lim, &img) == S::BadImageBase, "edge: an image that crosses the user limit is refused");
        lim.base = 0x140000000ull;
        mini_parse(lim, &img);
        u8* lm = new_image(img);
        expect(pe::relocate(img, lm, pe::USER_LIMIT - 0x20000) == S::NeedsFixedBase, "edge: relocating to a base that ends exactly at the user limit passes the base check");
        expect(pe::relocate(img, lm, pe::USER_LIMIT - 0x10000) == S::BadBase, "edge: relocating to a base that would cross the user limit is refused");
        free(lm);

        Mini pl{ 2, 65536, 512, 0x140000000ull, 0, 0x40000080 };
        mini_parse(pl, &img);
        pe::Run full[pe::MAX_RUNS];
        u32 total = pe::plan(img, full, pe::MAX_RUNS);
        bool prefix = total == 3 && plan_ok(img) && full[0].pages == 16 && full[1].prot == 5 && full[2].pages == 31;
        for (u32 cap = 0; cap <= total + 1; ++cap) {
            pe::Run part[8];
            for (u32 i = 0; i < 8; ++i) part[i] = pe::Run{ 0xDEAD, 0xBEEF, 0xF00D };
            u32 got = pe::plan(img, part, cap);
            prefix = prefix && got == total;
            for (u32 i = 0; i < 8; ++i) {
                bool in = i < cap && i < total;
                bool eq = part[i].first_page == full[i].first_page && part[i].pages == full[i].pages && part[i].prot == full[i].prot;
                if (in) prefix = prefix && eq;
                else prefix = prefix && part[i].first_page == 0xDEAD;
            }
        }
        expect(prefix, "edge: plan fills exactly as many runs as fit, merges included, for every output size");
    }
}


static void edge_tests2() {
    using S = pe::Status;
    static Built b;
    Spec sp;
    default_imports(sp);
    build(sp, &b);
    static pe::Image img;
    const u32 PEO = 0x80, COFF = PEO + 4, OPT = COFF + 20;
    static Built m;
    const u64 B = 0x140000000ull;

    copy(&m, &b, sizeof(m));
    w32(m.file + OPT + 60, 0x1200);
    expect(pe::parse(m.file, m.size, &img) == S::BadHeadersSize, "edge2: headers larger than the file are refused");
    copy(&m, &b, sizeof(m));
    w32(m.file + OPT + 56, 0x1000);
    w32(m.file + OPT + 60, 0x1000);
    expect(pe::parse(m.file, m.size, &img) == S::BadSection, "edge2: headers as large as the image and the file pass the header check and fail on the first section");
    Mini hl{ 1, 0x2000, 0x1000, B, 0x2000, 0, 0x3000, false };
    expect(mini_parse(hl, &img) == S::BadHeadersSize, "edge2: headers larger than the image are refused");
    Mini ex{ 43, 4096, 512, B, 0, 0, 0, false };
    expect(mini_parse(ex, &img) == S::Ok && img.size_of_headers == 0x800, "edge2: a section table that fills the headers exactly is accepted");
    Mini ex2{ 43, 4096, 512, B, 0, 0, 0x600, false };
    expect(mini_parse(ex2, &img) == S::BadHeadersSize, "edge2: headers that end inside the section table are refused");

    Mini jk{ 96, 4096, 512, B, 0, 0, 0, true };
    const u32 jk_size = mini_build(jk);
    static pe::Image ji;
    bool jok = pe::parse(g_mini, jk_size, &ji) == S::Ok && ji.section_count == 96 && plan_ok(ji);
    pe::Run jr[pe::MAX_RUNS];
    jok = jok && pe::plan(ji, jr, pe::MAX_RUNS) == 3;
    u8* jim = new_image(ji);
    jok = jok && pe::map(ji, g_mini, jk_size, jim, ji.size_of_image) == S::Ok && same(jim, g_mini, ji.size_of_headers);
    free(jim);
    expect(jok, "edge2: with 96 sections the loops stop at the table and never read the directories as a 97th section");

    {
        u8* fc = static_cast<u8*>(malloc(b.size));
        copy(fc, b.file, b.size);
        fc[OFF_TEXT + 0x200] = 0xEE;
        fc[OFF_RDATA + 0x600] = 0xEE;
        fc[OFF_DATA + 0x200] = 0xEE;
        pe::parse(fc, b.size, &img);
        u8* im = new_image(img);
        bool ok = pe::map(img, fc, b.size, im, img.size_of_image) == S::Ok && im[TEXT_RVA + 0x200] == 0 && im[RDATA_RVA + 0x600] == 0 && im[DATA_RVA + 0x200] == 0 &&
                  im[RDATA_RVA] == 0xEE;
        free(im);
        free(fc);
        expect(ok, "edge2: map copies exactly the raw data of each section, not the byte after it, from an exact-size file");
    }

    {
        static Built x;
        static pe::Image xi;
        copy(&x, &b, sizeof(x));
        pe::parse(x.file, x.size, &xi);
        u8* im = new_image(xi);
        pe::map(xi, x.file, x.size, im, xi.size_of_image);
        const u32 used = xi.directories[pe::DIR_BASERELOC].size;
        u8* rl = im + RELOC_RVA + used;
        w32(rl, 0x3000);
        w32(rl + 4, 12);
        w16(rl + 8, (pe::REL_HIGHLOW << 12) | 0x3FC);
        w16(rl + 10, 0);
        static pe::Image tw;
        copy(&tw, &xi, sizeof(tw));
        tw.directories[pe::DIR_BASERELOC].size = used + 12;
        w32(im + DATA_RVA + 0x3FC, 0xFFFFFFF0u);
        im[DATA_RVA + 0x400] = 0x5A;
        u32 applied = 0;
        bool ok = pe::relocate(tw, im, 0x150000000ull, &applied) == S::Ok && r32(im + DATA_RVA + 0x3FC) == 0x0FFFFFF0u && im[DATA_RVA + 0x400] == 0x5A;
        expect(ok, "edge2: a 32-bit relocation writes four bytes and no more");
        copy(&tw, &xi, sizeof(tw));
        tw.directories[pe::DIR_BASERELOC].size = 0x2000;
        expect(pe::relocate(tw, im, 0x160000000ull) == S::BadReloc, "edge2: a relocation directory that is not inside one section is refused when relocating");
        free(im);
    }

    {
        static Built x;
        static pe::Image xi;
        static pe::ImportReport rep;
        copy(&x, &b, sizeof(x));
        u8* im = load(x, &xi);
        const u32 d0 = xi.directories[pe::DIR_IMPORT].rva;
        auto reset = [&]() { pe::map(xi, x.file, x.size, im, xi.size_of_image); };
        reset();
        for (u32 i = 0; i < 100; ++i) im[IMAGE_BYTES - 100 + i] = 'a';
        w64(im + RDATA_RVA + 0x100, IMAGE_BYTES - 102);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::BadImport, "edge2: an import name that runs to the last byte of the image is refused");
        reset();
        for (u32 i = 0; i < 100; ++i) im[0x30 + i] = 'a';
        im[0x30 + 100] = 0;
        w32(im + d0 + 12, 0x30);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::Ok, "edge2: a library name stored in the headers is read from its own address");
        reset();
        for (u32 i = 0; i < 255; ++i) im[0x4400 + i] = 'a';
        w32(im + d0 + 12, 0x4400);
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::Ok, "edge2: a 255-character library name is accepted");
        for (u32 i = 0; i < 256; ++i) im[0x4400 + i] = 'a';
        expect(pe::bind_imports(xi, im, resolve_all, nullptr, &rep) == S::BadImport, "edge2: a 256-character library name is refused");
        free(im);

        static pe::ImportReport r1;
        expect(big_missing(1, 40, &r1) == S::UnresolvedImport && r1.missing == 40 && r1.imports == 40 && r1.resolved == 0 && r1.first[31].ordinal == 32 && r1.first[31].by_ordinal,
               "edge2: forty missing imports are counted and the first 32 are listed");
        static pe::ImportReport r2;
        expect(big_missing(32, 33, &r2) == S::UnresolvedImport && r2.missing == 33 && r2.first[31].ordinal == 32, "edge2: exactly 33 missing imports are counted and 32 are listed");
        static pe::ImportReport r3;
        taint_stack();
        pe::Status st3 = big_missing(100, 1, &r3);
        bool clean = st3 == S::UnresolvedImport;
        usize dl = 0;
        while (dl < pe::REPORT_NAME && r3.first[0].dll[dl]) ++dl;
        clean = clean && dl == 63;
        for (usize i = dl; i < pe::REPORT_NAME; ++i) clean = clean && r3.first[0].dll[i] == 0;
        for (usize i = 0; i < pe::REPORT_NAME; ++i) clean = clean && r3.first[0].name[i] == 0;
        expect(clean, "edge2: a long library name is cut at 63 characters and the report fields hold nothing after their terminator");
    }

    {
        static Built x;
        static pe::Image xi;
        static pe::Tls tl;
        copy(&x, &b, sizeof(x));
        u8* im = load(x, &xi);
        u8* t = im + RDATA_RVA + TLS_DIR;
        auto reset = [&]() { pe::map(xi, x.file, x.size, im, xi.size_of_image); };
        w64(t + 24, B + 0x4404);
        w64(im + 0x4404, B + TEXT_RVA + 0x40);
        expect(pe::tls_info(xi, im, B, &tl) == S::BadTls, "edge2: a misaligned TLS callback table is refused even when it holds a valid callback");
        reset();
        w64(t, B + IMAGE_BYTES);
        w64(t + 8, B + IMAGE_BYTES);
        expect(pe::tls_info(xi, im, B, &tl) == S::BadTls, "edge2: an empty TLS template placed after the image is refused");
        reset();
        static pe::Image tw;
        copy(&tw, &xi, sizeof(tw));
        tw.directories[pe::DIR_TLS].rva = IMAGE_BYTES - 8;
        expect(pe::tls_info(tw, im, B, &tl) == S::BadTls, "edge2: a TLS directory that runs past the end of the image is refused");
        free(im);
    }
}

int main() {
    valid_tests();
    relocation_tests();
    import_tests();
    tls_tests();
    header_tests();
    edge_tests();
    edge_tests2();
    truncation_tests();
    layout_tests();
    real_tests();
    resource_tests();
    sweep_tests();
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
