#include <aphelion.hpp>
#include <begin.hpp>
#include <cosmos/fileabi.hpp>
#include <cosmos/handleabi.hpp>
#include "call.hpp"
#include "host.hpp"
#include "ucrt.hpp"
#include "win32x.hpp"

namespace {

constexpr u64 IMAGE_AT = 0x100100000000ull;
constexpr u64 MAX_FILE = 16ull << 20;
constexpr u64 RW = 3;
constexpr u64 RIGHT_READ = 1;
constexpr u64 PAGE = pehost::PAGE;

struct Platform {
    u8* reserve(u64 bytes, bool image) {
        u64 pages = (bytes + PAGE - 1) / PAGE;
        u64 vmo = 0;
        if (sys::vmo_create(pages, &vmo) != 0) return nullptr;
        i64 at = sys::vm_map(vmo, image ? IMAGE_AT : 0, 0, pages, RW);
        sys::handle_close(vmo);
        return at > 0 ? reinterpret_cast<u8*>(at) : nullptr;
    }
    bool release(u8* at, u64 bytes) { return sys::vm_unmap(reinterpret_cast<u64>(at), (bytes + PAGE - 1) / PAGE) == 0; }
    bool protect(u8* at, u64 bytes, u32 prot) { return sys::vm_protect(reinterpret_cast<u64>(at), bytes / PAGE, prot) == 0; }
    bool set_teb(pehost::Teb* teb) { return sys::set_gs_base(reinterpret_cast<u64>(teb)) == 0; }
    u64 thread_id() { return static_cast<u64>(sys::gettid()); }
    void log(const char* text) {
        sys::debug_write(text, length_of(text));
        sys::debug_write("\n", 1);
    }
    u64 call(void* function, u64 a, u64 b, u64 c, void* top) { return pehost_call3(function, a, b, c, top); }
};

void exit_process(u32 code) { sys::exit(static_cast<i32>(code)); }

u8* reserve_memory(u64 bytes) {
    u64 pages = (bytes + PAGE - 1) / PAGE;
    u64 vmo = 0;
    if (sys::vmo_create(pages, &vmo) != 0) return nullptr;
    i64 at = sys::vm_map(vmo, 0, 0, pages, RW);
    sys::handle_close(vmo);
    return at > 0 ? reinterpret_cast<u8*>(at) : nullptr;
}

bool protect_memory(u8* at, u64 bytes, u32 prot) { return sys::vm_protect(reinterpret_cast<u64>(at), bytes / PAGE, prot) == 0; }

void sleep_ms(u32 milliseconds) {
    static u64 pair[2];
    if (milliseconds == 0) {
        sys::yield();
        return;
    }
    if (pair[0] == 0 && sys::pipe_create(16, pair) != 0) {
        pair[0] = 0;
        return;
    }
    struct {
        u64 handle;
        u32 want;
        u32 got;
    } entry = { pair[0], 1, 0 };
    sys::handle_wait(&entry, 1, milliseconds == 0xFFFFFFFFu ? ~0ull : milliseconds);
}

i64 write_console(u32, const void* data, u32 length) { return sys::debug_write(static_cast<const char*>(data), length); }

u64 g_directory = 0;

u8* read_file(const char* name, u64* size) {
    if (g_directory == 0) return nullptr;
    u64 length = length_of(name);
    if (length == 0 || length > fileabi::MAX_PATH) return nullptr;
    u64 handle = 0;
    if (sys::file_open(g_directory, name, length, RIGHT_READ, &handle) != 0) return nullptr;
    fileabi::Stat st{};
    u8* buffer = nullptr;
    if (sys::file_stat(handle, &st) == 0 && st.kind == fileabi::KIND_FILE && st.size != 0 && st.size <= MAX_FILE) {
        buffer = reserve_memory(st.size);
        if (buffer) {
            u64 have = 0;
            while (have < st.size) {
                i64 n = sys::file_read(handle, have, buffer + have, st.size - have);
                if (n <= 0) break;
                have += static_cast<u64>(n);
            }
            if (have == st.size) {
                *size = st.size;
            } else {
                sys::vm_unmap(reinterpret_cast<u64>(buffer), (st.size + PAGE - 1) / PAGE);
                buffer = nullptr;
            }
        }
    }
    sys::handle_close(handle);
    return buffer;
}

void unmap_memory(u8* at, u64 bytes) { sys::vm_unmap(reinterpret_cast<u64>(at), (bytes + PAGE - 1) / PAGE); }

u64 read_tsc() {
    u32 low, high;
    asm volatile("lfence\n\trdtsc" : "=a"(low), "=d"(high) : : "memory");
    return (static_cast<u64>(high) << 32) | low;
}

bool read_clock(pehost::Clock* out) {
    userabi::ClockRecord record{};
    if (sys::clock(&record) != 0) return false;
    if (record.flags & userabi::CLOCK_TSC_USABLE) {
        out->counter = read_tsc();
        out->frequency = record.tsc_khz * 1000;
    } else {
        out->counter = record.uptime_ms;
        out->frequency = 1000;
    }
    out->wall_valid = (record.flags & userabi::CLOCK_WALL_VALID) != 0;
    out->wall_ms = out->wall_valid ? record.wall_ms : 0;
    return true;
}

constexpr u32 MAX_FILES = 64;
u64 g_files[MAX_FILES];

i64 file_open_hook(const char* name, u64* size) {
    if (g_directory == 0) return -1;
    u64 length = length_of(name);
    if (!fileabi::path_len_ok(length)) return -1;
    u32 slot = 0;
    while (slot < MAX_FILES && g_files[slot] != 0) ++slot;
    if (slot == MAX_FILES) return -1;
    u64 handle = 0;
    if (sys::file_open(g_directory, name, length, RIGHT_READ, &handle) != 0) return -1;
    fileabi::Stat st{};
    if (sys::file_stat(handle, &st) != 0 || st.kind != fileabi::KIND_FILE) {
        sys::handle_close(handle);
        return -1;
    }
    g_files[slot] = handle;
    *size = st.size;
    return slot;
}

i64 file_read_hook(i64 slot, u64 offset, void* buffer, u64 length) {
    if (slot < 0 || slot >= MAX_FILES || g_files[slot] == 0) return -1;
    return sys::file_read(g_files[slot], offset, buffer, length);
}

void file_close_hook(i64 slot) {
    if (slot < 0 || slot >= MAX_FILES || g_files[slot] == 0) return;
    sys::handle_close(g_files[slot]);
    g_files[slot] = 0;
}

i32 dir_entry_hook(u64 index, char* name, u32 capacity, u64* size, u64* mtime_ms, bool* directory) {
    if (g_directory == 0 || capacity == 0) return 0;
    fileabi::DirEntry entry{};
    if (sys::dir_entry(g_directory, index, &entry) != 0) return 0;
    u32 n = 0;
    while (entry.name[n] && n + 1 < capacity) {
        name[n] = entry.name[n];
        ++n;
    }
    name[n] = 0;
    *size = entry.size;
    *mtime_ms = entry.mtime_ms;
    *directory = entry.kind == fileabi::KIND_DIRECTORY;
    return 1;
}

u64 g_work = 0;

struct WorkFile {
    u64 handle;
    bool owned;
};

WorkFile g_work_files[MAX_FILES];

u64 work_rights(u32 want) {
    u64 rights = 0;
    if (want & pehost::FS_WANT_READ) rights |= 1;
    if (want & pehost::FS_WANT_WRITE) rights |= 2 | 4;
    if (want & pehost::FS_WANT_LIST) rights |= 8;
    return rights;
}

i64 work_slot(u64 handle, bool owned) {
    for (u32 slot = 0; slot < MAX_FILES; ++slot) {
        if (g_work_files[slot].handle == 0) {
            g_work_files[slot] = WorkFile{ handle, owned };
            return slot;
        }
    }
    return -1;
}

bool work_free_slot() {
    for (u32 slot = 0; slot < MAX_FILES; ++slot) {
        if (g_work_files[slot].handle == 0) return true;
    }
    return false;
}

u64 work_handle(i64 slot) {
    if (slot < 0 || slot >= MAX_FILES) return 0;
    return g_work_files[slot].handle;
}

bool work_info(u64 handle, pehost::FsInfo* info) {
    fileabi::Stat st{};
    if (sys::file_stat(handle, &st) != 0) return false;
    info->size = st.size;
    info->mtime_ms = st.mtime_ms;
    info->kind = st.kind == fileabi::KIND_DIRECTORY ? pehost::FS_DIRECTORY : pehost::FS_FILE;
    info->reserved = 0;
    return true;
}

i64 work_adopt(u64 handle, bool owned, pehost::FsInfo* info) {
    if (!work_info(handle, info)) {
        if (owned) sys::handle_close(handle);
        return pehost::FS_FAILED;
    }
    i64 slot = work_slot(handle, owned);
    if (slot < 0) {
        if (owned) sys::handle_close(handle);
        return pehost::FS_NO_SPACE;
    }
    return slot;
}

i64 fs_open_hook(const char* path, u32 want, pehost::FsInfo* info) {
    if (g_work == 0) return pehost::FS_DENIED;
    if (!work_free_slot()) return pehost::FS_NO_SPACE;
    u64 length = length_of(path);
    if (length == 0) return work_adopt(g_work, false, info);
    if (!fileabi::path_len_ok(length)) return pehost::FS_NOT_FOUND;
    u64 handle = 0;
    i64 e = sys::file_open(g_work, path, length, work_rights(want), &handle);
    if (e != 0) return e < 0 ? e : pehost::FS_FAILED;
    return work_adopt(handle, true, info);
}

i64 fs_create_hook(const char* path, u32 directory, u32 want, pehost::FsInfo* info) {
    if (g_work == 0) return pehost::FS_DENIED;
    u64 length = length_of(path);
    if (!fileabi::path_len_ok(length)) return pehost::FS_NOT_FOUND;
    u64 handle = 0;
    if (directory) {
        i64 e = sys::file_create(g_work, path, length, 0, fileabi::CREATE_DIRECTORY, &handle);
        return e == 0 ? 0 : (e < 0 ? e : pehost::FS_FAILED);
    }
    if (!work_free_slot()) return pehost::FS_NO_SPACE;
    i64 e = sys::file_create(g_work, path, length, work_rights(want), 0, &handle);
    if (e != 0) return e < 0 ? e : pehost::FS_FAILED;
    return work_adopt(handle, true, info);
}

i64 fs_read_hook(i64 slot, u64 offset, void* buffer, u64 length) {
    u64 handle = work_handle(slot);
    return handle ? sys::file_read(handle, offset, buffer, length) : pehost::FS_FAILED;
}

i64 fs_write_hook(i64 slot, u64 offset, const void* buffer, u64 length) {
    u64 handle = work_handle(slot);
    return handle ? sys::file_write(handle, offset, buffer, length) : pehost::FS_FAILED;
}

i64 fs_resize_hook(i64 slot, u64 size) {
    u64 handle = work_handle(slot);
    return handle ? sys::file_resize(handle, size) : pehost::FS_FAILED;
}

void fs_close_hook(i64 slot) {
    if (slot < 0 || slot >= MAX_FILES || g_work_files[slot].handle == 0) return;
    if (g_work_files[slot].owned) sys::handle_close(g_work_files[slot].handle);
    g_work_files[slot] = WorkFile{};
}

i64 fs_stat_hook(i64 slot, pehost::FsInfo* info) {
    u64 handle = work_handle(slot);
    return handle && work_info(handle, info) ? 0 : pehost::FS_FAILED;
}

i64 fs_remove_hook(const char* path, u32 directory) {
    if (g_work == 0) return pehost::FS_DENIED;
    u64 length = length_of(path);
    if (!fileabi::path_len_ok(length)) return pehost::FS_NOT_FOUND;
    return sys::file_unlink(g_work, path, length, directory ? fileabi::UNLINK_DIRECTORY : 0);
}

i64 fs_rename_hook(const char* from, const char* to, u32 replace) {
    if (g_work == 0) return pehost::FS_DENIED;
    u64 from_length = length_of(from);
    u64 to_length = length_of(to);
    if (!fileabi::path_len_ok(from_length) || !fileabi::path_len_ok(to_length)) return pehost::FS_NOT_FOUND;
    return sys::file_rename(g_work, from, from_length, to, to_length, replace ? fileabi::RENAME_REPLACE : 0);
}

i32 fs_entry_hook(i64 slot, u64 index, char* name, u32 capacity, pehost::FsInfo* info) {
    u64 handle = work_handle(slot);
    if (handle == 0 || capacity == 0) return -1;
    fileabi::DirEntry entry{};
    i64 r = sys::dir_entry(handle, index, &entry);
    if (r != 0) return r > 0 ? 1 : -1;
    u32 n = 0;
    while (entry.name[n] && n + 1 < capacity) {
        name[n] = entry.name[n];
        ++n;
    }
    name[n] = 0;
    info->size = entry.size;
    info->mtime_ms = entry.mtime_ms;
    info->kind = entry.kind == fileabi::KIND_DIRECTORY ? pehost::FS_DIRECTORY : pehost::FS_FILE;
    info->reserved = 0;
    return 0;
}

u64 g_stdin = 0;

i64 read_stdin_hook(void* buffer, u32 length) {
    if (g_stdin == 0) return -1;
    u64 want = length < fileabi::MAX_IO ? length : fileabi::MAX_IO;
    return sys::file_read(g_stdin, 0, buffer, want);
}

i32 futex_wait_hook(volatile u32* word, u32 expected, u64 timeout_ms) {
    i64 r = sys::futex_wait(word, expected, timeout_ms);
    return r == handleabi::ERR_TIMEDOUT ? 1 : 0;
}

void futex_wake_hook(volatile u32* word) { sys::futex_wake(word, 0x7FFFFFFF); }

bool spawn_hook(void (*entry)(u64), u64 argument, u8* stack_top) { return sys::thread_create(entry, reinterpret_cast<u64>(stack_top), argument) > 0; }

bool set_teb_hook(void* teb) { return sys::set_gs_base(reinterpret_cast<u64>(teb)) == 0; }

pehost::Runtime g_runtime = { &exit_process, &write_console, &reserve_memory, &protect_memory, &sleep_ms, &read_file, &unmap_memory,
                              &read_clock, &file_open_hook, &file_read_hook, &file_close_hook, &dir_entry_hook, &read_stdin_hook,
                              &futex_wait_hook, &futex_wake_hook, &spawn_hook, &set_teb_hook,
                              &fs_open_hook, &fs_create_hook, &fs_read_hook, &fs_write_hook, &fs_resize_hook, &fs_close_hook, &fs_stat_hook,
                              &fs_remove_hook, &fs_rename_hook, &fs_entry_hook };
pehost::Prepared g_prepared;

constexpr u32 COMMAND_LINE = 1024;
char g_command_line[COMMAND_LINE];
begin::Start g_start;

void seed_environment(const startup::Parsed& parsed) {
    for (u32 i = 0; i < parsed.envc; ++i) {
        char name[64];
        const char* entry = parsed.env[i];
        u32 n = 0;
        while (entry[n] && entry[n] != '=' && n + 1 < sizeof(name)) {
            name[n] = entry[n];
            ++n;
        }
        if (entry[n] != '=') continue;
        name[n] = 0;
        pehost::win32::SetEnvironmentVariableA(name, entry + n + 1);
    }
}

}

