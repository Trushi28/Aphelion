#pragma once
#include <cosmos/types.hpp>

namespace pe {

constexpr u16 MZ_MAGIC = 0x5A4D;
constexpr u32 PE_SIGNATURE = 0x00004550;
constexpr u16 MACHINE_I386 = 0x014C;
constexpr u16 MACHINE_AMD64 = 0x8664;
constexpr u16 OPTIONAL_PE32 = 0x010B;
constexpr u16 OPTIONAL_PE32PLUS = 0x020B;

constexpr u16 FILE_RELOCS_STRIPPED = 0x0001;
constexpr u16 FILE_EXECUTABLE = 0x0002;
constexpr u16 FILE_DLL = 0x2000;

constexpr u16 DLLCHAR_DYNAMIC_BASE = 0x0040;
constexpr u16 DLLCHAR_NX_COMPAT = 0x0100;

constexpr u16 SUBSYSTEM_NATIVE = 1;
constexpr u16 SUBSYSTEM_GUI = 2;
constexpr u16 SUBSYSTEM_CONSOLE = 3;

constexpr u32 SCN_CNT_CODE = 0x00000020;
constexpr u32 SCN_CNT_INITIALIZED = 0x00000040;
constexpr u32 SCN_CNT_UNINITIALIZED = 0x00000080;
constexpr u32 SCN_MEM_DISCARDABLE = 0x02000000;
constexpr u32 SCN_MEM_EXECUTE = 0x20000000;
constexpr u32 SCN_MEM_READ = 0x40000000;
constexpr u32 SCN_MEM_WRITE = 0x80000000;

constexpr u32 DIR_EXPORT = 0;
constexpr u32 DIR_IMPORT = 1;
constexpr u32 DIR_RESOURCE = 2;
constexpr u32 DIR_EXCEPTION = 3;
constexpr u32 DIR_SECURITY = 4;
constexpr u32 DIR_BASERELOC = 5;
constexpr u32 DIR_DEBUG = 6;
constexpr u32 DIR_TLS = 9;
constexpr u32 DIR_LOAD_CONFIG = 10;
constexpr u32 DIR_BOUND_IMPORT = 11;
constexpr u32 DIR_IAT = 12;
constexpr u32 DIR_DELAY_IMPORT = 13;
constexpr u32 DIR_CLR = 14;
constexpr u32 DIR_COUNT = 16;

constexpr u32 REL_ABSOLUTE = 0;
constexpr u32 REL_HIGHLOW = 3;
constexpr u32 REL_DIR64 = 10;

constexpr u32 PROT_READ = 1;
constexpr u32 PROT_WRITE = 2;
constexpr u32 PROT_EXEC = 4;

constexpr u32 MAX_SECTIONS = 96;
constexpr u64 MAX_IMAGE_BYTES = 16ull * 1024 * 1024;
constexpr u64 PAGE = 4096;
constexpr u64 BASE_ALIGN = 0x10000;
constexpr u64 USER_LIMIT = 0x0000800000000000ull;
constexpr u32 MAX_RUNS = 2 * MAX_SECTIONS + 2;
constexpr u32 MAX_NAME = 255;
constexpr u32 MAX_LIBRARIES = 256;
constexpr u32 MAX_IMPORTS_PER_LIBRARY = 32768;
constexpr u32 MAX_REPORT = 32;
constexpr u32 REPORT_NAME = 64;
constexpr u32 MAX_TLS_CALLBACKS = 16;
constexpr u32 TLS_DIRECTORY_BYTES = 40;

enum class Status : u8 {
    Ok = 0,
    TooSmall,
    BadDosMagic,
    BadPeOffset,
    BadPeMagic,
    BadMachine,
    Pe32Unsupported,
    BadOptionalMagic,
    BadOptionalSize,
    BadDirectoryCount,
    NotExecutable,
    BadSectionCount,
    BadAlignment,
    BadImageSize,
    BadHeadersSize,
    BadImageBase,
    BadSection,
    SectionOverlap,
    SectionOutsideFile,
    WritableExecutable,
    BadEntry,
    BadDirectory,
    ManagedUnsupported,
    ImageBufferTooSmall,
    BadBase,
    NeedsFixedBase,
    BadReloc,
    BadRelocType,
    BadImport,
    UnresolvedImport,
    BadTls,
};

constexpr const char* status_name(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::TooSmall: return "file is too small";
        case Status::BadDosMagic: return "missing MZ header";
        case Status::BadPeOffset: return "PE header offset is outside the file";
        case Status::BadPeMagic: return "missing PE signature";
        case Status::BadMachine: return "not an x86-64 image";
        case Status::Pe32Unsupported: return "32-bit images are not supported";
        case Status::BadOptionalMagic: return "unknown optional header format";
        case Status::BadOptionalSize: return "optional header size is wrong";
        case Status::BadDirectoryCount: return "too many data directories";
        case Status::NotExecutable: return "not an executable image";
        case Status::BadSectionCount: return "bad section count";
        case Status::BadAlignment: return "unsupported alignment";
        case Status::BadImageSize: return "bad image size";
        case Status::BadHeadersSize: return "bad headers size";
        case Status::BadImageBase: return "bad image base";
        case Status::BadSection: return "bad section";
        case Status::SectionOverlap: return "sections overlap";
        case Status::SectionOutsideFile: return "section data is outside the file";
        case Status::WritableExecutable: return "a section is both writable and executable";
        case Status::BadEntry: return "entry point is not in executable code";
        case Status::BadDirectory: return "bad data directory";
        case Status::ManagedUnsupported: return ".NET images are not supported";
        case Status::ImageBufferTooSmall: return "image buffer is too small";
        case Status::BadBase: return "bad load address";
        case Status::NeedsFixedBase: return "image cannot be relocated";
        case Status::BadReloc: return "bad relocation";
        case Status::BadRelocType: return "unsupported relocation type";
        case Status::BadImport: return "bad import table";
        case Status::UnresolvedImport: return "unresolved import";
        case Status::BadTls: return "bad TLS directory";
    }
    return "unknown";
}

