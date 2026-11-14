#pragma once
#include "win32.hpp"

namespace pehost {

namespace fsys {

constexpr u32 TEXT_MAX = 255;
constexpr u32 COMPONENT_MAX = 255;

constexpr u32 E_FILE_NOT_FOUND = 2;
constexpr u32 E_PATH_NOT_FOUND = 3;
constexpr u32 E_ACCESS_DENIED = 5;
constexpr u32 E_NOT_ENOUGH_MEMORY = 8;
constexpr u32 E_SHARING_VIOLATION = 32;
constexpr u32 E_NOT_SUPPORTED = 50;
constexpr u32 E_FILE_EXISTS = 80;
constexpr u32 E_INVALID_PARAMETER = 87;
constexpr u32 E_DISK_FULL = 112;
constexpr u32 E_INVALID_NAME = 123;
constexpr u32 E_DIR_NOT_EMPTY = 145;
constexpr u32 E_ALREADY_EXISTS = 183;
constexpr u32 E_DIRECTORY = 267;

constexpr u32 ACCESS_READ_DATA = 0x1;
constexpr u32 ACCESS_WRITE_DATA = 0x2;
constexpr u32 ACCESS_APPEND_DATA = 0x4;
constexpr u32 ACCESS_GENERIC_ALL = 0x10000000u;
constexpr u32 ACCESS_GENERIC_WRITE = 0x40000000u;
constexpr u32 ACCESS_GENERIC_READ = 0x80000000u;

constexpr u32 CREATE_NEW = 1;
constexpr u32 CREATE_ALWAYS = 2;
constexpr u32 OPEN_EXISTING = 3;
constexpr u32 OPEN_ALWAYS = 4;
constexpr u32 TRUNCATE_EXISTING = 5;

constexpr u32 FLAG_BACKUP_SEMANTICS = 0x02000000u;
constexpr u32 FLAG_DELETE_ON_CLOSE = 0x04000000u;

constexpr u32 ATTRIBUTE_DIRECTORY = 0x10;
constexpr u32 ATTRIBUTE_NORMAL = 0x80;

struct Resolved {
    bool program;
    u32 length;
    char rel[TEXT_MAX + 1];
};

inline bool enabled() { return g_runtime != nullptr && g_runtime->fs_open != nullptr; }

inline u32 length_of(const char* s) {
    u32 n = 0;
    while (s[n]) ++n;
    return n;
}

inline char fold_letter(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

inline bool is_separator(char c) { return c == '\\' || c == '/'; }

inline bool component_ok(const char* p, u32 n, bool wildcards) {
    if (n == 0 || n > COMPONENT_MAX) return false;
    bool dots = true;
    for (u32 i = 0; i < n; ++i) {
        char c = p[i];
        if (static_cast<u8>(c) < 0x20 || c == ':' || c == '"' || c == '<' || c == '>' || c == '|') return false;
        if (!wildcards && (c == '*' || c == '?')) return false;
        if (c != '.') dots = false;
    }
    return !dots;
}

struct Stack {
    const char* start[64];
    u32 length[64];
    u32 count;
};

inline bool push_components(Stack& stack, const char* text, u32 text_length, bool wildcards) {
    u32 i = 0;
    while (i < text_length) {
        while (i < text_length && is_separator(text[i])) ++i;
        u32 begin = i;
        while (i < text_length && !is_separator(text[i])) ++i;
        u32 n = i - begin;
        if (n == 0) continue;
        if (n == 1 && text[begin] == '.') continue;
        if (n == 2 && text[begin] == '.' && text[begin + 1] == '.') {
            if (stack.count) --stack.count;
            continue;
        }
        if (!component_ok(text + begin, n, wildcards)) return false;
        if (stack.count >= 64) return false;
        stack.start[stack.count] = text + begin;
        stack.length[stack.count] = n;
        ++stack.count;
    }
    return true;
}

inline u32 resolve(const char* name, Resolved* out, bool wildcards = false) {
    out->program = false;
    out->length = 0;
    out->rel[0] = 0;
    if (name == nullptr || name[0] == 0) return E_PATH_NOT_FOUND;
    u32 n = length_of(name);
    if (n > TEXT_MAX) return E_INVALID_NAME;
    if (is_separator(name[0]) && n > 1 && is_separator(name[1])) return E_PATH_NOT_FOUND;
    const char* rest = name;
    u32 rest_length = n;
    bool absolute = false;
    if (n >= 2 && name[1] == ':') {
        char drive = fold_letter(name[0]);
        if (drive == 'A') out->program = true;
        else if (drive != 'C') return E_PATH_NOT_FOUND;
        rest = name + 2;
        rest_length = n - 2;
        absolute = rest_length > 0 && is_separator(rest[0]);
    } else if (is_separator(name[0])) {
        absolute = true;
    }
    Stack stack{};
    if (!absolute && !out->program) {
        const char* cwd = g_world.cwd;
        if (!push_components(stack, cwd, length_of(cwd), false)) return E_PATH_NOT_FOUND;
    }
    if (!push_components(stack, rest, rest_length, wildcards)) return E_INVALID_NAME;
    if (out->program && stack.count > 1) return E_PATH_NOT_FOUND;
    u32 at = 0;
    for (u32 i = 0; i < stack.count; ++i) {
        if (i) {
            if (at + 1 > TEXT_MAX) return E_INVALID_NAME;
            out->rel[at++] = '/';
        }
        if (at + stack.length[i] > TEXT_MAX) return E_INVALID_NAME;
        for (u32 k = 0; k < stack.length[i]; ++k) out->rel[at++] = stack.start[i][k];
    }
    out->rel[at] = 0;
    out->length = at;
    return 0;
}

inline u32 parent_length(const Resolved& r) {
    u32 at = r.length;
    while (at > 0 && r.rel[at - 1] != '/') --at;
    return at == 0 ? 0 : at - 1;
}

inline bool has_parent(const Resolved& r) { return parent_length(r) != 0; }

inline u32 translate(i64 error, u32 fallback) {
    switch (error) {
        case FS_NOT_FOUND: return E_FILE_NOT_FOUND;
        case FS_EXISTS: return E_ALREADY_EXISTS;
        case FS_WRONG_TYPE: return E_ACCESS_DENIED;
        case FS_NOT_EMPTY: return E_DIR_NOT_EMPTY;
        case FS_BUSY: return E_SHARING_VIOLATION;
        case FS_NO_SPACE: return E_DISK_FULL;
        case FS_DENIED: return E_ACCESS_DENIED;
        default: return fallback;
    }
}

inline u32 missing_error(const Resolved& r) {
    u32 up = parent_length(r);
    if (up == 0) return E_FILE_NOT_FOUND;
    char parent[TEXT_MAX + 1];
    for (u32 i = 0; i < up; ++i) parent[i] = r.rel[i];
    parent[up] = 0;
    FsInfo info{};
    i64 h = g_runtime->fs_open(parent, FS_WANT_LIST, &info);
    if (h < 0) return E_PATH_NOT_FOUND;
    g_runtime->fs_close(h);
    return info.kind == FS_DIRECTORY ? E_FILE_NOT_FOUND : E_PATH_NOT_FOUND;
}

inline u32 wrong_type_error(const Resolved& r, u32 target_error) {
    u32 up = parent_length(r);
    if (up == 0) return target_error;
    char parent[TEXT_MAX + 1];
    for (u32 i = 0; i < up; ++i) parent[i] = r.rel[i];
    parent[up] = 0;
    FsInfo info{};
    i64 h = g_runtime->fs_open(parent, FS_WANT_LIST, &info);
    if (h < 0) return E_PATH_NOT_FOUND;
    g_runtime->fs_close(h);
    return info.kind == FS_DIRECTORY ? target_error : E_PATH_NOT_FOUND;
}

inline bool wants_write(u32 access) {
    return (access & (ACCESS_GENERIC_WRITE | ACCESS_GENERIC_ALL | ACCESS_WRITE_DATA | ACCESS_APPEND_DATA)) != 0;
}

struct Opened {
    i64 handle;
    FsInfo info;
    bool existed;
};

inline u32 open_path(const Resolved& r, u32 access, u32 disposition, u32 flags, Opened* out) {
    out->handle = -1;
    out->existed = false;
    if (disposition < CREATE_NEW || disposition > TRUNCATE_EXISTING) return E_INVALID_PARAMETER;
    bool write = wants_write(access);
    u32 want = write ? (FS_WANT_READ | FS_WANT_WRITE) : FS_WANT_READ;
    if (!write && (flags & FLAG_BACKUP_SEMANTICS)) want |= FS_WANT_LIST;
    if ((disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING) && !write) return E_INVALID_PARAMETER;
    FsInfo info{};
    i64 h = g_runtime->fs_open(r.rel, want, &info);
    if (h >= 0) {
        out->existed = true;
        if (disposition == CREATE_NEW) {
            g_runtime->fs_close(h);
            return E_FILE_EXISTS;
        }
        if (info.kind == FS_DIRECTORY) {
            if (write || !(flags & FLAG_BACKUP_SEMANTICS) || disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING) {
                g_runtime->fs_close(h);
                return E_ACCESS_DENIED;
            }
        } else if (disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING) {
            i64 e = g_runtime->fs_resize ? g_runtime->fs_resize(h, 0) : FS_DENIED;
            if (e != 0) {
                g_runtime->fs_close(h);
                return translate(e, E_ACCESS_DENIED);
            }
            info.size = 0;
        }
        out->handle = h;
        out->info = info;
        return 0;
    }
    if (h == FS_WRONG_TYPE) return wrong_type_error(r, E_ACCESS_DENIED);
    if (h != FS_NOT_FOUND) return translate(h, E_ACCESS_DENIED);
    if (disposition == OPEN_EXISTING || disposition == TRUNCATE_EXISTING) return missing_error(r);
    if (r.length == 0) return E_ACCESS_DENIED;
    if (!g_runtime->fs_create) return E_ACCESS_DENIED;
    i64 c = g_runtime->fs_create(r.rel, 0, want | FS_WANT_WRITE, &info);
    if (c < 0) {
        if (c == FS_NOT_FOUND) return missing_error(r);
        if (c == FS_EXISTS) return E_FILE_EXISTS;
        if (c == FS_WRONG_TYPE) return wrong_type_error(r, E_ACCESS_DENIED);
        return translate(c, E_ACCESS_DENIED);
    }
    out->handle = c;
    out->info = info;
    return 0;
}

inline bool stat_path(const Resolved& r, FsInfo* info) {
    i64 h = g_runtime->fs_open(r.rel, FS_WANT_READ | FS_WANT_LIST, info);
    if (h < 0) return false;
    g_runtime->fs_close(h);
    return true;
}

inline u32 delete_file(const Resolved& r) {
    if (r.program || r.length == 0 || !g_runtime->fs_remove) return E_ACCESS_DENIED;
    i64 e = g_runtime->fs_remove(r.rel, 0);
    if (e == 0) return 0;
    if (e == FS_NOT_FOUND) return missing_error(r);
    if (e == FS_WRONG_TYPE) return wrong_type_error(r, E_ACCESS_DENIED);
    return translate(e, E_ACCESS_DENIED);
}

inline u32 remove_directory(const Resolved& r) {
    if (r.program || r.length == 0 || !g_runtime->fs_remove) return E_ACCESS_DENIED;
    i64 e = g_runtime->fs_remove(r.rel, 1);
    if (e == 0) return 0;
    if (e == FS_NOT_FOUND) return missing_error(r);
    if (e == FS_WRONG_TYPE) return wrong_type_error(r, E_DIRECTORY);
    return translate(e, E_ACCESS_DENIED);
}

inline u32 make_directory(const Resolved& r) {
    if (r.program || !g_runtime->fs_create) return E_ACCESS_DENIED;
    if (r.length == 0) return E_ACCESS_DENIED;
    i64 e = g_runtime->fs_create(r.rel, 1, 0, nullptr);
    if (e >= 0) return 0;
    if (e == FS_NOT_FOUND) return E_PATH_NOT_FOUND;
    if (e == FS_WRONG_TYPE) return wrong_type_error(r, E_ACCESS_DENIED);
    return translate(e, E_ACCESS_DENIED);
}

inline u32 move_path(const Resolved& from, const Resolved& to, bool replace) {
    if (from.program || to.program || from.length == 0 || to.length == 0 || !g_runtime->fs_rename) return E_ACCESS_DENIED;
    i64 e = g_runtime->fs_rename(from.rel, to.rel, replace ? 1 : 0);
    if (e == 0) return 0;
    if (e == FS_NOT_FOUND) {
        FsInfo info{};
        if (!stat_path(from, &info)) return missing_error(from);
        return missing_error(to);
    }
    return translate(e, E_ACCESS_DENIED);
}

inline i64 read_at(i64 handle, u64 offset, void* buffer, u64 length) {
    u8* out = static_cast<u8*>(buffer);
    u64 got = 0;
    while (got < length) {
        i64 n = g_runtime->fs_read(handle, offset + got, out + got, length - got);
        if (n < 0) return got ? static_cast<i64>(got) : n;
        if (n == 0) break;
        got += static_cast<u64>(n);
    }
    return static_cast<i64>(got);
}

inline i64 write_at(i64 handle, u64 offset, const void* buffer, u64 length) {
    const u8* in = static_cast<const u8*>(buffer);
    u64 done = 0;
    while (done < length) {
        i64 n = g_runtime->fs_write(handle, offset + done, in + done, length - done);
        if (n < 0) return done ? static_cast<i64>(done) : n;
        if (n == 0) break;
        done += static_cast<u64>(n);
    }
    return static_cast<i64>(done);
}

inline u32 copy_file(const Resolved& from, const Resolved& to, bool fail_if_exists) {
    if (to.program || to.length == 0) return E_ACCESS_DENIED;
    Opened source;
    u32 e = open_path(from, ACCESS_GENERIC_READ, OPEN_EXISTING, 0, &source);
    if (e) return e;
    if (source.info.kind == FS_DIRECTORY) {
        g_runtime->fs_close(source.handle);
        return E_ACCESS_DENIED;
    }
    Opened target;
    e = open_path(to, ACCESS_GENERIC_WRITE | ACCESS_GENERIC_READ, fail_if_exists ? CREATE_NEW : CREATE_ALWAYS, 0, &target);
    if (e) {
        g_runtime->fs_close(source.handle);
        return e;
    }
    u8 chunk[1024];
    u64 offset = 0;
    u32 result = 0;
    for (;;) {
        i64 got = read_at(source.handle, offset, chunk, sizeof(chunk));
        if (got < 0) {
            result = translate(got, E_ACCESS_DENIED);
            break;
        }
        if (got == 0) break;
        i64 put = write_at(target.handle, offset, chunk, static_cast<u64>(got));
        if (put != got) {
            result = put < 0 ? translate(put, E_DISK_FULL) : E_DISK_FULL;
            break;
        }
        offset += static_cast<u64>(got);
    }
    g_runtime->fs_close(source.handle);
    g_runtime->fs_close(target.handle);
    return result;
}

inline u32 change_directory(const Resolved& r) {
    if (r.program) return E_ACCESS_DENIED;
    FsInfo info{};
    if (!stat_path(r, &info)) return missing_error(r);
    if (info.kind != FS_DIRECTORY) return E_DIRECTORY;
    for (u32 i = 0; i <= r.length; ++i) g_world.cwd[i] = r.rel[i];
    return 0;
}

inline u32 format_full(const Resolved& r, char* out, u32 capacity) {
    u32 n = 0;
    if (r.program) {
        if (capacity < 4) return 0;
        out[n++] = 'A';
        out[n++] = ':';
    }
    if (n + 2 > capacity) return 0;
    out[n++] = '\\';
    for (u32 i = 0; i < r.length; ++i) {
        if (n + 2 > capacity) return 0;
        out[n++] = r.rel[i] == '/' ? '\\' : r.rel[i];
    }
    out[n] = 0;
    return n;
}

}

}
