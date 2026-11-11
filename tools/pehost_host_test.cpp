#include <cosmos/pe.hpp>
#include "../userland/pehost/call.hpp"
#include "../userland/pehost/host.hpp"
#include "../userland/pehost/ucrt.hpp"
#include "pe_samples.hpp"
#include "pe_samples_mingw.hpp"
#include "pe_samples_run.hpp"

extern "C" {
int printf(const char*, ...);
void* malloc(usize);
void free(void*);
}

namespace {

int g_failures = 0;
bool g_quick = false;

void expect(bool cond, const char* what) {
    printf("%-96s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

i64 lx(u64 nr, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0, u64 e = 0, u64 f = 0) {
    i64 result;
    register u64 r10 asm("r10") = d;
    register u64 r8 asm("r8") = e;
    register u64 r9 asm("r9") = f;
    asm volatile("syscall" : "=a"(result) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return result;
}

constexpr u64 SYS_READ = 0, SYS_WRITE = 1, SYS_CLOSE = 3, SYS_MMAP = 9, SYS_MPROTECT = 10, SYS_MUNMAP = 11, SYS_PIPE = 22, SYS_FORK = 57,
              SYS_WAIT4 = 61, SYS_GETTID = 186, SYS_ARCH_PRCTL = 158, SYS_EXIT_GROUP = 231;

bool mapped(i64 r) { return r > 0 || r < -4095; }

u32 text_length(const char* s) {
    u32 n = 0;
    while (s[n]) ++n;
    return n;
}

bool contains(const char* hay, const char* needle) {
    u32 n = text_length(needle);
    for (u32 i = 0; hay[i]; ++i) {
        u32 k = 0;
        while (k < n && hay[i + k] == needle[k]) ++k;
        if (k == n) return true;
    }
    return n == 0;
}

bool equals(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

u64 g_rng = 0x9E3779B97F4A7C15ull;
u64 rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

void decode_b64(const char* s, u8* out, usize* n) {
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

struct Sample {
    u8 bytes[0x10000];
    usize size;
};

void load_sample(Sample* s, const char* b64) { decode_b64(b64, s->bytes, &s->size); }

bool load_file_sample(Sample* s, const char* path) {
    i64 fd = lx(2, reinterpret_cast<u64>(path), 0);
    if (fd < 0) return false;
    i64 got = lx(SYS_READ, static_cast<u64>(fd), reinterpret_cast<u64>(s->bytes), sizeof(s->bytes));
    lx(SYS_CLOSE, static_cast<u64>(fd));
    if (got <= 0) return false;
    s->size = static_cast<usize>(got);
    return true;
}

struct Region {
    u8* at;
    u64 bytes;
};

struct Protect {
    u8* at;
    u64 bytes;
    u32 prot;
};

struct Call {
    void* function;
    u64 first, second, third;
    void* top;
};

struct Linux {
    bool dry = false;
    int log_fd = -1;
    char logbuf[4096];
    u32 loglen = 0;
    Region regions[64];
    u32 region_count = 0;
    Protect protects[256];
    u32 protect_count = 0;
    Call calls[32];
    u32 call_count = 0;
    u32 fail_reserve_at = 0;
    u32 reserves = 0;
    u32 fail_protect_at = 0;
    bool fail_teb = false;

    void reset() {
        release_all();
        dry = false;
        log_fd = -1;
        loglen = 0;
        logbuf[0] = 0;
        protect_count = 0;
        call_count = 0;
        fail_reserve_at = 0;
        reserves = 0;
        fail_protect_at = 0;
        fail_teb = false;
    }

    u8* reserve(u64 bytes, bool image) {
        ++reserves;
        if (fail_reserve_at != 0 && reserves == fail_reserve_at) return nullptr;
        if (region_count == 64) return nullptr;
        u64 span = bytes + 0x10000;
        i64 r = lx(SYS_MMAP, 0, span, 3, 0x22, static_cast<u64>(-1), 0);
        if (!mapped(r)) return nullptr;
        regions[region_count++] = Region{ reinterpret_cast<u8*>(r), span };
        u64 at = static_cast<u64>(r);
        if (image) at = (at + 0xFFFF) & ~0xFFFFull;
        return reinterpret_cast<u8*>(at);
    }

    bool protect(u8* at, u64 bytes, u32 prot) {
        if (protect_count < 256) protects[protect_count] = Protect{ at, bytes, prot };
        ++protect_count;
        if (fail_protect_at != 0 && protect_count == fail_protect_at) return false;
        return lx(SYS_MPROTECT, reinterpret_cast<u64>(at), bytes, prot) == 0;
    }

    bool set_teb(pehost::Teb* teb) {
        if (fail_teb) return false;
        if (dry) return true;
        return lx(SYS_ARCH_PRCTL, 0x1001, reinterpret_cast<u64>(teb)) == 0;
    }

    u64 thread_id() { return static_cast<u64>(lx(SYS_GETTID)); }

    void log(const char* text) {
        u32 n = text_length(text);
        if (log_fd >= 0) {
            lx(SYS_WRITE, static_cast<u64>(log_fd), reinterpret_cast<u64>(text), n);
            lx(SYS_WRITE, static_cast<u64>(log_fd), reinterpret_cast<u64>("\n"), 1);
        }
        for (u32 i = 0; i < n && loglen + 2 < sizeof(logbuf); ++i) logbuf[loglen++] = text[i];
        if (loglen + 2 < sizeof(logbuf)) logbuf[loglen++] = '\n';
        logbuf[loglen] = 0;
    }

    u64 call(void* function, u64 a, u64 b, u64 c, void* top) {
        if (call_count < 32) calls[call_count] = Call{ function, a, b, c, top };
        ++call_count;
        if (dry) return 0;
        return pehost_call3(function, a, b, c, top);
    }

    void release_all() {
        for (u32 i = 0; i < region_count; ++i) lx(SYS_MUNMAP, reinterpret_cast<u64>(regions[i].at), regions[i].bytes);
        region_count = 0;
    }
};

Linux g_platform;
pehost::Prepared g_prepared;

int g_out_fd = -1;
int g_status_fd = -1;

void child_exit(u32 code) {
    i64 status = static_cast<i32>(code);
    lx(SYS_WRITE, static_cast<u64>(g_status_fd), reinterpret_cast<u64>(&status), 8);
    lx(SYS_EXIT_GROUP, 0);
    for (;;) asm volatile("ud2");
}

bool g_show_stderr = false;

i64 child_write(u32 stream, const void* data, u32 length) {
    if (stream == 2) {
        if (g_show_stderr) lx(SYS_WRITE, 2, reinterpret_cast<u64>(data), length);
        return length;
    }
    return lx(SYS_WRITE, static_cast<u64>(g_out_fd), reinterpret_cast<u64>(data), length);
}

u8* child_reserve(u64 bytes) { return g_platform.reserve(bytes, false); }

bool child_protect(u8* at, u64 bytes, u32 prot) { return g_platform.protect(at, bytes, prot); }

void child_sleep(u32 milliseconds) {
    u64 spec[2] = { milliseconds / 1000, static_cast<u64>(milliseconds % 1000) * 1000000 };
    lx(35, reinterpret_cast<u64>(spec), 0);
}

const char* g_dll_dir = nullptr;
const char* g_hide_file = nullptr;

u8* child_read_file(const char* name, u64* size) {
    if (g_dll_dir == nullptr) return nullptr;
    if (g_hide_file) {
        const char* a = g_hide_file;
        const char* b = name;
        while (*a && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == 0 && *b == 0) return nullptr;
    }
    char path[512];
    u32 n = 0;
    for (const char* p = g_dll_dir; *p && n + 2 < sizeof(path); ++p) path[n++] = *p;
    path[n++] = '/';
    for (const char* p = name; *p && n + 1 < sizeof(path); ++p) path[n++] = *p;
    path[n] = 0;
    i64 fd = lx(2, reinterpret_cast<u64>(path), 0);
    if (fd < 0) return nullptr;
    i64 end = lx(8, static_cast<u64>(fd), 0, 2);
    u8* out = nullptr;
    if (end > 0 && end <= (16 << 20)) {
        i64 map = lx(SYS_MMAP, 0, static_cast<u64>(end), 3, 0x22, static_cast<u64>(-1), 0);
        if (mapped(map)) {
            i64 got = lx(17, static_cast<u64>(fd), static_cast<u64>(map), static_cast<u64>(end), 0);
            if (got == end) {
                out = reinterpret_cast<u8*>(map);
                *size = static_cast<u64>(end);
            } else {
                lx(SYS_MUNMAP, static_cast<u64>(map), static_cast<u64>(end));
            }
        }
    }
    lx(SYS_CLOSE, static_cast<u64>(fd));
    return out;
}

void child_unmap(u8* at, u64 bytes) { lx(SYS_MUNMAP, reinterpret_cast<u64>(at), bytes); }

bool child_clock(pehost::Clock* out) {
    u64 mono[2] = { 0, 0 };
    u64 wall[2] = { 0, 0 };
    if (lx(228, 1, reinterpret_cast<u64>(mono)) != 0 || lx(228, 0, reinterpret_cast<u64>(wall)) != 0) return false;
    out->counter = mono[0] * 1000000000ull + mono[1];
    out->frequency = 1000000000ull;
    out->wall_ms = wall[0] * 1000ull + wall[1] / 1000000ull;
    out->wall_valid = true;
    return true;
}

i64 child_file_open(const char* name, u64* size) {
    if (g_dll_dir == nullptr) return -1;
    char path[512];
    u32 n = 0;
    for (const char* p = g_dll_dir; *p && n + 2 < sizeof(path); ++p) path[n++] = *p;
    path[n++] = '/';
    for (const char* p = name; *p && n + 1 < sizeof(path); ++p) path[n++] = *p;
    path[n] = 0;
    i64 fd = lx(2, reinterpret_cast<u64>(path), 0);
    if (fd < 0) return -1;
    i64 end = lx(8, static_cast<u64>(fd), 0, 2);
    if (end < 0) {
        lx(SYS_CLOSE, static_cast<u64>(fd));
        return -1;
    }
    *size = static_cast<u64>(end);
    return fd;
}

i64 child_file_read(i64 handle, u64 offset, void* buffer, u64 length) { return lx(17, static_cast<u64>(handle), reinterpret_cast<u64>(buffer), length, offset); }

void child_file_close(i64 handle) { lx(SYS_CLOSE, static_cast<u64>(handle)); }

i32 child_dir_entry(u64 index, char* name, u32 capacity, u64* size, u64* mtime_ms, bool* directory) {
    if (g_dll_dir == nullptr) return -1;
    i64 fd = lx(2, reinterpret_cast<u64>(g_dll_dir), 0x10000);
    if (fd < 0) return -1;
    static char buffer[32768];
    u64 seen = 0;
    i32 result = 0;
    for (;;) {
        i64 got = lx(217, static_cast<u64>(fd), reinterpret_cast<u64>(buffer), sizeof(buffer));
        if (got <= 0) break;
        for (i64 at = 0; at < got && result == 0;) {
            u16 reclen = *reinterpret_cast<u16*>(buffer + at + 16);
            const char* entry = buffer + at + 19;
            at += reclen;
            if (entry[0] == '.' && (entry[1] == 0 || (entry[1] == '.' && entry[2] == 0))) continue;
            if (seen++ != index) continue;
            u32 n = 0;
            while (entry[n] && n + 1 < capacity) {
                name[n] = entry[n];
                ++n;
            }
            name[n] = 0;
            u8 st[144];
            i64 r = lx(262, static_cast<u64>(fd), reinterpret_cast<u64>(entry), reinterpret_cast<u64>(st), 0);
            *size = 0;
            *mtime_ms = 0;
            *directory = false;
            if (r == 0) {
                u32 mode = *reinterpret_cast<u32*>(st + 24);
                *directory = (mode & 0170000) == 0040000;
                *size = *reinterpret_cast<u64*>(st + 48);
                *mtime_ms = *reinterpret_cast<u64*>(st + 88) * 1000ull + *reinterpret_cast<u64*>(st + 96) / 1000000ull;
            }
            result = 1;
        }
        if (result) break;
    }
    lx(SYS_CLOSE, static_cast<u64>(fd));
    return result;
}

pehost::Runtime g_runtime = { &child_exit, &child_write, &child_reserve, &child_protect, &child_sleep, &child_read_file, &child_unmap, &child_clock, &child_file_open, &child_file_read, &child_file_close, &child_dir_entry };

const char* g_command_line = "pe.exe";

pehost::ImportTable imports() { return *pehost::all_imports(); }

struct Result {
    bool have_status;
    i64 status;
    char out[8192];
    u32 out_length;
    char log[4096];
    u32 log_length;
    int signal;
};

void drain(int fd, char* into, u32 cap, u32* length) {
    *length = 0;
    for (;;) {
        i64 n = lx(SYS_READ, static_cast<u64>(fd), reinterpret_cast<u64>(into + *length), cap - 1 - *length);
        if (n <= 0) break;
        *length += static_cast<u32>(n);
        if (*length + 1 >= cap) break;
    }
    into[*length] = 0;
}

void run_child(const u8* file, usize size, Result* r) {
    int po[2], pl[2], ps[2];
    lx(SYS_PIPE, reinterpret_cast<u64>(po));
    lx(SYS_PIPE, reinterpret_cast<u64>(pl));
    lx(SYS_PIPE, reinterpret_cast<u64>(ps));
    i64 pid = lx(SYS_FORK);
    if (pid == 0) {
        lx(SYS_CLOSE, static_cast<u64>(po[0]));
        lx(SYS_CLOSE, static_cast<u64>(pl[0]));
        lx(SYS_CLOSE, static_cast<u64>(ps[0]));
        g_out_fd = po[1];
        g_status_fd = ps[1];
        g_platform.reset();
        g_platform.log_fd = pl[1];
        pehost::g_runtime = &g_runtime;
        pehost::ImportTable table = imports();
        i64 st = pehost::load(g_platform, file, size, table, &g_prepared);
        if (st == 0) {
            pehost::attach(g_prepared, pehost::all_imports(), pehost::Config{ g_command_line, "pe.exe" });
            st = pehost::start(g_platform, g_prepared);
        }
        lx(SYS_WRITE, static_cast<u64>(g_status_fd), reinterpret_cast<u64>(&st), 8);
        lx(SYS_EXIT_GROUP, 0);
    }
    lx(SYS_CLOSE, static_cast<u64>(po[1]));
    lx(SYS_CLOSE, static_cast<u64>(pl[1]));
    lx(SYS_CLOSE, static_cast<u64>(ps[1]));
    int wstatus = 0;
    lx(SYS_WAIT4, static_cast<u64>(pid), reinterpret_cast<u64>(&wstatus), 0, 0);
    r->signal = wstatus & 0x7F;
    drain(po[0], r->out, sizeof(r->out), &r->out_length);
    drain(pl[0], r->log, sizeof(r->log), &r->log_length);
    i64 status = 0;
    i64 got = lx(SYS_READ, static_cast<u64>(ps[0]), reinterpret_cast<u64>(&status), 8);
    r->have_status = got == 8;
    r->status = status;
    lx(SYS_CLOSE, static_cast<u64>(po[0]));
    lx(SYS_CLOSE, static_cast<u64>(pl[0]));
    lx(SYS_CLOSE, static_cast<u64>(ps[0]));
}

bool runs_with(const Sample& s, i64 status, const char* out, Result* r) {
    run_child(s.bytes, s.size, r);
    return r->signal == 0 && r->have_status && r->status == status && equals(r->out, out);
}

u32 file_offset(const pe::Image& img, u32 rva) {
    for (u32 i = 0; i < img.section_count; ++i) {
        const pe::Section& s = img.sections[i];
        if (rva >= s.rva && rva - s.rva < s.raw_size) return s.raw_offset + (rva - s.rva);
    }
    return 0;
}

void w32(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}

void w64(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}

u32 r32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24); }

bool known_status(i64 s) {
    return s == 0 || s == pehost::STATUS_INVALID_IMAGE_FORMAT || s == pehost::STATUS_CONFLICTING_ADDRESSES || s == pehost::STATUS_NO_MEMORY ||
           s == pehost::STATUS_ENTRYPOINT_NOT_FOUND;
}

bool prepared_ok(const Linux& plat, const pehost::Prepared& p) {
    const pe::Image& img = p.image;
    if (p.base == nullptr || (reinterpret_cast<u64>(p.base) & 0xFFFF) != 0) return false;
    u32 pages = img.size_of_image / 4096;
    u32 covered = 0;
    for (u32 i = 0; i < plat.protect_count && i < 256; ++i) {
        const Protect& q = plat.protects[i];
        if ((q.prot & pe::PROT_WRITE) && (q.prot & pe::PROT_EXEC)) return false;
        if (q.at != p.base + static_cast<u64>(covered) * 4096 || q.bytes % 4096 != 0) return false;
        covered += static_cast<u32>(q.bytes / 4096);
    }
    if (covered != pages) return false;
    auto executable = [&](u64 rva) {
        for (u32 i = 0; i < plat.protect_count && i < 256; ++i) {
            u64 lo = static_cast<u64>(plat.protects[i].at - p.base);
            if (rva >= lo && rva < lo + plat.protects[i].bytes) return (plat.protects[i].prot & pe::PROT_EXEC) != 0;
        }
        return false;
    };
    if (!executable(img.entry_rva)) return false;
    for (u32 i = 0; i < p.tls.callback_count; ++i)
        if (!executable(p.tls.callbacks[i] - reinterpret_cast<u64>(p.base))) return false;
    const pehost::Teb* teb = p.teb;
    if (teb->self != reinterpret_cast<u64>(teb) || teb->peb != reinterpret_cast<u64>(p.peb)) return false;
    if (p.peb->image_base_address != reinterpret_cast<u64>(p.base)) return false;
    if (teb->stack_base != teb->stack_limit + p.stack_bytes || p.stack_bytes < pehost::MIN_STACK || p.stack_bytes > pehost::MAX_STACK) return false;
    if (p.tls.present) {
        const u64* array = reinterpret_cast<const u64*>(teb->tls_pointer);
        const u8* block = reinterpret_cast<const u8*>(array[0]);
        if (block != p.support + pehost::SUPPORT_HEAD) return false;
        for (u32 i = 0; i < p.tls.template_bytes; ++i)
            if (block[i] != p.base[p.tls.template_rva + i]) return false;
        if (r32(p.base + p.tls.index_rva) != 0) return false;
    } else if (teb->tls_pointer != reinterpret_cast<u64>(p.support + 3 * 4096)) {
        return false;
    }
    return true;
}

}

struct Sweep {
    u32 runs;
    u32 loaded;
    u32 refused;
    u32 violations;
};

void sweep_one(const u8* buf, usize size, Sweep* acc) {
    g_platform.reset();
    g_platform.dry = true;
    pehost::g_runtime = &g_runtime;
    pehost::ImportTable table = imports();
    i64 st = pehost::load(g_platform, buf, size, table, &g_prepared);
    ++acc->runs;
    if (!known_status(st)) ++acc->violations;
    if (st == 0) {
        ++acc->loaded;
        if (!prepared_ok(g_platform, g_prepared)) ++acc->violations;
        pehost::start(g_platform, g_prepared);
    } else {
        ++acc->refused;
    }
    g_platform.release_all();
}

void sweep(const Sample& good, u32 from, u32 to, bool all_values, Sweep* acc) {
    u8* buf = static_cast<u8*>(malloc(good.size));
    for (usize i = 0; i < good.size; ++i) buf[i] = good.bytes[i];
    for (u32 pos = from; pos < to && pos < good.size; ++pos) {
        u8 keep = buf[pos];
        for (u32 v = 0; v < 256; v += all_values ? 1 : 17) {
            u8 val = static_cast<u8>(v);
            if (!all_values && v == 0) val = static_cast<u8>(keep ^ 0x80);
            if (val == keep) continue;
            buf[pos] = val;
            sweep_one(buf, good.size, acc);
        }
        buf[pos] = keep;
    }
    free(buf);
}

void rewrite_sweep(const Sample& good, u32 rounds, u32 span, Sweep* acc) {
    u8* buf = static_cast<u8*>(malloc(good.size));
    for (u32 round = 0; round < rounds; ++round) {
        for (usize i = 0; i < good.size; ++i) buf[i] = good.bytes[i];
        u32 edits = 1 + static_cast<u32>(rnd() % 4);
        for (u32 e = 0; e < edits; ++e) {
            u32 at = static_cast<u32>(rnd() % (span - 8));
            u32 kind = static_cast<u32>(rnd() % 6);
            u32 v = kind == 0 ? 0 : kind == 1 ? 0xFFFFFFFFu : kind == 2 ? static_cast<u32>(rnd()) : kind == 3 ? static_cast<u32>(rnd() % 0x8000) : kind == 4 ? 0x1000 : 0x200;
            if (rnd() & 1) w32(buf + at, v);
            else {
                buf[at] = static_cast<u8>(v);
                buf[at + 1] = static_cast<u8>(v >> 8);
            }
        }
        sweep_one(buf, good.size, acc);
    }
    free(buf);
}

Sample g_console, g_k32, g_teb, g_tlscb, g_fixed, g_ordinals, g_library, g_stdio, g_crt, g_ucrt;

void load_all() {
    load_sample(&g_console, pe_sample_console);
    load_sample(&g_k32, pe_sample_mingw_k32);
    load_sample(&g_teb, pe_sample_teb);
    load_sample(&g_tlscb, pe_sample_tlscb);
    load_sample(&g_fixed, pe_sample_fixed);
    load_sample(&g_ordinals, pe_sample_ordinals);
    load_sample(&g_library, pe_sample_library);
    load_sample(&g_stdio, pe_sample_mingw_stdio);
    load_sample(&g_crt, pe_sample_mingw_crt);
    load_sample(&g_ucrt, pe_sample_mingw_ucrt);
}


struct Recorder {
    u32 writes;
    u32 stream;
    u32 length;
};

Recorder g_recorder;
void recorder_exit(u32) {}
i64 recorder_write(u32 stream, const void*, u32 length) {
    ++g_recorder.writes;
    g_recorder.stream = stream;
    g_recorder.length = length;
    return length;
}
pehost::Runtime g_recording = { &recorder_exit, &recorder_write };

void helper_tests() {
    using pehost::detail::Line;
    Line big;
    for (u32 i = 0; i < 300; ++i) big.add("x");
    expect(big.length == sizeof(big.text) - 1 && big.done()[sizeof(big.text) - 1] == 0 && big.done()[sizeof(big.text) - 2] == 'x', "log lines stop one short of their buffer and stay terminated");
    Line digits;
    for (u32 i = 0; i < 300; ++i) digits.dec(7);
    expect(digits.length == sizeof(digits.text) - 1, "a number is also cut at the buffer limit");
    Line n;
    n.dec(0).add(",").dec(1234567890123ull).add(",").dec(10);
    expect(equals(n.done(), "0,1234567890123,10"), "decimal formatting of 0, a 13 digit number and 10");

    expect(pehost::detail::same_dll("KERNEL32.DLL", "kernel32.dll") && pehost::detail::same_dll("A.dll", "a.DLL") && pehost::detail::same_dll("Z", "z") && pehost::detail::same_dll("az", "AZ"),
           "DLL names compare case-insensitively including the first and last letters");
    expect(!pehost::detail::same_dll("kernel32.dll", "kernel32.dll.x") && !pehost::detail::same_dll("kernel32.dllx", "kernel32.dll") && !pehost::detail::same_dll("a", "") &&
               !pehost::detail::same_dll("", "a") && pehost::detail::same_dll("", ""),
           "DLL names must match to the end on both sides");
    expect(pehost::detail::same_name("ExitProcess", "ExitProcess") && !pehost::detail::same_name("ExitProcess", "exitprocess") && !pehost::detail::same_name("ExitProcess", "ExitProcessA") &&
               !pehost::detail::same_name("ExitProcessA", "ExitProcess") && !pehost::detail::same_name("", "x") && !pehost::detail::same_name("x", ""),
           "function names are exact and must match to the end on both sides");
    expect(!pehost::detail::resolve(nullptr, "kernel32.dll", nullptr, 0, nullptr), "an import by ordinal is never resolved");

    static pehost::Teb fake;
    lx(SYS_ARCH_PRCTL, 0x1001, reinterpret_cast<u64>(&fake));
    pehost::g_runtime = &g_recording;
    using namespace pehost::win32;
    expect(GetStdHandle(pehost::STD_INPUT) == pehost::HANDLE_STDIN && GetStdHandle(pehost::STD_OUTPUT) == pehost::HANDLE_STDOUT && GetStdHandle(pehost::STD_ERROR) == pehost::HANDLE_STDERR,
           "GetStdHandle returns the three standard handles");
    expect(GetStdHandle(0) == pehost::INVALID_HANDLE && GetLastError() == pehost::ERROR_INVALID_HANDLE, "any other id gives an invalid handle and sets the last error");
    SetLastError(1234);
    expect(GetLastError() == 1234 && fake.last_error == 1234, "the last error lives in the TEB at offset 0x68");
    u32 written = 99;
    g_recorder = Recorder{};
    expect(WriteFile(pehost::HANDLE_STDOUT, "abc", 3, &written, nullptr) == 1 && written == 3 && g_recorder.writes == 1 && g_recorder.stream == 1 && g_recorder.length == 3,
           "WriteFile to standard output reports the length and writes to stream 1");
    written = 99;
    expect(WriteFile(pehost::HANDLE_STDERR, "ab", 2, &written, nullptr) == 1 && written == 2 && g_recorder.writes == 2 && g_recorder.stream == 2, "WriteFile to standard error writes to stream 2");
    SetLastError(0);
    written = 99;
    expect(WriteFile(pehost::HANDLE_STDIN, "abc", 3, &written, nullptr) == 0 && written == 0 && g_recorder.writes == 2 && GetLastError() == pehost::ERROR_INVALID_HANDLE,
           "WriteFile to standard input fails with an invalid handle and writes nothing");
    expect(WriteFile(pehost::INVALID_HANDLE, "abc", 3, nullptr, nullptr) == 0 && g_recorder.writes == 2, "WriteFile to an invalid handle fails even without a length pointer");
    SetLastError(0);
    expect(WriteFile(pehost::HANDLE_STDOUT, "abc", 3, &written, &fake) == 0 && GetLastError() == pehost::ERROR_INVALID_PARAMETER && g_recorder.writes == 2,
           "overlapped output is refused with ERROR_INVALID_PARAMETER");
    expect(WriteFile(pehost::HANDLE_STDOUT, "", 0, nullptr, nullptr) == 1 && g_recorder.writes == 3 && g_recorder.length == 0, "a zero length write succeeds");
    pehost::g_runtime = &g_runtime;
}

void tls_tests() {
    static Sample p;
    static pe::Image img;
    pehost::ImportTable table = imports();
    pehost::g_runtime = &g_runtime;
    for (usize i = 0; i < g_tlscb.size; ++i) p.bytes[i] = g_tlscb.bytes[i];
    p.size = g_tlscb.size;
    pe::parse(p.bytes, p.size, &img);
    u32 dir = file_offset(img, img.directories[pe::DIR_TLS].rva);

    w32(p.bytes + dir + 36, 13u << 20);
    g_platform.reset();
    g_platform.dry = true;
    i64 st = pehost::load(g_platform, p.bytes, p.size, table, &g_prepared);
    expect(st == 0, "TLS aligned to exactly one page is accepted");
    g_platform.release_all();

    w32(p.bytes + dir + 36, 0);
    u32 template_bytes = 288;
    w32(p.bytes + dir + 32, 0x100000 - template_bytes);
    g_platform.reset();
    g_platform.dry = true;
    st = pehost::load(g_platform, p.bytes, p.size, table, &g_prepared);
    expect(st == 0 && g_platform.region_count == 3 && g_platform.regions[1].bytes == pehost::SUPPORT_HEAD + 0x100000 + 0x10000, "TLS of exactly 1 MiB is accepted and the thread block is sized for it");
    g_platform.release_all();
    w32(p.bytes + dir + 32, 0x100000 - template_bytes + 1);
    g_platform.reset();
    g_platform.dry = true;
    st = pehost::load(g_platform, p.bytes, p.size, table, &g_prepared);
    expect(st == pehost::STATUS_INVALID_IMAGE_FORMAT, "one byte more is refused");
    g_platform.release_all();

    w32(p.bytes + dir + 32, 4096 + 8);
    u32 after = file_offset(img, img.sections[3].rva) + template_bytes;
    p.bytes[after] = 0xFF;
    g_platform.reset();
    g_platform.dry = true;
    st = pehost::load(g_platform, p.bytes, p.size, table, &g_prepared);
    const u8* block = g_prepared.support + pehost::SUPPORT_HEAD;
    expect(st == 0 && g_platform.regions[1].bytes == pehost::SUPPORT_HEAD + 2 * 4096 + 0x10000 && block[template_bytes] == 0 && block[template_bytes + 7] == 0,
           "zero fill follows the template as zeroes even when the image has other bytes there, and sizes the block");
    g_platform.release_all();

    g_platform.reset();
    g_platform.dry = true;
    g_platform.fail_teb = true;
    st = pehost::load(g_platform, g_console.bytes, g_console.size, table, &g_prepared);
    expect(st == 0, "the load itself does not touch the thread pointer");
    st = pehost::start(g_platform, g_prepared);
    expect(st == pehost::STATUS_NO_MEMORY && contains(g_platform.logbuf, "thread block") && g_platform.call_count == 0, "a failure to install the TEB stops the program before anything runs");
    g_platform.release_all();

    g_platform.reset();
    g_platform.dry = true;
    u32 min_stack_ok = 0;
    for (usize i = 0; i < g_console.size; ++i) p.bytes[i] = g_console.bytes[i];
    p.size = g_console.size;
    u32 pe_at = r32(p.bytes + 0x3C);
    u64 asks[6] = { 0x10000, 0x10001, 0xFFFF, 0x100000, 0x100001, 0x4000000 };
    u64 gets[6] = { 0x10000, 0x11000, 0x10000, 0x100000, 0x100000, 0x100000 };
    for (u32 i = 0; i < 6; ++i) {
        w64(p.bytes + pe_at + 24 + 72, asks[i]);
        g_platform.reset();
        g_platform.dry = true;
        st = pehost::load(g_platform, p.bytes, p.size, table, &g_prepared);
        if (st == 0 && g_prepared.stack_bytes == gets[i]) ++min_stack_ok;
        g_platform.release_all();
    }
    expect(min_stack_ok == 6, "stack sizes: exact limits stay, in-between values round up to a page, values outside are clamped");
}

void run_tests() {
    static Result r;
    expect(g_console.size > 0 && g_k32.size > 0 && g_teb.size > 0 && g_tlscb.size > 0, "the sample programs decode");

    expect(runs_with(g_console, 9, "hello from pe\n", &r), "console.exe (lld): relocated pointer table, TLS and WriteFile run; the exit code is 3 + 6");
    expect(r.log_length == 0, "console.exe: the loader logs nothing");
    expect(runs_with(g_k32, 0, "Hello World\n", &r), "k32.exe (GNU ld, no relocation table): runs and writes through GetStdHandle and WriteFile");
    expect(runs_with(g_teb, 0, "", &r), "teb.exe: GS points at a TEB with a self pointer, PEB image base, stack bounds, TLS array, process id and an entry stack at 8 mod 16");
    expect(runs_with(g_tlscb, 0, "", &r), "tlscb.exe: two TLS callbacks run with the module base and reason 1, TLS data starts initialised and zeroed and can be written");

    run_child(g_fixed.bytes, g_fixed.size, &r);
    expect(r.have_status && r.status == pehost::STATUS_CONFLICTING_ADDRESSES && contains(r.log, "preferred base") && r.out_length == 0,
           "a fixed-base program is refused with STATUS_CONFLICTING_ADDRESSES and a reason");
    run_child(g_library.bytes, g_library.size, &r);
    expect(r.have_status && r.status == pehost::STATUS_INVALID_IMAGE_FORMAT && contains(r.log, "DLL"), "a DLL is refused as a program");
    run_child(g_ordinals.bytes, g_ordinals.size, &r);
    expect(r.have_status && r.status == pehost::STATUS_ENTRYPOINT_NOT_FOUND && contains(r.log, "mylib.dll!Alpha") && contains(r.log, "mylib.dll!Beta") &&
               contains(r.log, "mylib.dll!#7") && contains(r.log, "3 of 4 imports are not provided") && r.out_length == 0,
           "missing imports by name and by ordinal are each named, counted, and nothing runs");
    expect(runs_with(g_stdio, 0, "Hello World\n", &r), "stdio.exe (GNU ld, msvcrt printf): C runtime start-up, printf and exit run to completion");
    expect(runs_with(g_crt, 7, "argc=1\nargv[0]=<pe.exe>\ndirect 5 msvcrt    ab|\ndone failures=0\nexit handler 2\nexit handler 1\n", &r),
           "crt.exe (msvcrt): arguments, allocator, formatting, strings, signal, heap, TLS, critical sections, modules and code pages all pass, exit handlers run in reverse, exit status 7");
    expect(runs_with(g_ucrt, 7, "argc=1\nargv[0]=<pe.exe>\ndirect 5 ucrt    ab|\na line\nno newline;\ndone failures=0\nexit handler 2\nexit handler 1\n", &r),
           "ucrt.exe (api-ms-win-crt sets, ucrtbase.dll): UCRT start-up, __stdio_common_* printf, snprintf, allocator, strings, LoadLibrary and GetProcAddress all pass, exit status 7");
    g_command_line = "pe.exe \"two words\" caf\xC3\xA9 last";
    expect(runs_with(g_ucrt, 7, "argc=4\nargv[0]=<pe.exe>\nargv[1]=<two words>\nargv[2]=<caf\xC3\xA9>\nargv[3]=<last>\ndirect 5 ucrt    ab|\na line\nno newline;\ndone failures=0\nexit handler 2\nexit handler 1\n", &r),
           "ucrt.exe sees its quoted and UTF-8 command line arguments through _configure_narrow_argv");
    g_command_line = "pe.exe";
    {
        static Sample dyn;
        expect(load_file_sample(&dyn, "tools/pe-samples/bin/dyn.exe"), "dyn.exe is readable from tools/pe-samples/bin (run the tests from the repository root)");
        g_dll_dir = "tools/pe-samples/bin";
        expect(runs_with(dyn, 7,
                         "dllmath reason=1 reserved=1\ndllchain reason=1 reserved=1\nstart\ndllplug reason=1 reserved=0\ndllplug reason=0 reserved=0\ndllplug reason=1 reserved=0\n"
                         "done failures=0\ndllplug reason=0 reserved=1\ndllchain reason=0 reserved=1\ndllmath reason=0 reserved=1\n",
                         &r),
               "dyn.exe: two DLLs loaded in dependency order before the program starts, forwarders, ordinals, exported data, LoadLibrary and FreeLibrary at run time, a DLL whose DllMain fails refused, and every DLL detached in reverse order at exit");
        g_hide_file = "dllmath.dll";
        run_child(dyn.bytes, dyn.size, &r);
        expect(r.have_status && r.status == pehost::STATUS_ENTRYPOINT_NOT_FOUND && contains(r.log, "missing import dllchain.dll!add3") && r.out_length == 0,
               "a program whose DLL cannot be loaded because that DLL's own dependency is missing is refused, naming the import, and nothing runs");
        g_hide_file = nullptr;
        g_dll_dir = nullptr;
        run_child(dyn.bytes, dyn.size, &r);
        expect(r.have_status && r.status == pehost::STATUS_ENTRYPOINT_NOT_FOUND && contains(r.log, "missing import dllchain.dll!add3") && r.out_length == 0,
               "without a place to look for DLLs the same program is refused too");
    }

    run_child(g_console.bytes, 100, &r);
    expect(r.have_status && r.status == pehost::STATUS_INVALID_IMAGE_FORMAT && contains(r.log, "image refused"), "a truncated file is refused");
    run_child(g_console.bytes, 0, &r);
    expect(r.have_status && r.status == pehost::STATUS_INVALID_IMAGE_FORMAT, "an empty file is refused");

    pehost::ImportTable table = imports();
    pehost::g_runtime = &g_runtime;

    g_platform.reset();
    g_platform.dry = true;
    i64 st = pehost::load(g_platform, g_console.bytes, g_console.size, table, &g_prepared);
    const pehost::Prepared& p = g_prepared;
    expect(st == 0 && prepared_ok(g_platform, p), "console.exe: base aligned, protections tile the image without a writable executable run, TEB and PEB consistent, TLS block initialised");
    expect(p.relocations == 7 && p.tls.present && p.tls.template_bytes == 132 && p.stack_bytes == 0x100000, "console.exe: 7 relocations applied, a 132 byte TLS template, the 1 MiB stack it asks for");
    u64 in_range = 0;
    for (u32 i = 0; i + 8 <= p.image.size_of_image; i += 8) {
        u64 v = 0;
        for (int k = 0; k < 8; ++k) v |= static_cast<u64>(p.base[i + k]) << (8 * k);
        if (v >= reinterpret_cast<u64>(p.base) && v < reinterpret_cast<u64>(p.base) + p.image.size_of_image) ++in_range;
    }
    expect(in_range >= 4, "console.exe: the pointer table now holds addresses inside the loaded image");
    pehost::start(g_platform, g_prepared);
    expect(g_platform.call_count == 1 && g_platform.calls[0].function == p.base + p.image.entry_rva && g_platform.calls[0].first == reinterpret_cast<u64>(p.peb) &&
               g_platform.calls[0].top == p.stack_base + p.stack_bytes,
           "console.exe: the entry is called once with the PEB on top of the new stack");
    g_platform.release_all();

    g_platform.reset();
    g_platform.dry = true;
    st = pehost::load(g_platform, g_tlscb.bytes, g_tlscb.size, table, &g_prepared);
    expect(st == 0 && prepared_ok(g_platform, g_prepared) && g_prepared.tls.callback_count == 2 && g_prepared.tls.template_bytes == 288, "tlscb.exe: two callbacks and a 288 byte template are prepared");
    pehost::start(g_platform, g_prepared);
    u64 module = reinterpret_cast<u64>(g_prepared.base);
    expect(g_platform.call_count == 3 && g_platform.calls[0].first == module && g_platform.calls[0].second == 1 && g_platform.calls[0].third == 0 &&
               g_platform.calls[1].first == module && g_platform.calls[1].second == 1 && g_platform.calls[2].function == g_prepared.base + g_prepared.image.entry_rva,
           "tlscb.exe: both callbacks are called before the entry, in table order");
    expect(reinterpret_cast<u64>(g_platform.calls[0].function) == g_prepared.tls.callbacks[0] && reinterpret_cast<u64>(g_platform.calls[1].function) == g_prepared.tls.callbacks[1],
           "tlscb.exe: the callback addresses are the relocated ones");
    g_platform.release_all();

    g_platform.reset();
    g_platform.dry = true;
    st = pehost::load(g_platform, g_k32.bytes, g_k32.size, table, &g_prepared);
    expect(st == 0 && g_prepared.relocations == 0 && g_prepared.stack_bytes == pehost::MAX_STACK && !g_prepared.tls.present, "k32.exe: moved with nothing to patch, a 2 MiB stack request is clamped to 1 MiB");
    g_platform.release_all();

    static Sample patched;
    for (usize i = 0; i < g_console.size; ++i) patched.bytes[i] = g_console.bytes[i];
    patched.size = g_console.size;
    static pe::Image img;
    pe::parse(patched.bytes, patched.size, &img);
    u32 pe_at = r32(patched.bytes + 0x3C);
    w64(patched.bytes + pe_at + 24 + 72, 0x1000);
    g_platform.reset();
    g_platform.dry = true;
    st = pehost::load(g_platform, patched.bytes, patched.size, table, &g_prepared);
    expect(st == 0 && g_prepared.stack_bytes == pehost::MIN_STACK, "a 4 KiB stack request is raised to 64 KiB");
    g_platform.release_all();

    for (usize i = 0; i < g_tlscb.size; ++i) patched.bytes[i] = g_tlscb.bytes[i];
    patched.size = g_tlscb.size;
    pe::parse(patched.bytes, patched.size, &img);
    u32 tls_at = file_offset(img, img.directories[pe::DIR_TLS].rva);
    w32(patched.bytes + tls_at + 32, 0x200000);
    g_platform.reset();
    g_platform.dry = true;
    st = pehost::load(g_platform, patched.bytes, patched.size, table, &g_prepared);
    expect(st == pehost::STATUS_INVALID_IMAGE_FORMAT && contains(g_platform.logbuf, "1 MiB"), "TLS of more than 1 MiB per thread is refused");
    g_platform.release_all();
    w32(patched.bytes + tls_at + 32, 0);
    w32(patched.bytes + tls_at + 36, 14u << 20);
    g_platform.reset();
    g_platform.dry = true;
    st = pehost::load(g_platform, patched.bytes, patched.size, table, &g_prepared);
    expect(st == pehost::STATUS_INVALID_IMAGE_FORMAT && contains(g_platform.logbuf, "alignment"), "TLS aligned above one page is refused");
    g_platform.release_all();

    bool every_reserve = true;
    for (u32 n = 1; n <= 3; ++n) {
        g_platform.reset();
        g_platform.dry = true;
        g_platform.fail_reserve_at = n;
        st = pehost::load(g_platform, g_console.bytes, g_console.size, table, &g_prepared);
        every_reserve = every_reserve && st == pehost::STATUS_NO_MEMORY && g_platform.loglen > 0;
        g_platform.release_all();
    }
    expect(every_reserve, "failing the image, the thread block or the stack allocation each gives STATUS_NO_MEMORY with a message");
    bool every_protect = true;
    for (u32 n = 1; n <= 7; ++n) {
        g_platform.reset();
        g_platform.dry = true;
        g_platform.fail_protect_at = n;
        st = pehost::load(g_platform, g_console.bytes, g_console.size, table, &g_prepared);
        every_protect = every_protect && st == pehost::STATUS_NO_MEMORY && contains(g_platform.logbuf, "protected");
        g_platform.release_all();
    }
    expect(every_protect, "failing any one of the seven protection calls is reported and stops the load");

    Sweep a{}, b{}, c{}, d{};
    sweep(g_console, 0, g_console.size, false, &a);
    sweep(g_k32, 0, g_quick ? 0x300 : g_k32.size, false, &b);
    sweep(g_tlscb, 0, g_quick ? 0x300 : g_tlscb.size, false, &c);
    sweep(g_teb, 0, g_quick ? 0x100 : 0x400, !g_quick, &d);
    expect(a.violations + b.violations + c.violations + d.violations == 0, "sweep: every mutated byte of four programs either fails with a known status or yields a consistent load");
    printf("    (%u runs, %u loaded, %u refused)\n", a.runs + b.runs + c.runs + d.runs, a.loaded + b.loaded + c.loaded + d.loaded, a.refused + b.refused + c.refused + d.refused);
    expect(a.refused > 500 && a.loaded > 500, "sweep: both outcomes are well represented");
    Sweep e{};
    rewrite_sweep(g_console, g_quick ? 5000 : 60000, 0x300, &e);
    rewrite_sweep(g_tlscb, g_quick ? 5000 : 60000, 0x300, &e);
    expect(e.violations == 0, "sweep: random multi-field header rewrites never give an inconsistent load");
    printf("    (%u runs, %u loaded)\n", e.runs, e.loaded);
}

int run_file(const char* path, const char* command_line) {
    i64 fd = lx(2, reinterpret_cast<u64>(path), 0);
    if (fd < 0) {
        printf("cannot open %s\n", path);
        return 2;
    }
    constexpr u64 LIMIT = 16u << 20;
    i64 map = lx(SYS_MMAP, 0, LIMIT, 3, 0x22, static_cast<u64>(-1), 0);
    if (!mapped(map)) return 2;
    u8* bytes = reinterpret_cast<u8*>(map);
    u64 have = 0;
    for (;;) {
        i64 n = lx(SYS_READ, static_cast<u64>(fd), reinterpret_cast<u64>(bytes + have), LIMIT - have);
        if (n <= 0) break;
        have += static_cast<u64>(n);
    }
    lx(SYS_CLOSE, static_cast<u64>(fd));
    static Result r;
    g_command_line = command_line;
    run_child(bytes, have, &r);
    printf("%s", r.out);
    if (r.log_length) printf("[host log]\n%s", r.log);
    if (r.signal) {
        printf("[host] the program died with signal %d\n", r.signal);
        return 1;
    }
    printf("[host] exit status %lld (0x%llx)\n", static_cast<long long>(r.status), static_cast<unsigned long long>(r.status) & 0xFFFFFFFFull);
    return r.have_status ? 0 : 1;
}

int list_supported() {
    for (const pehost::ImportTable* t = pehost::all_imports(); t; t = t->next) {
        bool stub = t == pehost::kernel32_stub_table();
        for (u32 i = 0; i < t->count; ++i) printf("%s!%s%s\n", t->items[i].dll, t->items[i].name, stub ? " stub" : "");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && equals(argv[1], "--list-supported")) return list_supported();
    if (argc > 2 && equals(argv[1], "--run")) {
        g_show_stderr = true;
        const char* line = "pe.exe";
        for (int i = 3; i < argc; ++i) {
            if (equals(argv[i], "--dlls") && i + 1 < argc) g_dll_dir = argv[++i];
            else line = argv[i];
        }
        return run_file(argv[2], line);
    }
    g_quick = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q';
    load_all();
    run_tests();
    helper_tests();
    tls_tests();
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