struct Directory {
    u32 rva;
    u32 size;
};

struct Section {
    char name[9];
    u32 rva;
    u32 virtual_size;
    u32 mapped_bytes;
    u32 raw_offset;
    u32 raw_size;
    u32 characteristics;
    u32 prot;
};

struct Image {
    u64 image_base;
    u32 entry_rva;
    u32 size_of_image;
    u32 size_of_headers;
    u32 section_alignment;
    u32 file_alignment;
    u16 subsystem;
    u16 dll_characteristics;
    u16 file_characteristics;
    u64 stack_reserve;
    u64 stack_commit;
    u64 heap_reserve;
    u64 heap_commit;
    u32 section_count;
    u32 directory_count;
    bool is_dll;
    bool has_relocations;
    bool relocatable;
    Section sections[MAX_SECTIONS];
    Directory directories[DIR_COUNT];
};

struct Run {
    u32 first_page;
    u32 pages;
    u32 prot;
};

struct Missing {
    char dll[REPORT_NAME];
    char name[REPORT_NAME];
    u16 ordinal;
    bool by_ordinal;
};

struct ImportReport {
    u32 libraries;
    u32 imports;
    u32 resolved;
    u32 missing;
    Missing first[MAX_REPORT];
};

struct Tls {
    bool present;
    u32 template_rva;
    u32 template_bytes;
    u32 zero_fill;
    u32 index_rva;
    u32 callback_count;
    u32 characteristics;
    u64 callbacks[MAX_TLS_CALLBACKS];
};

using ResolveFn = bool (*)(void* ctx, const char* dll, const char* name, u16 ordinal, u64* address);

