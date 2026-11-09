#include <aphelion.hpp>
#include <cosmos/fileabi.hpp>
#include "call.hpp"
#include "host.hpp"
#include "ucrt.hpp"

namespace {

constexpr u64 IMAGE_AT = 0x100100000000ull;
constexpr u64 MAX_FILE = 16ull << 20;
constexpr u64 RW = 3;
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

pehost::Runtime g_runtime = { &exit_process, &write_console, &reserve_memory, &protect_memory, &sleep_ms };
pehost::Prepared g_prepared;

}

extern "C" i64 aphelion_main(u64 file) {
    Platform platform;
    fileabi::Stat st{};
    if (sys::file_stat(file, &st) != 0 || st.kind != fileabi::KIND_FILE) {
        log("[pehost] the argument is not a file handle\n");
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
    pehost::attach(g_prepared, imports, pehost::Config{ "pe.exe", "pe.exe" });
    return pehost::start(platform, g_prepared);
}