extern "C" i64 aphelion_main(u64 channel, u64) {
    Platform platform;
    if (channel == 0 || !begin::receive(channel, &g_start)) {
        log("[pehost] no start-up message\n");
        return pehost::STATUS_INVALID_IMAGE_FORMAT;
    }
    u64 file = g_start.handle[startup::PROGRAM];
    g_directory = g_start.handle[startup::DIRECTORY];
    g_stdin = g_start.handle[startup::STDIN];
    g_work = g_start.handle[startup::WORK];
    if (g_work == 0) g_runtime.fs_open = nullptr;
    if (file == 0) {
        log("[pehost] the start-up message names no program\n");
        return pehost::STATUS_INVALID_IMAGE_FORMAT;
    }
    if (!pehost::cmdline::join(g_start.parsed.argv, g_start.parsed.argc, g_command_line, sizeof(g_command_line))) {
        log("[pehost] the arguments do not fit in a command line\n");
        return pehost::STATUS_INVALID_IMAGE_FORMAT;
    }
    fileabi::Stat st{};
    if (sys::file_stat(file, &st) != 0 || st.kind != fileabi::KIND_FILE) {
        log("[pehost] the program is not a file handle\n");
        return pehost::STATUS_INVALID_IMAGE_FORMAT;
    }
    if (st.size == 0 || st.size > MAX_FILE) {
        log("[pehost] the file is empty or larger than 16 MiB\n");
        return pehost::STATUS_INVALID_IMAGE_FORMAT;
    }
    u8* buffer = platform.reserve(st.size, false);
    if (!buffer) {
        log("[pehost] no memory for the file\n");
        return pehost::STATUS_NO_MEMORY;
    }
    u64 have = 0;
    while (have < st.size) {
        i64 n = sys::file_read(file, have, buffer + have, st.size - have);
        if (n <= 0) break;
        have += static_cast<u64>(n);
    }
    if (have != st.size) {
        log("[pehost] the file could not be read completely\n");
        return pehost::STATUS_INVALID_IMAGE_FORMAT;
    }

    pehost::g_runtime = &g_runtime;
    const pehost::ImportTable* imports = pehost::all_imports();
    i64 status = pehost::load(platform, buffer, st.size, *imports, &g_prepared);
    platform.release(buffer, st.size);
    sys::handle_close(file);
    if (status != 0) return status;
    pehost::attach(g_prepared, imports, pehost::Config{ g_command_line, g_start.parsed.argc ? g_start.parsed.argv[0] : "pe.exe" });
    seed_environment(g_start.parsed);
    return pehost::start(platform, g_prepared);
}
