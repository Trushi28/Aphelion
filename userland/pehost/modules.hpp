#pragma once
#include "link.hpp"

namespace pehost::modules {

constexpr u32 MAX_MODULES = 16;
constexpr u32 MAX_DEPENDENCIES = 16;
constexpr u32 MAX_NESTING = 4;
constexpr u32 MAX_HOPS = 8;
constexpr u32 NAME_BYTES = 64;
constexpr u32 TLS_LIMIT = 64;
constexpr u64 MAX_TLS_BYTES = 0x100000;
constexpr u32 PAGE = 4096;
constexpr u64 BASE_ALIGN = 0x10000;
constexpr u32 REASON_DETACH = 0;
constexpr u32 REASON_ATTACH = 1;
constexpr u32 ERROR_NO_MEMORY = 8;
constexpr u32 ERROR_NOT_FOUND = 126;
constexpr u32 ERROR_PROC_NOT_FOUND = 127;
constexpr u32 ERROR_BAD_IMAGE = 193;
constexpr u32 ERROR_INIT_FAILED = 1114;

struct Module {
    bool used;
    bool binding;
    bool attaching;
    bool attached;
    bool rooted;
    u32 refs;
    char key[NAME_BYTES];
    u8* base;
    u64 bytes;
    u8* raw;
    u64 raw_bytes;
    u32 entry_rva;
    pe::Directory exports;
    pe::Tls tls;
    u32 tls_index;
    u8* tls_block;
    u64 tls_bytes;
    u8 deps[MAX_DEPENDENCIES];
    u32 dep_count;
};

struct State {
    Module items[MAX_MODULES];
    u8 order[MAX_MODULES];
    u32 order_count;
    bool tls_taken[TLS_LIMIT];
    u64* tls_array;
    const ImportTable* builtins;
    u32 nesting;
    u32 cause;
    bool exiting;
};

inline State g_state;
inline pe::Image g_scratch[MAX_NESTING];
inline pe::Run g_runs[pe::MAX_RUNS];

struct BindContext {
    const ImportTable* builtins;
    i32 owner;
};

struct DirectCall {
    u64 operator()(u64 fn, u64 a, u64 b, u64 c) const { return reinterpret_cast<u64(PEHOST_WINAPI*)(u64, u64, u64)>(fn)(a, b, c); }
};

template <class P>
struct PlatformCall {
    P& platform;
    void* top;
    u64 operator()(u64 fn, u64 a, u64 b, u64 c) const { return platform.call(reinterpret_cast<void*>(fn), a, b, c, top); }
};

inline void reset(const ImportTable* builtins) {
    u8* raw = reinterpret_cast<u8*>(&g_state);
    for (usize i = 0; i < sizeof(g_state); ++i) raw[i] = 0;
    g_state.builtins = builtins;
}

inline bool normalise(const char* path, char* key, char* spelled) {
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    u32 n = 0;
    bool dotted = false;
    for (; base[n]; ++n) {
        if (n + 5 >= NAME_BYTES) return false;
        if (base[n] == '.') dotted = true;
        spelled[n] = base[n];
        key[n] = detail::fold(base[n]);
    }
    if (n == 0) return false;
    bool only_dots = true;
    for (u32 i = 0; i < n; ++i) {
        if (base[i] != '.') only_dots = false;
    }
    if (only_dots) return false;
    if (!dotted) {
        const char* ext = ".dll";
        for (u32 i = 0; i < 4; ++i, ++n) {
            spelled[n] = ext[i];
            key[n] = ext[i];
        }
    }
    spelled[n] = 0;
    key[n] = 0;
    return true;
}

inline i32 find(const char* key) {
    for (u32 i = 0; i < MAX_MODULES; ++i) {
        if (g_state.items[i].used && detail::same_name(g_state.items[i].key, key)) return static_cast<i32>(i);
    }
    return -1;
}

inline i32 find_base(u64 handle) {
    for (u32 i = 0; i < MAX_MODULES; ++i) {
        if (g_state.items[i].used && !g_state.items[i].binding && reinterpret_cast<u64>(g_state.items[i].base) == handle) return static_cast<i32>(i);
    }
    return -1;
}

inline bool resolve_import(const ImportTable* builtins, i32 owner, u32 hops, const char* dll, const char* name, u16 ordinal, u64* address);

inline bool add_dependency(i32 owner, i32 dep) {
    Module& d = g_state.items[dep];
    if (owner < 0) {
        if (!d.rooted) {
            d.rooted = true;
            ++d.refs;
        }
        return true;
    }
    Module& o = g_state.items[owner];
    for (u32 i = 0; i < o.dep_count; ++i) {
        if (o.deps[i] == dep) return true;
    }
    if (o.dep_count == MAX_DEPENDENCIES) return false;
    o.deps[o.dep_count++] = static_cast<u8>(dep);
    ++d.refs;
    return true;
}

template <class Call>
void release(i32 index, Call& call);

template <class Call>
void detach_module(i32 index, u32 reserved, Call& call) {
    Module& m = g_state.items[index];
    if (!m.attached) return;
    m.attached = false;
    u32 at = 0;
    for (u32 i = 0; i < g_state.order_count; ++i) {
        if (g_state.order[i] != index) g_state.order[at++] = g_state.order[i];
    }
    g_state.order_count = at;
    u64 module = reinterpret_cast<u64>(m.base);
    if (m.entry_rva) call(module + m.entry_rva, module, REASON_DETACH, reserved);
    for (u32 i = 0; i < m.tls.callback_count; ++i) call(m.tls.callbacks[i], module, REASON_DETACH, reserved);
}

template <class Call>
void unload(i32 index, Call& call) {
    detach_module(index, 0, call);
    Module& m = g_state.items[index];
    for (u32 i = 0; i < m.dep_count; ++i) release(m.deps[i], call);
    if (m.tls_block) {
        if (g_state.tls_array) g_state.tls_array[m.tls_index] = 0;
        g_state.tls_taken[m.tls_index] = false;
        g_runtime->unmap(m.tls_block, (m.tls_bytes + PAGE - 1) / PAGE * PAGE);
    }
    g_runtime->unmap(m.raw, m.raw_bytes);
    u8* raw = reinterpret_cast<u8*>(&m);
    for (usize i = 0; i < sizeof(m); ++i) raw[i] = 0;
}

template <class Call>
void release(i32 index, Call& call) {
    Module& m = g_state.items[index];
    if (m.refs) --m.refs;
    if (m.refs != 0 || m.binding) return;
    unload(index, call);
}

struct NoCall {
    u64 operator()(u64, u64, u64, u64) const { return 1; }
};

inline void abandon(i32 index) {
    NoCall none;
    Module& m = g_state.items[index];
    for (u32 i = 0; i < m.dep_count; ++i) release(m.deps[i], none);
    m.dep_count = 0;
}

inline void discard_slot(i32 slot) {
    abandon(slot);
    u8* clear = reinterpret_cast<u8*>(&g_state.items[slot]);
    for (usize i = 0; i < sizeof(Module); ++i) clear[i] = 0;
}

inline bool export_address(const ImportTable* builtins, i32 index, const char* name, u32 ordinal, u32 hops, u64* address) {
    const Module& m = g_state.items[index];
    pe::ExportRef ref;
    if (pe::find_export(m.base, m.bytes, m.exports, name, ordinal, &ref) != pe::ExportResult::Found) return false;
    if (!ref.forwarded) {
        *address = reinterpret_cast<u64>(m.base) + ref.rva;
        return true;
    }
    if (hops >= MAX_HOPS) return false;
    u32 dot = 0;
    bool seen = false;
    for (u32 i = 0; ref.forward[i]; ++i) {
        if (ref.forward[i] == '.') {
            dot = i;
            seen = true;
        }
    }
    if (!seen || dot == 0 || dot + 1 >= sizeof(ref.forward) || ref.forward[dot + 1] == 0 || dot + 5 > NAME_BYTES) return false;
    char target[NAME_BYTES];
    for (u32 i = 0; i < dot; ++i) target[i] = ref.forward[i];
    const char* ext = ".dll";
    for (u32 i = 0; i < 5; ++i) target[dot + i] = ext[i];
    const char* func = ref.forward + dot + 1;
    if (func[0] == '#') {
        u32 value = 0;
        for (u32 i = 1; func[i]; ++i) {
            if (func[i] < '0' || func[i] > '9' || value > 6553) return false;
            value = value * 10 + static_cast<u32>(func[i] - '0');
        }
        if (value == 0 || value > 0xFFFF || func[1] == 0) return false;
        return resolve_import(builtins, index, hops + 1, target, nullptr, static_cast<u16>(value), address);
    }
    return resolve_import(builtins, index, hops + 1, target, func, 0, address);
}

inline bool bind_callback(void* ctx, const char* dll, const char* name, u16 ordinal, u64* address) {
    const BindContext* c = static_cast<const BindContext*>(ctx);
    return resolve_import(c->builtins, c->owner, 0, dll, name, ordinal, address);
}

inline i32 load_image(const char* spelled, const char* key, const ImportTable* builtins, u32* error) {
    if (g_state.nesting >= MAX_NESTING) {
        *error = ERROR_BAD_IMAGE;
        return -1;
    }
    i32 slot = -1;
    for (u32 i = 0; i < MAX_MODULES && slot < 0; ++i) {
        if (!g_state.items[i].used) slot = static_cast<i32>(i);
    }
    if (slot < 0) {
        *error = ERROR_NO_MEMORY;
        return -1;
    }
    if (!g_runtime || !g_runtime->read_file || !g_runtime->unmap) {
        *error = ERROR_NOT_FOUND;
        return -1;
    }
    u64 size = 0;
    u8* file = g_runtime->read_file(spelled, &size);
    if (!file && !detail::same_name(spelled, key)) file = g_runtime->read_file(key, &size);
    if (!file) {
        *error = ERROR_NOT_FOUND;
        return -1;
    }
    pe::Image& img = g_scratch[g_state.nesting];
    ++g_state.nesting;
    u8* base = nullptr;
    u8* raw = nullptr;
    u64 raw_bytes = 0;
    u8* tls_block = nullptr;
    u64 tls_bytes = 0;
    u32 tls_index = 0;
    i32 result = -1;
    do {
        if (pe::parse(file, size, &img) != pe::Status::Ok || !img.is_dll || !img.relocatable) {
            *error = ERROR_BAD_IMAGE;
            break;
        }
        raw = g_runtime->reserve(static_cast<u64>(img.size_of_image) + BASE_ALIGN);
        if (!raw) {
            *error = ERROR_NO_MEMORY;
            break;
        }
        raw_bytes = (static_cast<u64>(img.size_of_image) + BASE_ALIGN + PAGE - 1) / PAGE * PAGE;
        base = reinterpret_cast<u8*>((reinterpret_cast<u64>(raw) + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1));
        if (pe::map(img, file, size, base, img.size_of_image) != pe::Status::Ok ||
            pe::relocate(img, base, reinterpret_cast<u64>(base)) != pe::Status::Ok) {
            *error = ERROR_BAD_IMAGE;
            break;
        }
        Module& m = g_state.items[slot];
        u8* wipe = reinterpret_cast<u8*>(&m);
        for (usize i = 0; i < sizeof(m); ++i) wipe[i] = 0;
        m.used = true;
        m.binding = true;
        for (u32 i = 0; i < NAME_BYTES - 1 && key[i]; ++i) m.key[i] = key[i];
        m.base = base;
        m.bytes = img.size_of_image;
        m.raw = raw;
        m.raw_bytes = raw_bytes;
        m.entry_rva = img.entry_rva;
        if (img.directory_count > pe::DIR_EXPORT) m.exports = img.directories[pe::DIR_EXPORT];

        BindContext ctx{ builtins, slot };
        pe::ImportReport report;
        if (pe::bind_imports(img, base, &bind_callback, &ctx, &report) != pe::Status::Ok) {
            *error = g_state.cause ? g_state.cause : ERROR_PROC_NOT_FOUND;
            discard_slot(slot);
            break;
        }
        pe::Tls tls;
        if (pe::tls_info(img, base, reinterpret_cast<u64>(base), &tls) != pe::Status::Ok) {
            *error = ERROR_BAD_IMAGE;
            discard_slot(slot);
            break;
        }
        bool tls_ok = true;
        if (tls.present) {
            tls_bytes = static_cast<u64>(tls.template_bytes) + tls.zero_fill;
            for (u32 i = 1; i < TLS_LIMIT && tls_index == 0; ++i) {
                if (!g_state.tls_taken[i]) tls_index = i;
            }
            if (tls_index == 0 || tls_bytes > MAX_TLS_BYTES) tls_ok = false;
            else {
                tls_block = g_runtime->reserve(tls_bytes ? tls_bytes : 1);
                if (!tls_block) tls_ok = false;
            }
        }
        if (!tls_ok) {
            *error = ERROR_NO_MEMORY;
            discard_slot(slot);
            tls_index = 0;
            break;
        }
        u32 run_count = pe::plan(img, g_runs, pe::MAX_RUNS);
        bool protected_ok = true;
        for (u32 i = 0; i < run_count && protected_ok; ++i) {
            protected_ok = g_runtime->protect(base + static_cast<u64>(g_runs[i].first_page) * PAGE, static_cast<u64>(g_runs[i].pages) * PAGE, g_runs[i].prot);
        }
        if (!protected_ok) {
            *error = ERROR_NO_MEMORY;
            discard_slot(slot);
            break;
        }
        if (tls.present) {
            const u8* from = base + tls.template_rva;
            for (u32 i = 0; i < tls.template_bytes; ++i) tls_block[i] = from[i];
            u8* slot_at = base + tls.index_rva;
            slot_at[0] = static_cast<u8>(tls_index);
            slot_at[1] = slot_at[2] = slot_at[3] = 0;
            g_state.tls_taken[tls_index] = true;
            if (g_state.tls_array) g_state.tls_array[tls_index] = reinterpret_cast<u64>(tls_block);
        }
        m.tls = tls;
        m.tls_index = tls_index;
        m.tls_block = tls_block;
        m.tls_bytes = tls_bytes;
        m.binding = false;
        result = slot;
    } while (false);
    --g_state.nesting;
    g_runtime->unmap(file, (size + PAGE - 1) / PAGE * PAGE);
    if (result < 0) {
        if (raw) g_runtime->unmap(raw, raw_bytes);
        if (tls_block) g_runtime->unmap(tls_block, ((tls_bytes ? tls_bytes : 1) + PAGE - 1) / PAGE * PAGE);
    }
    return result;
}

inline i32 acquire(const char* dll, const ImportTable* builtins, u32* error) {
    char key[NAME_BYTES];
    char spelled[NAME_BYTES];
    if (!normalise(dll, key, spelled)) {
        *error = ERROR_NOT_FOUND;
        return -1;
    }
    i32 found = find(key);
    if (found >= 0) return found;
    return load_image(spelled, key, builtins, error);
}

inline bool resolve_import(const ImportTable* builtins, i32 owner, u32 hops, const char* dll, const char* name, u16 ordinal, u64* address) {
    const char* home = pe::modules::canonical(dll);
    if (name != nullptr && detail::lookup_builtin(builtins, home, name, address)) return true;
    if (pe::modules::provided(home)) return false;
    u32 error = 0;
    i32 index = acquire(dll, builtins, &error);
    if (index < 0) {
        if (g_state.cause == 0) g_state.cause = error;
        return false;
    }
    if (!add_dependency(owner, index)) return false;
    bool ok = export_address(builtins, index, name, ordinal, hops, address);
    if (!ok && g_state.cause == 0) g_state.cause = ERROR_PROC_NOT_FOUND;
    return ok;
}

template <class Call>
bool attach_tree(i32 index, u32 reserved, Call& call) {
    Module& m = g_state.items[index];
    if (m.attached || m.attaching) return true;
    m.attaching = true;
    for (u32 i = 0; i < m.dep_count; ++i) {
        if (!attach_tree(m.deps[i], reserved, call)) {
            m.attaching = false;
            return false;
        }
    }
    u64 module = reinterpret_cast<u64>(m.base);
    for (u32 i = 0; i < m.tls.callback_count; ++i) call(m.tls.callbacks[i], module, REASON_ATTACH, reserved);
    if (m.entry_rva && static_cast<u32>(call(module + m.entry_rva, module, REASON_ATTACH, reserved)) == 0) {
        m.attaching = false;
        return false;
    }
    m.attaching = false;
    m.attached = true;
    g_state.order[g_state.order_count++] = static_cast<u8>(index);
    return true;
}

template <class Call>
bool attach_static(Call& call) {
    for (u32 i = 0; i < MAX_MODULES; ++i) {
        if (g_state.items[i].used && g_state.items[i].rooted && !attach_tree(static_cast<i32>(i), 1, call)) return false;
    }
    return true;
}

template <class Call>
void process_detach(Call& call) {
    if (g_state.exiting) return;
    g_state.exiting = true;
    while (g_state.order_count) detach_module(g_state.order[g_state.order_count - 1], 1, call);
}

inline void install_tls(u64* tls_array) {
    g_state.tls_array = tls_array;
    for (u32 i = 0; i < MAX_MODULES; ++i) {
        const Module& m = g_state.items[i];
        if (m.used && m.tls_block) tls_array[m.tls_index] = reinterpret_cast<u64>(m.tls_block);
    }
}

template <class Call>
u64 load_library(const char* path, Call& call, u32* error) {
    char key[NAME_BYTES];
    char spelled[NAME_BYTES];
    if (!normalise(path, key, spelled)) {
        *error = ERROR_NOT_FOUND;
        return 0;
    }
    g_state.cause = 0;
    i32 index = find(key);
    if (index < 0) {
        index = load_image(spelled, key, g_state.builtins, error);
        if (index < 0) {
            if (g_state.cause) *error = g_state.cause;
            return 0;
        }
    }
    ++g_state.items[index].refs;
    if (!attach_tree(index, 0, call)) {
        release(index, call);
        *error = ERROR_INIT_FAILED;
        return 0;
    }
    return reinterpret_cast<u64>(g_state.items[index].base);
}

}
