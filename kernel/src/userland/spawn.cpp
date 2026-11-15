#include "internal.hpp"
#include <cosmos/handleabi.hpp>
#include <cosmos/fileabi.hpp>
#include <cosmos/apx.hpp>
#include <cosmos/startup.hpp>

namespace userland {

namespace impl {

struct Loaded {
    u8* buf = nullptr;
    apx::Header header{};
    char label[orbital::NAME_BYTES] = {};
    Loaded() = default;
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    ~Loaded() { if (buf) kheap::free(buf); }
};

lens::Status read_exact(lens::Table& t, lens::Lens l, u8* dst, u64 n) {
    u64 got = 0;
    while (got < n) {
        lens::Result<usize> r = lens::read(t.domain, l, got, lens::Bytes(dst + got, static_cast<usize>(n - got)));
        if (!r.ok()) return r.status();
        if (r.value() == 0) return lens::Status::Io;
        got += r.value();
    }
    return lens::Status::Ok;
}

i64 load_apx(lens::Table& t, lens::Lens file, Loaded* out) {
    TableGuard guard(t.lock);
    lens::Result<lens::Slot*> sr = lens::slot_of(t.domain, file);
    if (!sr.ok()) return fileabi::error_of(sr.status());
    lens::Slot* s = sr.value();
    if (s->kind != lens::NodeKind::Blob) return fileabi::ERR_WRONGTYPE;
    if (!s->rights.has_all(fileabi::LAUNCH_RIGHTS)) return handleabi::ERR_DENIED;
    lens::Result<u64> sz = lens::size(t.domain, file);
    if (!sz.ok()) return fileabi::error_of(sz.status());
    u64 size = sz.value();
    if (size < apx::HEADER_BYTES || size > apx::MAX_FILE_BYTES) return handleabi::ERR_BADIMAGE;
    u8 head[apx::HEADER_BYTES];
    lens::Status st = read_exact(t, file, head, sizeof(head));
    if (st != lens::Status::Ok) return fileabi::error_of(st);
    if (apx::parse_header(head, sizeof(head), size).status != apx::Status::Ok) return handleabi::ERR_BADIMAGE;
    out->buf = static_cast<u8*>(kheap::alloc(static_cast<usize>(size)));
    if (!out->buf) return userabi::ERR_LIMIT;
    st = read_exact(t, file, out->buf, size);
    if (st != lens::Status::Ok) return fileabi::error_of(st);
    apx::Parsed full = apx::parse(out->buf, static_cast<usize>(size));
    if (full.status != apx::Status::Ok) return handleabi::ERR_BADIMAGE;
    out->header = full.header;
    lens::Result<u32> label = lens::label_of(t.domain, file, out->label, sizeof(out->label));
    if (!label.ok() || out->label[0] == 0) {
        const char fallback[] = "apx";
        for (u32 i = 0; i < sizeof(fallback); ++i) out->label[i] = fallback[i];
    }
    return 0;
}

bool arg_handle_ok(lens::Table& t, lens::Lens arg, i64* error) {
    TableGuard guard(t.lock);
    lens::Result<lens::Rights> r = lens::rights_of(t.domain, arg);
    if (!r.ok()) {
        *error = handleabi::error_of(r.status());
        return false;
    }
    if (!r.value().has(lens::Right::Delegate)) {
        *error = handleabi::ERR_DENIED;
        return false;
    }
    return true;
}

Process* spawn_file(lens::Table& files, lens::Lens file, const char* name, lens::Table* from, lens::Lens arg, lens::Lens arg2, i64* error) {
    i64 scratch = 0;
    if (!error) error = &scratch;
    *error = 0;
    if (!supported()) {
        *error = userabi::ERR_NOSYS;
        return nullptr;
    }
    if (from && !arg.is_null() && !arg_handle_ok(*from, arg, error)) return nullptr;
    if (from && !arg2.is_null() && (arg.is_null() || !arg_handle_ok(*from, arg2, error))) {
        if (*error == 0) *error = userabi::ERR_INVAL;
        return nullptr;
    }
    Loaded loaded;
    i64 e = load_apx(files, file, &loaded);
    if (e) {
        *error = e;
        return nullptr;
    }
    const apx::Header& h = loaded.header;
    Image img{ loaded.buf + h.code_offset, h.code_bytes, h.entry_offset, h.data_pages, h.data_bytes ? loaded.buf + h.data_offset : nullptr, h.data_bytes };
    bool with_arg = from && !arg.is_null();
    Process* p = launch_impl(name ? name : loaded.label, img, with_arg ? from : nullptr, arg, arg2);
    if (!p) *error = userabi::ERR_LIMIT;
    return p;
}

}

using namespace impl;

Process* launch_file(const char* name, lens::Table& files, lens::Lens file, lens::Table* from, lens::Lens handle, i64* error, lens::Lens handle2) {
    return spawn_file(files, file, name, from, handle, handle2, error);
}

Process* launch_started(const char* name, lens::Table& table, lens::Lens file, const Startup& startup, i64* error) {
    i64 scratch = 0;
    if (!error) error = &scratch;
    *error = 0;
    lens::Lens handles[startup::MAX_HANDLES];
    u32 count = 0;
    u32 present = 0;
    const lens::Lens* wanted[startup::SLOT_COUNT] = { &startup.program, &startup.directory, &startup.input, &startup.output, &startup.work };
    for (u32 slot = 0; slot < startup::SLOT_COUNT; ++slot) {
        if (wanted[slot]->is_null()) continue;
        present |= 1u << slot;
        handles[count++] = *wanted[slot];
    }
    u8 message[startup::MAX_MESSAGE];
    u32 length = 0;
    i32 encoded = startup::encode(startup.argv, startup.argc, startup.env, startup.envc, present, message, sizeof(message), &length);
    if (encoded != startup::OK) {
        *error = (encoded == startup::ERR_TOO_MANY || encoded == startup::ERR_TOO_LARGE) ? handleabi::ERR_TOOBIG : userabi::ERR_INVAL;
        return nullptr;
    }
    lens::Lens mine, theirs;
    lens::Done made = lens::channel::create(table, &mine, &theirs);
    if (!made.ok()) {
        *error = handleabi::error_of(made.status());
        return nullptr;
    }
    lens::Done sent = lens::channel::send(table, mine, lens::ConstBytes(message, length), handles, count);
    if (!sent.ok()) {
        *error = handleabi::error_of(sent.status());
        TableGuard guard(table.lock);
        (void)lens::close(table.domain, mine);
        (void)lens::close(table.domain, theirs);
        return nullptr;
    }
    Process* p = spawn_file(table, file, name, &table, theirs, lens::Lens(), error);
    TableGuard guard(table.lock);
    (void)lens::close(table.domain, mine);
    if (!p) (void)lens::close(table.domain, theirs);
    return p;
}

}
