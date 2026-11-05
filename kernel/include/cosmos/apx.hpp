#pragma once
#include <cosmos/types.hpp>
#include <cosmos/userabi.hpp>

namespace apx {

constexpr u32 MAGIC = 0x31585041u;
constexpr u16 VERSION = 1;
constexpr u32 HEADER_BYTES = 64;
constexpr u64 PAGE = 4096;
constexpr u64 MAX_CODE_BYTES = static_cast<u64>(userabi::MAX_CODE_PAGES) * PAGE;
constexpr u64 MAX_DATA_BYTES = static_cast<u64>(userabi::MAX_DATA_PAGES) * PAGE;
constexpr u64 MAX_FILE_BYTES = HEADER_BYTES + MAX_CODE_BYTES + MAX_DATA_BYTES;
constexpr u32 SECTION_ALIGN = 16;

enum class Status : u8 {
    Ok = 0,
    Truncated,
    BadMagic,
    BadVersion,
    BadHeaderSize,
    BadFlags,
    BadReserved,
    BadCodeRange,
    CodeTooLarge,
    BadEntry,
    TooManyDataPages,
    BadDataRange,
    DataTooLarge,
    Misaligned,
    Overlap,
    SizeMismatch,
    BadChecksum,
};

constexpr const char* status_name(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::Truncated: return "image is shorter than its header";
        case Status::BadMagic: return "not an APX image";
        case Status::BadVersion: return "unsupported APX version";
        case Status::BadHeaderSize: return "bad header size";
        case Status::BadFlags: return "unknown flags set";
        case Status::BadReserved: return "reserved bytes are not zero";
        case Status::BadCodeRange: return "code range is outside the file";
        case Status::CodeTooLarge: return "code is too large";
        case Status::BadEntry: return "entry point is outside the code";
        case Status::TooManyDataPages: return "too many data pages";
        case Status::BadDataRange: return "data range is outside the file";
        case Status::DataTooLarge: return "initial data does not fit its pages";
        case Status::Misaligned: return "section is not aligned";
        case Status::Overlap: return "sections overlap";
        case Status::SizeMismatch: return "file size does not match the header";
        case Status::BadChecksum: return "checksum mismatch";
    }
    return "unknown";
}

struct Header {
    u32 flags;
    u32 code_offset;
    u32 code_bytes;
    u32 entry_offset;
    u32 data_pages;
    u32 data_offset;
    u32 data_bytes;
    u32 file_bytes;
    u32 checksum;
};

constexpr u32 rd32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

constexpr u16 rd16(const u8* p) { return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8)); }

constexpr void wr32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}

constexpr void wr16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}

struct Crc {
    u32 state;
    constexpr Crc() : state(0xFFFFFFFFu) {}
    constexpr void feed(const u8* p, usize n) {
        u32 s = state;
        for (usize i = 0; i < n; ++i) {
            s ^= p[i];
            for (int k = 0; k < 8; ++k) s = (s >> 1) ^ (0xEDB88320u & (0u - (s & 1u)));
        }
        state = s;
    }
    constexpr u32 value() const { return ~state; }
};

constexpr u32 crc32(const u8* p, usize n) {
    Crc c;
    c.feed(p, n);
    return c.value();
}

struct Parsed {
    Status status;
    Header header;
};

constexpr bool range_ok(u64 off, u64 len, u64 total) { return off <= total && len <= total - off; }

constexpr Status check_header(const Header& h, u64 file_size) {
    if (h.flags != 0) return Status::BadFlags;
    if (h.code_bytes == 0) return Status::BadCodeRange;
    if (h.code_bytes > MAX_CODE_BYTES) return Status::CodeTooLarge;
    if (h.data_pages > userabi::MAX_DATA_PAGES) return Status::TooManyDataPages;
    if (h.data_bytes > static_cast<u64>(h.data_pages) * PAGE) return Status::DataTooLarge;
    if (h.entry_offset >= h.code_bytes) return Status::BadEntry;
    if (h.code_offset % SECTION_ALIGN != 0 || h.data_offset % SECTION_ALIGN != 0) return Status::Misaligned;
    if (h.code_offset < HEADER_BYTES) return Status::BadCodeRange;
    if (file_size > MAX_FILE_BYTES) return Status::SizeMismatch;
    if (h.file_bytes != file_size) return Status::SizeMismatch;
    if (!range_ok(h.code_offset, h.code_bytes, file_size)) return Status::BadCodeRange;
    u64 code_end = static_cast<u64>(h.code_offset) + h.code_bytes;
    u64 end = code_end;
    if (h.data_bytes == 0) {
        if (h.data_offset != 0) return Status::BadDataRange;
    } else {
        if (!range_ok(h.data_offset, h.data_bytes, file_size)) return Status::BadDataRange;
        if (h.data_offset < HEADER_BYTES) return Status::BadDataRange;
        u64 data_end = static_cast<u64>(h.data_offset) + h.data_bytes;
        bool disjoint = data_end <= h.code_offset || h.data_offset >= code_end;
        if (!disjoint) return Status::Overlap;
        if (data_end > end) end = data_end;
    }
    if (end != file_size) return Status::SizeMismatch;
    return Status::Ok;
}

