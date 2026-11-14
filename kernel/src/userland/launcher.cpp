#include <cosmos/userland_apps.hpp>
#include <cosmos/userland.hpp>
#include <cosmos/lens_channel.hpp>
#include <cosmos/startup.hpp>
#include <cosmos/lens_text.hpp>
#include <cosmos/mutex.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

extern "C" const u8 apx_hello_start[];
extern "C" const u8 apx_hello_end[];
extern "C" const u8 apx_echo_start[];
extern "C" const u8 apx_echo_end[];
extern "C" const u8 apx_pehost_start[];
extern "C" const u8 apx_pehost_end[];
extern "C" const u8 pe_console_start[];
extern "C" const u8 pe_console_end[];
extern "C" const u8 pe_k32_start[];
extern "C" const u8 pe_k32_end[];
extern "C" const u8 pe_teb_start[];
extern "C" const u8 pe_teb_end[];
extern "C" const u8 pe_tlscb_start[];
extern "C" const u8 pe_tlscb_end[];
extern "C" const u8 pe_stdio_start[];
extern "C" const u8 pe_stdio_end[];
extern "C" const u8 pe_crt_start[];
extern "C" const u8 pe_crt_end[];
extern "C" const u8 pe_ucrt_start[];
extern "C" const u8 pe_ucrt_end[];
extern "C" const u8 pe_dllmath_start[];
extern "C" const u8 pe_dllmath_end[];
extern "C" const u8 pe_dllchain_start[];
extern "C" const u8 pe_dllchain_end[];
extern "C" const u8 pe_dllplug_start[];
extern "C" const u8 pe_dllplug_end[];
extern "C" const u8 pe_dllfail_start[];
extern "C" const u8 pe_dllfail_end[];
extern "C" const u8 pe_dyn_start[];
extern "C" const u8 pe_dyn_end[];
extern "C" const u8 pe_sysio_start[];
extern "C" const u8 pe_sysio_end[];
extern "C" const u8 pe_argv_start[];
extern "C" const u8 pe_argv_end[];
extern "C" const u8 pe_files_start[];
extern "C" const u8 pe_files_end[];
extern "C" const u8 pe_threads_start[];
extern "C" const u8 pe_threads_end[];
extern "C" const u8 pe_thrdll_start[];
extern "C" const u8 pe_thrdll_end[];

