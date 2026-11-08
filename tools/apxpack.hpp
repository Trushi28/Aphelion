#pragma once
#include <cosmos/apx.hpp>
#include <cosmos/userabi.hpp>

namespace apxpack {

enum class Status : u8 {
    Ok = 0,
    TooSmall,
    NotElf,
    NotElf64,
    NotLittleEndian,
    NotExecutable,
    NotX86_64,
    BadProgramHeaders,
    UnsupportedSegment,
    NoCode,
    TwoCodeSegments,
    TwoDataSegments,
    CodeAddress,
    DataAddress,
    CodeHasBss,
    SegmentOutsideFile,
    CodeTooLarge,
    DataTooLarge,
    EntryOutsideCode,
    PackFailed,
};

constexpr const char* status_name(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::TooSmall: return "file is too small to be an ELF";
        case Status::NotElf: return "not an ELF file";
        case Status::NotElf64: return "not a 64-bit ELF";
        case Status::NotLittleEndian: return "not little-endian";
        case Status::NotExecutable: return "not a statically linked executable";
        case Status::NotX86_64: return "not an x86-64 ELF";
        case Status::BadProgramHeaders: return "program headers are missing or outside the file";
        case Status::UnsupportedSegment: return "a loadable segment is neither read-execute nor read-write";
        case Status::NoCode: return "no executable segment";
        case Status::TwoCodeSegments: return "more than one executable segment";
        case Status::TwoDataSegments: return "more than one writable segment";
        case Status::CodeAddress: return "the code segment is not at the APX code base";
        case Status::DataAddress: return "the data segment is not at the APX data base";
        case Status::CodeHasBss: return "the code segment has memory beyond its file bytes";
        case Status::SegmentOutsideFile: return "a segment's bytes are outside the file";
        case Status::CodeTooLarge: return "code does not fit in the APX code limit";
        case Status::DataTooLarge: return "data does not fit in the APX data limit";
        case Status::EntryOutsideCode: return "the entry point is outside the code segment";
        case Status::PackFailed: return "the output buffer is too small";
    }
    return "unknown";
}

constexpr u32 PT_LOAD = 1;
constexpr u32 PF_X = 1;
constexpr u32 PF_W = 2;
constexpr u32 PF_R = 4;

inline u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline u32 rd32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
inline u64 rd64(const u8* p) { return rd32(p) | (static_cast<u64>(rd32(p + 4)) << 32); }

inline bool range_ok(u64 off, u64 len, u64 total) { return off <= total && len <= total - off; }

inline Status convert(const u8* elf, usize size, u8* out, usize cap, usize* written) {
    *written = 0;
    if (size < 64) return Status::TooSmall;
    if (elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') return Status::NotElf;
    if (elf[4] != 2) return Status::NotElf64;
    if (elf[5] != 1) return Status::NotLittleEndian;
    if (rd16(elf + 16) != 2) return Status::NotExecutable;
    if (rd16(elf + 18) != 62) return Status::NotX86_64;
    u64 entry = rd64(elf + 24);
    u64 phoff = rd64(elf + 32);
    u32 phentsize = rd16(elf + 54);
    u32 phnum = rd16(elf + 56);
    if (phnum == 0 || phentsize < 56 || phnum > 64) return Status::BadProgramHeaders;
    if (!range_ok(phoff, static_cast<u64>(phentsize) * phnum, size)) return Status::BadProgramHeaders;

    bool have_code = false, have_data = false;
    u64 code_off = 0, code_size = 0, code_vaddr = 0;
    u64 data_off = 0, data_filesz = 0, data_memsz = 0, data_vaddr = 0;
    for (u32 i = 0; i < phnum; ++i) {
        const u8* ph = elf + phoff + static_cast<u64>(phentsize) * i;
        if (rd32(ph) != PT_LOAD) continue;
        u32 flags = rd32(ph + 4);
        u64 off = rd64(ph + 8);
        u64 vaddr = rd64(ph + 16);
        u64 filesz = rd64(ph + 32);
        u64 memsz = rd64(ph + 40);
        if (memsz == 0 && filesz == 0) continue;
        u32 rwx = flags & (PF_R | PF_W | PF_X);
        if (rwx == (PF_R | PF_X)) {
            if (have_code) return Status::TwoCodeSegments;
            if (vaddr != userabi::CODE_BASE) return Status::CodeAddress;
            if (memsz != filesz) return Status::CodeHasBss;
            if (!range_ok(off, filesz, size)) return Status::SegmentOutsideFile;
            have_code = true;
            code_off = off;
            code_size = filesz;
            code_vaddr = vaddr;
        } else if (rwx == (PF_R | PF_W)) {
            if (have_data) return Status::TwoDataSegments;
            if (vaddr != userabi::DATA_BASE) return Status::DataAddress;
            if (filesz > memsz) return Status::BadProgramHeaders;
            if (!range_ok(off, filesz, size)) return Status::SegmentOutsideFile;
            have_data = true;
            data_off = off;
            data_filesz = filesz;
            data_memsz = memsz;
            data_vaddr = vaddr;
        } else {
            return Status::UnsupportedSegment;
        }
    }
    (void)data_vaddr;
    if (!have_code) return Status::NoCode;
    if (code_size > apx::MAX_CODE_BYTES) return Status::CodeTooLarge;
    u64 data_pages = have_data ? (data_memsz + apx::PAGE - 1) / apx::PAGE : 0;
    if (data_pages > userabi::MAX_DATA_PAGES) return Status::DataTooLarge;
    if (entry < code_vaddr || entry - code_vaddr >= code_size) return Status::EntryOutsideCode;
    u32 data_bytes = static_cast<u32>(data_filesz);
    usize total = static_cast<usize>(apx::pack_size(code_size, data_bytes));
    if (total > cap) return Status::PackFailed;
    if (!apx::pack(elf + code_off, static_cast<u32>(code_size), static_cast<u32>(entry - code_vaddr), static_cast<u32>(data_pages),
                   have_data ? elf + data_off : nullptr, data_bytes, out, cap, written))
        return Status::PackFailed;
    return Status::Ok;
}

}