constexpr Parsed parse_header(const u8* bytes, usize available, u64 file_size) {
    Parsed r{ Status::Ok, Header{} };
    if (available < HEADER_BYTES || file_size < HEADER_BYTES) { r.status = Status::Truncated; return r; }
    if (rd32(bytes) != MAGIC) { r.status = Status::BadMagic; return r; }
    if (rd16(bytes + 4) != VERSION) { r.status = Status::BadVersion; return r; }
    if (rd16(bytes + 6) != HEADER_BYTES) { r.status = Status::BadHeaderSize; return r; }
    for (u32 i = 44; i < HEADER_BYTES; ++i) {
        if (bytes[i] != 0) { r.status = Status::BadReserved; return r; }
    }
    r.header.flags = rd32(bytes + 8);
    r.header.code_offset = rd32(bytes + 12);
    r.header.code_bytes = rd32(bytes + 16);
    r.header.entry_offset = rd32(bytes + 20);
    r.header.data_pages = rd32(bytes + 24);
    r.header.data_offset = rd32(bytes + 28);
    r.header.data_bytes = rd32(bytes + 32);
    r.header.file_bytes = rd32(bytes + 36);
    r.header.checksum = rd32(bytes + 40);
    r.status = check_header(r.header, file_size);
    return r;
}

constexpr Parsed parse(const u8* bytes, usize size) {
    Parsed r = parse_header(bytes, size, size);
    if (r.status != Status::Ok) return r;
    if (crc32(bytes + HEADER_BYTES, size - HEADER_BYTES) != r.header.checksum) r.status = Status::BadChecksum;
    return r;
}

constexpr usize encode(const Header& h, u8* out) {
    for (u32 i = 0; i < HEADER_BYTES; ++i) out[i] = 0;
    wr32(out, MAGIC);
    wr16(out + 4, VERSION);
    wr16(out + 6, static_cast<u16>(HEADER_BYTES));
    wr32(out + 8, h.flags);
    wr32(out + 12, h.code_offset);
    wr32(out + 16, h.code_bytes);
    wr32(out + 20, h.entry_offset);
    wr32(out + 24, h.data_pages);
    wr32(out + 28, h.data_offset);
    wr32(out + 32, h.data_bytes);
    wr32(out + 36, h.file_bytes);
    wr32(out + 40, h.checksum);
    return HEADER_BYTES;
}

constexpr u64 align_up(u64 v, u64 a) { return (v + a - 1) / a * a; }

constexpr u64 pack_size(u64 code_bytes, u64 data_bytes) {
    u64 code_off = HEADER_BYTES;
    u64 end = code_off + code_bytes;
    if (data_bytes) end = align_up(end, SECTION_ALIGN) + data_bytes;
    return end;
}

constexpr bool pack(const u8* code, u32 code_bytes, u32 entry_offset, u32 data_pages, const u8* data, u32 data_bytes, u8* out,
                    usize cap, usize* written) {
    u64 total = pack_size(code_bytes, data_bytes);
    if (total > cap) return false;
    Header h{};
    h.code_offset = HEADER_BYTES;
    h.code_bytes = code_bytes;
    h.entry_offset = entry_offset;
    h.data_pages = data_pages;
    h.data_offset = 0;
    h.data_bytes = data_bytes;
    for (u64 i = 0; i < code_bytes; ++i) out[HEADER_BYTES + i] = code[i];
    u64 end = HEADER_BYTES + static_cast<u64>(code_bytes);
    if (data_bytes) {
        u64 doff = align_up(end, SECTION_ALIGN);
        for (u64 i = end; i < doff; ++i) out[i] = 0;
        for (u64 i = 0; i < data_bytes; ++i) out[doff + i] = data[i];
        h.data_offset = static_cast<u32>(doff);
        end = doff + data_bytes;
    }
    h.file_bytes = static_cast<u32>(end);
    h.checksum = crc32(out + HEADER_BYTES, static_cast<usize>(end - HEADER_BYTES));
    encode(h, out);
    *written = static_cast<usize>(end);
    return true;
}

}