namespace detail {

struct View {
    const u8* p;
    u64 n;
    constexpr bool has(u64 off, u64 len) const { return off <= n && len <= n - off; }
    constexpr u16 r16(u64 o) const { return static_cast<u16>(static_cast<u16>(p[o]) | (static_cast<u16>(p[o + 1]) << 8)); }
    constexpr u32 r32(u64 o) const {
        return static_cast<u32>(p[o]) | (static_cast<u32>(p[o + 1]) << 8) | (static_cast<u32>(p[o + 2]) << 16) | (static_cast<u32>(p[o + 3]) << 24);
    }
    constexpr u64 r64(u64 o) const { return static_cast<u64>(r32(o)) | (static_cast<u64>(r32(o + 4)) << 32); }
};

constexpr bool pow2(u64 v) { return v != 0 && (v & (v - 1)) == 0; }
constexpr u64 up(u64 v, u64 a) { return (v + a - 1) / a * a; }

constexpr u32 prot_of(u32 characteristics) {
    if (characteristics & SCN_MEM_EXECUTE) return PROT_READ | PROT_EXEC;
    if (characteristics & SCN_MEM_WRITE) return PROT_READ | PROT_WRITE;
    return PROT_READ;
}

constexpr u64 effective_size(u32 virtual_size, u32 raw_size) { return virtual_size ? virtual_size : raw_size; }

constexpr bool in_headers(const Image& img, u64 rva, u64 len) { return rva <= img.size_of_headers && len <= img.size_of_headers - rva; }

constexpr const Section* section_holding(const Image& img, u64 rva, u64 len) {
    for (u32 i = 0; i < img.section_count; ++i) {
        const Section& s = img.sections[i];
        u64 start = s.rva;
        u64 span = effective_size(s.virtual_size, s.raw_size);
        if (rva >= start && rva - start <= span && len <= span - (rva - start)) return &s;
    }
    return nullptr;
}

inline void copy_text(char* dst, u32 cap, const char* src) {
    u32 n = 0;
    while (src[n] && n + 1 < cap) {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = 0;
}

inline bool read_text(const u8* image, u64 bytes, u64 rva, u32 max, char* out) {
    for (u32 i = 0; i <= max; ++i) {
        if (rva + i >= bytes) return false;
        char c = static_cast<char>(image[rva + i]);
        out[i] = c;
        if (c == 0) return i > 0;
    }
    return false;
}

}

inline Status parse(const u8* file, usize size, Image* out) {
    *out = Image{};
    detail::View v{ file, size };
    if (size < 64) return Status::TooSmall;
    if (v.r16(0) != MZ_MAGIC) return Status::BadDosMagic;
    u64 lfanew = v.r32(0x3C);
    if (lfanew < 64 || !v.has(lfanew, 24)) return Status::BadPeOffset;
    if (v.r32(lfanew) != PE_SIGNATURE) return Status::BadPeMagic;

    u64 coff = lfanew + 4;
    u16 machine = v.r16(coff);
    u32 nsec = v.r16(coff + 2);
    u64 optsize = v.r16(coff + 16);
    u16 chars = v.r16(coff + 18);
    if (machine == MACHINE_I386) return Status::Pe32Unsupported;
    if (machine != MACHINE_AMD64) return Status::BadMachine;
    if (!(chars & FILE_EXECUTABLE)) return Status::NotExecutable;
    if (nsec == 0 || nsec > MAX_SECTIONS) return Status::BadSectionCount;

    u64 opt = coff + 20;
    if (optsize < 2 || !v.has(opt, optsize)) return Status::BadOptionalSize;
    u16 magic = v.r16(opt);
    if (magic == OPTIONAL_PE32) return Status::Pe32Unsupported;
    if (magic != OPTIONAL_PE32PLUS) return Status::BadOptionalMagic;
    if (optsize < 112) return Status::BadOptionalSize;
    u32 ndirs = v.r32(opt + 108);
    if (ndirs > DIR_COUNT) return Status::BadDirectoryCount;
    if (optsize < 112 + 8ull * ndirs) return Status::BadOptionalSize;

    u64 sec_align = v.r32(opt + 32);
    u64 file_align = v.r32(opt + 36);
    if (!detail::pow2(file_align) || file_align < 512 || file_align > 65536) return Status::BadAlignment;
    if (!detail::pow2(sec_align) || sec_align < PAGE || sec_align > (1u << 20) || sec_align < file_align) return Status::BadAlignment;

    u64 image_size = v.r32(opt + 56);
    u64 headers_size = v.r32(opt + 60);
    if (image_size == 0 || image_size % sec_align != 0 || image_size > MAX_IMAGE_BYTES) return Status::BadImageSize;
    u64 table = opt + optsize;
    u64 table_end = table + 40ull * nsec;
    if (headers_size == 0 || headers_size % file_align != 0 || headers_size > image_size || headers_size > size || headers_size < table_end)
        return Status::BadHeadersSize;
    if (!v.has(table, 40ull * nsec)) return Status::BadHeadersSize;

    u64 base = v.r64(opt + 24);
    if (base % BASE_ALIGN != 0 || base >= USER_LIMIT || image_size > USER_LIMIT - base) return Status::BadImageBase;

    Image& img = *out;
    img.image_base = base;
    img.entry_rva = v.r32(opt + 16);
    img.size_of_image = static_cast<u32>(image_size);
    img.size_of_headers = static_cast<u32>(headers_size);
    img.section_alignment = static_cast<u32>(sec_align);
    img.file_alignment = static_cast<u32>(file_align);
    img.subsystem = v.r16(opt + 68);
    img.dll_characteristics = v.r16(opt + 70);
    img.file_characteristics = chars;
    img.stack_reserve = v.r64(opt + 72);
    img.stack_commit = v.r64(opt + 80);
    img.heap_reserve = v.r64(opt + 88);
    img.heap_commit = v.r64(opt + 96);
    img.section_count = nsec;
    img.directory_count = ndirs;
    img.is_dll = (chars & FILE_DLL) != 0;

    u64 prev_end = detail::up(headers_size, sec_align);
    for (u32 i = 0; i < nsec; ++i) {
        u64 at = table + 40ull * i;
        Section& s = img.sections[i];
        for (u32 k = 0; k < 8; ++k) s.name[k] = static_cast<char>(file[at + k]);
        s.name[8] = 0;
        u64 vsize = v.r32(at + 8);
        u64 va = v.r32(at + 12);
        u64 raw = v.r32(at + 16);
        u64 raw_off = v.r32(at + 20);
        u32 sc = v.r32(at + 36);
        if (va % sec_align != 0) return Status::BadSection;
        if (va < prev_end) return Status::SectionOverlap;
        u64 eff = detail::effective_size(static_cast<u32>(vsize), static_cast<u32>(raw));
        u64 span = detail::up(eff, sec_align);
        if (va > image_size || span > image_size - va) return Status::BadSection;
        if (raw != 0) {
            if (raw_off % file_align != 0) return Status::BadSection;
            if (!v.has(raw_off, raw)) return Status::SectionOutsideFile;
            if (raw_off < headers_size) return Status::BadSection;
            for (u32 j = 0; j < i; ++j) {
                const Section& o = img.sections[j];
                if (o.raw_size == 0) continue;
                bool apart = raw_off + raw <= o.raw_offset || o.raw_offset + static_cast<u64>(o.raw_size) <= raw_off;
                if (!apart) return Status::SectionOverlap;
            }
        }
        if ((sc & SCN_MEM_EXECUTE) && (sc & SCN_MEM_WRITE)) return Status::WritableExecutable;
        s.rva = static_cast<u32>(va);
        s.virtual_size = static_cast<u32>(vsize);
        s.mapped_bytes = static_cast<u32>(detail::up(eff, PAGE));
        s.raw_offset = static_cast<u32>(raw_off);
        s.raw_size = static_cast<u32>(raw);
        s.characteristics = sc;
        s.prot = detail::prot_of(sc);
        prev_end = va + span;
    }

    for (u32 i = 0; i < ndirs; ++i) {
        u32 rva = v.r32(opt + 112 + 8ull * i);
        u32 dsize = v.r32(opt + 112 + 8ull * i + 4);
        img.directories[i] = Directory{ rva, dsize };
        if (i == DIR_SECURITY || rva == 0 || dsize == 0) continue;
        if (detail::section_holding(img, rva, dsize) == nullptr && !detail::in_headers(img, rva, dsize)) return Status::BadDirectory;
    }
    if (ndirs > DIR_CLR && img.directories[DIR_CLR].rva != 0 && img.directories[DIR_CLR].size != 0) return Status::ManagedUnsupported;
    if (ndirs > DIR_EXCEPTION && img.directories[DIR_EXCEPTION].size % 12 != 0) return Status::BadDirectory;
    if (ndirs > DIR_TLS && img.directories[DIR_TLS].rva != 0 && img.directories[DIR_TLS].size != 0 &&
        img.directories[DIR_TLS].size < TLS_DIRECTORY_BYTES)
        return Status::BadDirectory;
    img.has_relocations = !(chars & FILE_RELOCS_STRIPPED) && ndirs > DIR_BASERELOC && img.directories[DIR_BASERELOC].rva != 0 &&
                          img.directories[DIR_BASERELOC].size != 0;
    img.relocatable = !(chars & FILE_RELOCS_STRIPPED) && (img.has_relocations || (img.dll_characteristics & DLLCHAR_DYNAMIC_BASE) != 0);

    if (img.entry_rva != 0 || !img.is_dll) {
        const Section* holder = detail::section_holding(img, img.entry_rva, 1);
        if (img.entry_rva == 0 || holder == nullptr || !(holder->prot & PROT_EXEC)) return Status::BadEntry;
    }
    return Status::Ok;
}

inline u32 plan(const Image& img, Run* out, u32 cap) {
    u32 count = 0;
    u32 pages_total = img.size_of_image / static_cast<u32>(PAGE);
    u32 cursor = 0;
    Run last{ 0, 0, 0 };
    auto emit = [&](u32 first, u32 pages, u32 prot) {
        if (pages == 0) return;
        if (count > 0 && last.prot == prot && last.first_page + last.pages == first) {
            last.pages += pages;
            if (count <= cap) out[count - 1].pages = last.pages;
            return;
        }
        last = Run{ first, pages, prot };
        if (count < cap) out[count] = last;
        ++count;
    };
    u32 header_pages = static_cast<u32>(detail::up(img.size_of_headers, PAGE) / PAGE);
    emit(0, header_pages, PROT_READ);
    cursor = header_pages;
    for (u32 i = 0; i < img.section_count; ++i) {
        const Section& s = img.sections[i];
        u32 first = s.rva / static_cast<u32>(PAGE);
        u32 pages = s.mapped_bytes / static_cast<u32>(PAGE);
        if (pages == 0) continue;
        if (first > cursor) emit(cursor, first - cursor, PROT_READ);
        emit(first, pages, s.prot);
        cursor = first + pages;
    }
    if (pages_total > cursor) emit(cursor, pages_total - cursor, PROT_READ);
    return count;
}

inline Status map(const Image& img, const u8* file, usize size, u8* image, usize image_bytes) {
    if (image_bytes < img.size_of_image) return Status::ImageBufferTooSmall;
    for (u64 i = 0; i < img.size_of_image; ++i) image[i] = 0;
    u64 head = img.size_of_headers < size ? img.size_of_headers : size;
    for (u64 i = 0; i < head; ++i) image[i] = file[i];
    for (u32 k = 0; k < img.section_count; ++k) {
        const Section& s = img.sections[k];
        u64 eff = detail::effective_size(s.virtual_size, s.raw_size);
        u64 n = s.raw_size < eff ? s.raw_size : eff;
        if (n > size || s.raw_offset > size - n) return Status::SectionOutsideFile;
        for (u64 i = 0; i < n; ++i) image[static_cast<u64>(s.rva) + i] = file[static_cast<u64>(s.raw_offset) + i];
    }
    return Status::Ok;
}

inline Status relocate(const Image& img, u8* image, u64 actual_base, u32* applied = nullptr) {
    if (applied) *applied = 0;
    if (actual_base % BASE_ALIGN != 0 || actual_base >= USER_LIMIT || img.size_of_image > USER_LIMIT - actual_base) return Status::BadBase;
    if (actual_base == img.image_base) return Status::Ok;
    if (!img.relocatable) return Status::NeedsFixedBase;
    if (!img.has_relocations) return Status::Ok;
    u64 delta = actual_base - img.image_base;
    const Directory& d = img.directories[DIR_BASERELOC];
    if (detail::section_holding(img, d.rva, d.size) == nullptr) return Status::BadReloc;
    detail::View v{ image, img.size_of_image };
    u64 pos = d.rva;
    u64 end = static_cast<u64>(d.rva) + d.size;
    u32 count = 0;
    while (pos < end) {
        if (end - pos < 8) return Status::BadReloc;
        u64 page = v.r32(pos);
        u64 block = v.r32(pos + 4);
        if (block < 8 || block % 2 != 0 || block > end - pos) return Status::BadReloc;
        if (page % PAGE != 0 || page >= img.size_of_image) return Status::BadReloc;
        for (u64 at = pos + 8; at < pos + block; at += 2) {
            u32 entry = v.r16(at);
            u32 type = entry >> 12;
            u64 target = page + (entry & 0xFFF);
            if (type == REL_ABSOLUTE) continue;
            if (type == REL_DIR64) {
                if (detail::section_holding(img, target, 8) == nullptr) return Status::BadReloc;
                u64 val = v.r64(target) + delta;
                for (u32 k = 0; k < 8; ++k) image[target + k] = static_cast<u8>(val >> (8 * k));
            } else if (type == REL_HIGHLOW) {
                if (detail::section_holding(img, target, 4) == nullptr) return Status::BadReloc;
                u32 val = v.r32(target) + static_cast<u32>(delta);
                for (u32 k = 0; k < 4; ++k) image[target + k] = static_cast<u8>(val >> (8 * k));
            } else {
                return Status::BadRelocType;
            }
            ++count;
        }
        pos += block;
    }
    if (applied) *applied = count;
    return Status::Ok;
}

namespace detail {

struct Thunk {
    bool by_ordinal;
    u16 ordinal;
    u64 name_rva;
};

inline bool decode_thunk(const View& v, u64 raw, Thunk* t) {
    if (raw & (1ull << 63)) {
        if ((raw & 0x7FFFFFFFFFFF0000ull) != 0) return false;
        t->by_ordinal = true;
        t->ordinal = static_cast<u16>(raw & 0xFFFF);
        t->name_rva = 0;
        return true;
    }
    if (raw > 0x7FFFFFFFull) return false;
    t->by_ordinal = false;
    t->ordinal = 0;
    t->name_rva = raw;
    return v.has(raw, 3);
}

}

inline Status bind_imports(const Image& img, u8* image, ResolveFn resolve, void* ctx, ImportReport* report) {
    *report = ImportReport{};
    if (img.directory_count <= DIR_IMPORT) return Status::Ok;
    const Directory& d = img.directories[DIR_IMPORT];
    if (d.rva == 0 || d.size == 0) return Status::Ok;
    detail::View v{ image, img.size_of_image };
    Status result = Status::Ok;
    for (u32 pass = 0; pass < 2; ++pass) {
        bool write = pass == 1;
        if (write && report->missing != 0) break;
        if (write) {
            report->libraries = 0;
            report->imports = 0;
            report->resolved = 0;
        }
        u64 at = d.rva;
        for (u32 lib = 0;; ++lib) {
            if (!v.has(at, 20)) return Status::BadImport;
            u32 original = v.r32(at);
            u32 name_rva = v.r32(at + 12);
            u32 first = v.r32(at + 16);
            if (original == 0 && name_rva == 0 && first == 0 && v.r32(at + 4) == 0 && v.r32(at + 8) == 0) break;
            if (lib >= MAX_LIBRARIES) return Status::BadImport;
            char dll[MAX_NAME + 1];
            if (name_rva == 0 || !detail::read_text(image, img.size_of_image, name_rva, MAX_NAME, dll)) return Status::BadImport;
            if (first == 0 || first % 8 != 0) return Status::BadImport;
            u64 lookup = original != 0 ? original : first;
            if (lookup % 8 != 0) return Status::BadImport;
            ++report->libraries;
            for (u32 i = 0;; ++i) {
                if (i >= MAX_IMPORTS_PER_LIBRARY) return Status::BadImport;
                u64 src = lookup + 8ull * i;
                u64 slot = first + 8ull * i;
                if (!v.has(src, 8) || !v.has(slot, 8)) return Status::BadImport;
                u64 raw = v.r64(src);
                if (raw == 0) break;
                detail::Thunk t{};
                if (!detail::decode_thunk(v, raw, &t)) return Status::BadImport;
                char name[MAX_NAME + 1];
                name[0] = 0;
                if (!t.by_ordinal && !detail::read_text(image, img.size_of_image, t.name_rva + 2, MAX_NAME, name)) return Status::BadImport;
                ++report->imports;
                u64 address = 0;
                bool found = resolve(ctx, dll, t.by_ordinal ? nullptr : name, t.ordinal, &address);
                if (!found) {
                    if (!write) {
                        if (report->missing < MAX_REPORT) {
                            Missing& m = report->first[report->missing];
                            detail::copy_text(m.dll, REPORT_NAME, dll);
                            detail::copy_text(m.name, REPORT_NAME, name);
                            m.ordinal = t.ordinal;
                            m.by_ordinal = t.by_ordinal;
                        }
                        ++report->missing;
                    }
                    continue;
                }
                ++report->resolved;
                if (write) {
                    for (u32 k = 0; k < 8; ++k) image[slot + k] = static_cast<u8>(address >> (8 * k));
                }
            }
            at += 20;
        }
    }
    if (report->missing != 0) result = Status::UnresolvedImport;
    return result;
}

inline Status tls_info(const Image& img, const u8* image, u64 loaded_base, Tls* out) {
    *out = Tls{};
    if (img.directory_count <= DIR_TLS) return Status::Ok;
    const Directory& d = img.directories[DIR_TLS];
    if (d.rva == 0 || d.size == 0) return Status::Ok;
    detail::View v{ image, img.size_of_image };
    if (!v.has(d.rva, TLS_DIRECTORY_BYTES)) return Status::BadTls;
    u64 start = v.r64(d.rva);
    u64 end = v.r64(d.rva + 8);
    u64 index = v.r64(d.rva + 16);
    u64 callbacks = v.r64(d.rva + 24);
    u32 zero = v.r32(d.rva + 32);
    u32 chars = v.r32(d.rva + 36);
    auto rva_of = [&](u64 va, u64* rva) {
        if (va < loaded_base) return false;
        u64 r = va - loaded_base;
        if (r >= img.size_of_image) return false;
        *rva = r;
        return true;
    };
    u64 start_rva = 0, end_rva = 0, index_rva = 0;
    if (end < start || !rva_of(start, &start_rva)) return Status::BadTls;
    u64 bytes = end - start;
    if (bytes != 0 && (!rva_of(end - 1, &end_rva) || detail::section_holding(img, start_rva, bytes) == nullptr)) return Status::BadTls;
    if (!rva_of(index, &index_rva) || detail::section_holding(img, index_rva, 4) == nullptr) return Status::BadTls;
    if (static_cast<u64>(zero) > MAX_IMAGE_BYTES) return Status::BadTls;
    out->present = true;
    out->template_rva = static_cast<u32>(start_rva);
    out->template_bytes = static_cast<u32>(bytes);
    out->zero_fill = zero;
    out->index_rva = static_cast<u32>(index_rva);
    out->characteristics = chars;
    if (callbacks != 0) {
        u64 cb_rva = 0;
        if (!rva_of(callbacks, &cb_rva) || cb_rva % 8 != 0) return Status::BadTls;
        for (u32 i = 0;; ++i) {
            u64 at = cb_rva + 8ull * i;
            if (!v.has(at, 8)) return Status::BadTls;
            u64 fn = v.r64(at);
            if (fn == 0) break;
            if (i >= MAX_TLS_CALLBACKS) return Status::BadTls;
            u64 fn_rva = 0;
            if (!rva_of(fn, &fn_rva)) return Status::BadTls;
            const Section* holder = detail::section_holding(img, fn_rva, 1);
            if (holder == nullptr || !(holder->prot & PROT_EXEC)) return Status::BadTls;
            out->callbacks[out->callback_count++] = fn;
        }
    }
    return Status::Ok;
}

}
