#pragma once
#include <cosmos/pe.hpp>

namespace pehost {

constexpr i64 STATUS_INVALID_IMAGE_FORMAT = static_cast<i32>(0xC000007Bu);
constexpr i64 STATUS_CONFLICTING_ADDRESSES = static_cast<i32>(0xC0000018u);
constexpr i64 STATUS_NO_MEMORY = static_cast<i32>(0xC0000017u);
constexpr i64 STATUS_ENTRYPOINT_NOT_FOUND = static_cast<i32>(0xC0000139u);

constexpr u64 PAGE = 4096;
constexpr u64 MIN_STACK = 0x10000;
constexpr u64 MAX_STACK = 0x100000;
constexpr u64 MAX_TLS_BYTES = 0x100000;
constexpr u64 MAX_TLS_ALIGN = 4096;
constexpr u64 SUPPORT_HEAD = 0x4000;
constexpr u32 TLS_SLOTS = 64;
constexpr u32 REASON_PROCESS_ATTACH = 1;

struct Teb {
    u64 exception_list;
    u64 stack_base;
    u64 stack_limit;
    u64 sub_system_tib;
    u64 fiber_data;
    u64 arbitrary_user_pointer;
    u64 self;
    u64 environment_pointer;
    u64 client_process;
    u64 client_thread;
    u64 active_rpc_handle;
    u64 tls_pointer;
    u64 peb;
    u32 last_error;
    u32 reserved;
    u8 gap[0x1480 - 0x70];
    u64 tls_slots[TLS_SLOTS];
};

struct Peb {
    u8 inherited_address_space;
    u8 read_image_file_exec_options;
    u8 being_debugged;
    u8 bit_field;
    u32 padding;
    u64 mutant;
    u64 image_base_address;
    u64 ldr;
    u64 process_parameters;
    u64 sub_system_data;
    u64 process_heap;
};

static_assert(__builtin_offsetof(Teb, stack_base) == 0x08 && __builtin_offsetof(Teb, stack_limit) == 0x10);
static_assert(__builtin_offsetof(Teb, self) == 0x30 && __builtin_offsetof(Teb, client_process) == 0x40);
static_assert(__builtin_offsetof(Teb, tls_pointer) == 0x58 && __builtin_offsetof(Teb, peb) == 0x60);
static_assert(__builtin_offsetof(Teb, last_error) == 0x68);
static_assert(__builtin_offsetof(Peb, being_debugged) == 0x02 && __builtin_offsetof(Peb, image_base_address) == 0x10);
static_assert(__builtin_offsetof(Peb, process_parameters) == 0x20 && __builtin_offsetof(Peb, process_heap) == 0x30);
static_assert(__builtin_offsetof(Teb, tls_slots) == 0x1480);
static_assert(sizeof(Teb) <= 2 * PAGE && sizeof(Peb) < PAGE);

struct Import {
    const char* dll;
    const char* name;
    void* address;
};

struct ImportTable {
    const Import* items;
    u32 count;
    const ImportTable* next = nullptr;
};

struct Prepared {
    pe::Image image;
    pe::Tls tls;
    pe::ImportReport report;
    u8* base;
    u8* support;
    u8* stack_base;
    u64 stack_bytes;
    Teb* teb;
    Peb* peb;
    u32 relocations;
};

namespace detail {

struct Line {
    char text[220];
    u32 length;
    Line() : text{}, length(0) {}
    Line& add(const char* s) {
        while (s && *s && length + 1 < sizeof(text)) text[length++] = *s++;
        return *this;
    }
    Line& dec(u64 v) {
        char digits[20];
        u32 n = 0;
        do {
            digits[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (n && length + 1 < sizeof(text)) text[length++] = digits[--n];
        return *this;
    }
    const char* done() {
        text[length] = 0;
        return text;
    }
};

inline char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

inline bool same_dll(const char* a, const char* b) {
    while (*a && fold(*a) == fold(*b)) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

inline bool same_name(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

inline bool resolve(void* ctx, const char* dll, const char* name, u16, u64* address) {
    if (name == nullptr) return false;
    for (const ImportTable* table = static_cast<const ImportTable*>(ctx); table; table = table->next) {
        for (u32 i = 0; i < table->count; ++i) {
            const Import& e = table->items[i];
            if (same_dll(e.dll, dll) && same_name(e.name, name)) {
                *address = reinterpret_cast<u64>(e.address);
                return true;
            }
        }
    }
    return false;
}

constexpr u64 up(u64 v, u64 a) { return (v + a - 1) / a * a; }

}

template <class P>
i64 refuse(P& p, i64 status, const char* what, const char* detail = nullptr) {
    detail::Line l;
    l.add("[pehost] ").add(what);
    if (detail) l.add(": ").add(detail);
    p.log(l.done());
    return status;
}

template <class P>
i64 load(P& p, const u8* file, usize size, const ImportTable& imports, Prepared* out) {
    u8* raw = reinterpret_cast<u8*>(out);
    for (usize i = 0; i < sizeof(*out); ++i) raw[i] = 0;
    pe::Status st = pe::parse(file, size, &out->image);
    if (st != pe::Status::Ok) return refuse(p, STATUS_INVALID_IMAGE_FORMAT, "image refused", pe::status_name(st));
    const pe::Image& img = out->image;
    if (img.is_dll) return refuse(p, STATUS_INVALID_IMAGE_FORMAT, "image refused", "a DLL cannot be started as a program");
    if (!img.relocatable) return refuse(p, STATUS_CONFLICTING_ADDRESSES, "image refused", "it must load at its preferred base, which user space cannot map");

    u8* base = p.reserve(img.size_of_image, true);
    if (!base) return refuse(p, STATUS_NO_MEMORY, "no memory for the image");
    out->base = base;
    st = pe::map(img, file, size, base, img.size_of_image);
    if (st != pe::Status::Ok) return refuse(p, STATUS_INVALID_IMAGE_FORMAT, "image refused", pe::status_name(st));
    st = pe::relocate(img, base, reinterpret_cast<u64>(base), &out->relocations);
    if (st != pe::Status::Ok) return refuse(p, STATUS_CONFLICTING_ADDRESSES, "relocation refused", pe::status_name(st));

    st = pe::bind_imports(img, base, detail::resolve, const_cast<ImportTable*>(&imports), &out->report);
    if (st == pe::Status::UnresolvedImport) {
        u32 listed = out->report.missing < pe::MAX_REPORT ? out->report.missing : pe::MAX_REPORT;
        for (u32 i = 0; i < listed; ++i) {
            const pe::Missing& m = out->report.first[i];
            detail::Line l;
            l.add("[pehost] missing import ").add(m.dll).add("!");
            if (m.by_ordinal) l.add("#").dec(m.ordinal);
            else l.add(m.name);
            p.log(l.done());
        }
        detail::Line l;
        l.add("[pehost] ").dec(out->report.missing).add(" of ").dec(out->report.imports).add(" imports are not provided");
        p.log(l.done());
        return STATUS_ENTRYPOINT_NOT_FOUND;
    }
    if (st != pe::Status::Ok) return refuse(p, STATUS_INVALID_IMAGE_FORMAT, "imports refused", pe::status_name(st));

    st = pe::tls_info(img, base, reinterpret_cast<u64>(base), &out->tls);
    if (st != pe::Status::Ok) return refuse(p, STATUS_INVALID_IMAGE_FORMAT, "TLS refused", pe::status_name(st));
    u64 tls_bytes = 0;
    if (out->tls.present) {
        u64 align = (out->tls.characteristics >> 20) & 15;
        if (align != 0 && (1ull << (align - 1)) > MAX_TLS_ALIGN) return refuse(p, STATUS_INVALID_IMAGE_FORMAT, "TLS refused", "alignment above one page");
        tls_bytes = static_cast<u64>(out->tls.template_bytes) + out->tls.zero_fill;
        if (tls_bytes > MAX_TLS_BYTES) return refuse(p, STATUS_INVALID_IMAGE_FORMAT, "TLS refused", "more than 1 MiB per thread");
    }

    u64 support_bytes = SUPPORT_HEAD + detail::up(tls_bytes, PAGE);
    u8* support = p.reserve(support_bytes, false);
    if (!support) return refuse(p, STATUS_NO_MEMORY, "no memory for the thread and process blocks");
    out->support = support;

    u64 stack_bytes = img.stack_reserve;
    if (stack_bytes < MIN_STACK) stack_bytes = MIN_STACK;
    if (stack_bytes > MAX_STACK) stack_bytes = MAX_STACK;
    stack_bytes = detail::up(stack_bytes, PAGE);
    u8* stack = p.reserve(stack_bytes, false);
    if (!stack) return refuse(p, STATUS_NO_MEMORY, "no memory for the stack");
    out->stack_base = stack;
    out->stack_bytes = stack_bytes;

    Teb* teb = reinterpret_cast<Teb*>(support);
    Peb* peb = reinterpret_cast<Peb*>(support + 2 * PAGE);
    u64* tls_array = reinterpret_cast<u64*>(support + 3 * PAGE);
    u8* tls_block = support + SUPPORT_HEAD;
    if (out->tls.present) {
        const u8* src = base + out->tls.template_rva;
        for (u32 i = 0; i < out->tls.template_bytes; ++i) tls_block[i] = src[i];
        u8* slot = base + out->tls.index_rva;
        slot[0] = slot[1] = slot[2] = slot[3] = 0;
        tls_array[0] = reinterpret_cast<u64>(tls_block);
    }
    peb->image_base_address = reinterpret_cast<u64>(base);
    teb->stack_base = reinterpret_cast<u64>(stack + stack_bytes);
    teb->stack_limit = reinterpret_cast<u64>(stack);
    teb->self = reinterpret_cast<u64>(teb);
    teb->client_process = 1;
    teb->client_thread = p.thread_id();
    teb->tls_pointer = reinterpret_cast<u64>(tls_array);
    teb->peb = reinterpret_cast<u64>(peb);
    out->teb = teb;
    out->peb = peb;

    static pe::Run runs[pe::MAX_RUNS];
    u32 run_count = pe::plan(img, runs, pe::MAX_RUNS);
    for (u32 i = 0; i < run_count; ++i) {
        if (!p.protect(base + static_cast<u64>(runs[i].first_page) * PAGE, static_cast<u64>(runs[i].pages) * PAGE, runs[i].prot))
            return refuse(p, STATUS_NO_MEMORY, "the image could not be protected", "the address space has too many mappings");
    }
    return 0;
}

template <class P>
i64 start(P& p, const Prepared& run) {
    if (!p.set_teb(run.teb)) return refuse(p, STATUS_NO_MEMORY, "the thread block could not be installed");
    void* top = run.stack_base + run.stack_bytes;
    u64 module = reinterpret_cast<u64>(run.base);
    for (u32 i = 0; i < run.tls.callback_count; ++i)
        p.call(reinterpret_cast<void*>(run.tls.callbacks[i]), module, REASON_PROCESS_ATTACH, 0, top);
    u64 result = p.call(run.base + run.image.entry_rva, reinterpret_cast<u64>(run.peb), 0, 0, top);
    return static_cast<i32>(static_cast<u32>(result));
}

}