namespace userland::apps {

namespace {

struct Entry {
    const char* name;
    const u8* begin;
    const u8* end;
};

const Entry ENTRIES[] = {
    { "hello", apx_hello_start, apx_hello_end },
    { "echo", apx_echo_start, apx_echo_end },
    { "pehost", apx_pehost_start, apx_pehost_end },
    { "console.exe", pe_console_start, pe_console_end },
    { "k32.exe", pe_k32_start, pe_k32_end },
    { "teb.exe", pe_teb_start, pe_teb_end },
    { "tlscb.exe", pe_tlscb_start, pe_tlscb_end },
    { "stdio.exe", pe_stdio_start, pe_stdio_end },
    { "crt.exe", pe_crt_start, pe_crt_end },
    { "ucrt.exe", pe_ucrt_start, pe_ucrt_end },
    { "dllmath.dll", pe_dllmath_start, pe_dllmath_end },
    { "dllchain.dll", pe_dllchain_start, pe_dllchain_end },
    { "dllplug.dll", pe_dllplug_start, pe_dllplug_end },
    { "dllfail.dll", pe_dllfail_start, pe_dllfail_end },
    { "dyn.exe", pe_dyn_start, pe_dyn_end },
    { "sysio.exe", pe_sysio_start, pe_sysio_end },
    { "argv.exe", pe_argv_start, pe_argv_end },
    { "files.exe", pe_files_start, pe_files_end },
    { "threads.exe", pe_threads_start, pe_threads_end },
    { "thrdll.dll", pe_thrdll_start, pe_thrdll_end },
};

constexpr const char* DIRECTORY = "apps";
constexpr u32 CAPTURE_BYTES = 8192;
constexpr u64 POLL_MS = 40;
constexpr u32 KILL_ROUNDS = 100;

lens::Table g_table;
orbital::Mutex g_gate;

struct Capture {
    cpu::Spinlock lock;
    char data[CAPTURE_BYTES];
    u32 head;
    u32 tail;
    u32 dropped;
};

Capture g_capture;

void capture_put(void*, const char* text, u32 length) {
    u64 fl = cpu::irq_save();
    g_capture.lock.lock();
    for (u32 i = 0; i < length; ++i) {
        u32 next = (g_capture.head + 1) % CAPTURE_BYTES;
        if (next == g_capture.tail) {
            ++g_capture.dropped;
            continue;
        }
        g_capture.data[g_capture.head] = text[i];
        g_capture.head = next;
    }
    g_capture.lock.unlock();
    cpu::irq_restore(fl);
}

void drain(Emit emit, void* ctx) {
    char chunk[256];
    for (;;) {
        u32 n = 0;
        u64 fl = cpu::irq_save();
        g_capture.lock.lock();
        while (n < sizeof(chunk) && g_capture.tail != g_capture.head) {
            chunk[n++] = g_capture.data[g_capture.tail];
            g_capture.tail = (g_capture.tail + 1) % CAPTURE_BYTES;
        }
        g_capture.lock.unlock();
        cpu::irq_restore(fl);
        if (n == 0) return;
        if (emit) emit(ctx, chunk, n);
    }
}

bool plain_name(const char* name) {
    if (!name || !name[0]) return false;
    for (u32 i = 0; name[i]; ++i) {
        if (i >= 48 || name[i] == '/' || name[i] == ':' || name[i] == '\\') return false;
    }
    return true;
}

constexpr u32 NAME_CAP = 56;

bool has_dot(const char* name) {
    for (u32 i = 0; name[i]; ++i) {
        if (name[i] == '.') return true;
    }
    return false;
}

void with_suffix(const char* name, char* out) {
    u32 n = 0;
    while (name[n] && n + 1 < NAME_CAP) {
        out[n] = name[n];
        ++n;
    }
    out[n] = 0;
    if (!has_dot(name)) {
        const char suffix[] = ".exe";
        for (u32 i = 0; i < sizeof(suffix) && n + i < NAME_CAP; ++i) out[n + i] = suffix[i];
        out[NAME_CAP - 1] = 0;
    }
}

bool probe_program(lens::Lens dir, const char* name, char* canon, bool* is_pe) {
    char suffixed[NAME_CAP];
    with_suffix(name, suffixed);
    const char* tries[2] = { name, suffixed };
    for (const char* candidate : tries) {
        lens::Result<lens::Lens> probe = lens::open_at(g_table.domain, dir, candidate, lens::Right::Read);
        if (!probe.ok()) continue;
        u32 n = 0;
        while (candidate[n] && n + 1 < NAME_CAP) {
            canon[n] = candidate[n];
            ++n;
        }
        canon[n] = 0;
        u8 magic[2] = { 0, 0 };
        lens::Result<usize> got = lens::read(g_table.domain, probe.value(), 0, lens::Bytes(magic, sizeof(magic)));
        *is_pe = got.ok() && got.value() == 2 && magic[0] == 'M' && magic[1] == 'Z';
        return true;
    }
    return false;
}

bool root_of(lens::Lens* out) {
    lens::Result<lens::Lens> r = lens::delegate(lens::boot_domain(), lens::boot_root("ram"), g_table.domain, lens::Rights::all());
    if (!r.ok()) return false;
    *out = r.value();
    return true;
}

}

bool exists(const char* name) {
    if (!supported() || !plain_name(name)) return false;
    g_gate.lock();
    bool found = false;
    lens::Lens root;
    if (root_of(&root)) {
        lens::Result<lens::Lens> dir = lens::open_at(g_table.domain, root, DIRECTORY, lens::Right::Read);
        char canon[NAME_CAP];
        bool is_pe = false;
        found = dir.ok() && probe_program(dir.value(), name, canon, &is_pe);
    }
    lens::close_all(g_table.domain);
    g_gate.unlock();
    return found;
}

bool install() {
    g_gate.lock();
    bool ok = false;
    lens::Lens root;
    if (root_of(&root)) {
        (void)lens::remove_tree(g_table.domain, root, DIRECTORY);
        ok = lens::mkdir(g_table.domain, root, DIRECTORY).ok();
        if (ok) {
            lens::Result<lens::Lens> dir = lens::open_at(g_table.domain, root, DIRECTORY, lens::Rights::all());
            ok = dir.ok();
            for (const Entry& e : ENTRIES) {
                if (!ok) break;
                ok = lens::put(g_table.domain, dir.value(), e.name, lens::ConstBytes(e.begin, static_cast<usize>(e.end - e.begin))).ok();
            }
        }
    }
    lens::close_all(g_table.domain);
    g_gate.unlock();
    serial::printf("[apps] %s the bundled programs in ram:/%s\n", ok ? "installed" : "could not install", DIRECTORY);
    return ok;
}

Report run(const char* name, Emit emit, Interrupted interrupted, void* ctx) {
    Request request;
    request.name = name;
    request.emit = emit;
    request.interrupted = interrupted;
    request.ctx = ctx;
    return run(request);
}

Report run(const Request& request) {
    const char* name = request.name;
    Emit emit = request.emit;
    Interrupted interrupted = request.interrupted;
    void* ctx = request.ctx;
    Report report{ Outcome::Finished, 0, 0 };
    if (!supported()) {
        report.outcome = Outcome::Unavailable;
        return report;
    }
    if (!plain_name(name)) {
        report.outcome = Outcome::BadName;
        return report;
    }
    if (request.input_length > MAX_INPUT || (request.input_length && !request.input)) {
        report.outcome = Outcome::InputTooLarge;
        return report;
    }
    const char* argv[startup::MAX_ARGS];
    u32 argc = 0;
    if (request.arg_count > startup::MAX_ARGS - 1 || (request.arg_count && !request.args)) {
        report.outcome = Outcome::BadArguments;
        return report;
    }
    char canon[NAME_CAP];
    with_suffix(name, canon);
    argv[argc++] = canon;
    for (u32 i = 0; i < request.arg_count; ++i) {
        if (!request.args[i]) {
            report.outcome = Outcome::BadArguments;
            return report;
        }
        argv[argc++] = request.args[i];
    }
    {
        u8 probe_message[startup::MAX_MESSAGE];
        u32 probe_length = 0;
        if (startup::encode(argv, argc, request.env, request.env_count, 0, probe_message, sizeof(probe_message), &probe_length) != startup::OK) {
            report.outcome = Outcome::BadArguments;
            return report;
        }
    }
    g_gate.lock();
    lens::Lens root;
    lens::Lens dir;
    bool have_dir = false;
    if (root_of(&root)) {
        lens::Result<lens::Lens> d = lens::open_at(g_table.domain, root, DIRECTORY, lens::Rights::all());
        if (d.ok()) {
            dir = d.value();
            have_dir = true;
        }
    }
    bool is_pe = false;
    bool found = false;
    if (have_dir) found = probe_program(dir, name, canon, &is_pe);
    if (!found) {
        lens::close_all(g_table.domain);
        g_gate.unlock();
        report.outcome = Outcome::NotFound;
        return report;
    }

    Startup begin;
    begin.argv = argv;
    begin.argc = argc;
    begin.env = request.env;
    begin.envc = request.env_count;
    bool prepared = true;
    lens::Result<lens::Lens> cwd = lens::open_at(g_table.domain, root, DIRECTORY, lens::Right::Read | lens::Right::Enumerate | lens::Right::Delegate);
    prepared = cwd.ok();
    if (prepared) begin.directory = cwd.value();
    if (prepared && request.work_domain && !request.work.is_null()) {
        lens::Result<lens::Rights> have = lens::rights_of(*request.work_domain, request.work);
        if (have.ok()) {
            lens::Rights mine = have.value() & (lens::RIGHTS_READ_WRITE | lens::Right::Delegate);
            lens::Result<lens::Lens> work = lens::delegate(*request.work_domain, request.work, g_table.domain, mine);
            if (work.ok()) begin.work = work.value();
        }
    }
    if (prepared && request.input) {
        lens::Lens reader, writer;
        usize size = request.input_length ? request.input_length : 1;
        prepared = lens::make_pipe(g_table.domain, size, &reader, &writer).ok();
        if (prepared && request.input_length) {
            lens::Result<usize> put = lens::write(g_table.domain, writer, 0, lens::ConstBytes(reinterpret_cast<const u8*>(request.input), request.input_length));
            prepared = put.ok() && put.value() == request.input_length;
        }
        if (prepared) {
            prepared = lens::close(g_table.domain, writer).ok();
            begin.input = reader;
        }
    }

    Process* p = nullptr;
    i64 error = -1;
    lens::Lens launched;
    const char* label = nullptr;
    if (prepared && is_pe) {
        lens::Result<lens::Lens> host = lens::open_at(g_table.domain, dir, "pehost", lens::Right::Read | lens::Right::Execute);
        lens::Result<lens::Lens> prog = lens::open_at(g_table.domain, dir, canon, lens::Right::Read | lens::Right::Delegate);
        if (host.ok() && prog.ok()) {
            begin.program = prog.value();
            launched = host.value();
            label = "pehost";
        }
    } else if (prepared) {
        lens::Result<lens::Lens> file = lens::open_at(g_table.domain, dir, canon, lens::Right::Read | lens::Right::Execute);
        if (file.ok()) launched = file.value();
    }
    if (!launched.is_null()) {
        g_capture.head = g_capture.tail = g_capture.dropped = 0;
        set_console(&capture_put, nullptr);
        p = launch_started(label, g_table, launched, begin, &error);
    }
    if (!p) {
        set_console(nullptr, nullptr);
        lens::close_all(g_table.domain);
        g_gate.unlock();
        report.outcome = Outcome::LaunchFailed;
        report.error = error;
        return report;
    }

    bool finished = false;
    bool killed = false;
    u32 rounds_after_kill = 0;
    i32 status = -1;
    for (;;) {
        finished = wait(p, &status, POLL_MS);
        drain(emit, ctx);
        if (finished) break;
        if (!killed && interrupted && interrupted(ctx)) {
            killed = true;
            (void)kill(p);
        }
        if (killed && ++rounds_after_kill >= KILL_ROUNDS) break;
    }
    set_console(nullptr, nullptr);
    drain(emit, ctx);
    if (finished) (void)reap(p);
    lens::close_all(g_table.domain);
    g_gate.unlock();
    if (g_capture.dropped && emit) {
        const char note[] = "\n[output was truncated]\n";
        emit(ctx, note, sizeof(note) - 1);
    }
    report.outcome = killed ? Outcome::Interrupted : Outcome::Finished;
    report.status = status;
    return report;
}

}
