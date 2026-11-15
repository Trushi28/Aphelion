#include "internal.hpp"
#include <cosmos/handleabi.hpp>
#include <cosmos/fileabi.hpp>
#include <cosmos/lens_channel.hpp>

namespace userland::impl {

namespace {

i64 fetch_path(Process* p, u64 uva, u64 len, char (&dst)[fileabi::MAX_PATH + 1]) {
    if (!fileabi::path_len_ok(len)) return userabi::ERR_INVAL;
    if (!user_read(p, uva, dst, len)) return userabi::ERR_FAULT;
    dst[len] = 0;
    if (!fileabi::path_text_ok(dst, len)) return userabi::ERR_INVAL;
    return 0;
}

struct DirPick {
    u64 index;
    u64 seen;
    bool found;
    fileabi::DirEntry entry;
};

bool dir_pick(void* ctx, const nebula::Entry& e) {
    DirPick* d = static_cast<DirPick*>(ctx);
    if (d->seen != d->index) {
        ++d->seen;
        return true;
    }
    d->found = true;
    d->entry.size = e.size;
    d->entry.mtime_ms = e.mtime_ms;
    d->entry.kind = e.kind == nebula::Kind::Dir ? fileabi::KIND_DIRECTORY : fileabi::KIND_FILE;
    u32 n = 0;
    while (e.name[n] && n + 1 < fileabi::NAME_CAP) {
        d->entry.name[n] = e.name[n];
        ++n;
    }
    d->entry.name[n] = 0;
    d->entry.name_len = n;
    return false;
}

static_assert(nebula::NAME_BYTES + 1 <= fileabi::NAME_CAP, "a directory entry name must fit the user record");

}

i64 sys_file_open(idt::Frame* f) {
    Process* p = current_process();
    if (!fileabi::rights_ok(f->r10)) return userabi::ERR_INVAL;
    char path[fileabi::MAX_PATH + 1];
    i64 e = fetch_path(p, f->rsi, f->rdx, path);
    if (e) return e;
    u64 raw = 0;
    if (!user_write(p, f->r8, &raw, sizeof(raw))) return userabi::ERR_FAULT;
    TableGuard guard(p->handles.lock);
    lens::Result<lens::Lens> r = lens::open_at(p->handles.domain, lens::Lens(f->rdi), path, lens::Rights::from_bits(static_cast<u32>(f->r10)));
    if (!r.ok()) return fileabi::error_of(r.status());
    raw = r.value().raw;
    if (!user_write(p, f->r8, &raw, sizeof(raw))) {
        (void)lens::close(p->handles.domain, r.value());
        return userabi::ERR_FAULT;
    }
    return 0;
}

i64 sys_file_read(idt::Frame* f) {
    Process* p = current_process();
    u64 n = fileabi::clamp_io(f->r10);
    if (n == 0) return 0;
    u8 bounce[fileabi::MAX_IO] = {};
    if (!user_write(p, f->rdx, bounce, n)) return userabi::ERR_FAULT;
    usize got = 0;
    {
        TableGuard guard(p->handles.lock);
        lens::Result<usize> r = lens::read(p->handles.domain, lens::Lens(f->rdi), f->rsi, lens::Bytes(bounce, n));
        if (!r.ok()) return fileabi::error_of(r.status());
        got = r.value();
    }
    if (got > n) return userabi::ERR_INVAL;
    if (got && !user_write(p, f->rdx, bounce, got)) return userabi::ERR_FAULT;
    return static_cast<i64>(got);
}

i64 sys_file_write(idt::Frame* f) {
    Process* p = current_process();
    u64 n = fileabi::clamp_io(f->r10);
    if (n == 0) return 0;
    u8 bounce[fileabi::MAX_IO];
    if (!user_read(p, f->rdx, bounce, n)) return userabi::ERR_FAULT;
    TableGuard guard(p->handles.lock);
    lens::Result<usize> r = lens::write(p->handles.domain, lens::Lens(f->rdi), f->rsi, lens::ConstBytes(bounce, n));
    if (!r.ok()) return fileabi::error_of(r.status());
    if (r.value() > n) return userabi::ERR_INVAL;
    return static_cast<i64>(r.value());
}

i64 sys_file_stat(idt::Frame* f) {
    Process* p = current_process();
    fileabi::Stat st{};
    if (!user_write(p, f->rsi, &st, sizeof(st))) return userabi::ERR_FAULT;
    {
        TableGuard guard(p->handles.lock);
        lens::Lens l(f->rdi);
        lens::Result<lens::NodeKind> k = lens::kind_of(p->handles.domain, l);
        if (!k.ok()) return fileabi::error_of(k.status());
        if (k.value() == lens::NodeKind::Stream) {
            st.kind = fileabi::KIND_STREAM;
        } else {
            lens::Result<nebula::Info> info = lens::stat(p->handles.domain, l);
            if (!info.ok()) return fileabi::error_of(info.status());
            st.kind = info.value().kind == nebula::Kind::Dir ? fileabi::KIND_DIRECTORY : fileabi::KIND_FILE;
            st.size = info.value().size;
            if (info.value().kind == nebula::Kind::File) {
                lens::Result<u64> live = lens::size(p->handles.domain, l);
                if (live.ok()) st.size = live.value();
            }
            st.mtime_ms = info.value().mtime_ms;
            st.links = info.value().links;
            st.flags = fileabi::stat_flags(info.value().read_only, info.value().dynamic);
        }
    }
    if (!user_write(p, f->rsi, &st, sizeof(st))) return userabi::ERR_FAULT;
    return 0;
}

i64 sys_file_create(idt::Frame* f) {
    Process* p = current_process();
    u64 flags = f->r8;
    if ((flags & ~static_cast<u64>(fileabi::CREATE_FLAGS_MASK)) != 0 || !fileabi::rights_ok(f->r10)) return userabi::ERR_INVAL;
    char path[fileabi::MAX_PATH + 1];
    i64 e = fetch_path(p, f->rsi, f->rdx, path);
    if (e) return e;
    u64 raw = 0;
    bool directory = (flags & fileabi::CREATE_DIRECTORY) != 0;
    if (!directory && !user_write(p, f->r9, &raw, sizeof(raw))) return userabi::ERR_FAULT;
    TableGuard guard(p->handles.lock);
    if (directory) {
        lens::Done d = lens::mkdir(p->handles.domain, lens::Lens(f->rdi), path, false);
        return d.ok() ? 0 : fileabi::error_of(d.status());
    }
    lens::Result<lens::Lens> r = lens::create(p->handles.domain, lens::Lens(f->rdi), path, lens::Rights::from_bits(static_cast<u32>(f->r10)));
    if (!r.ok()) return fileabi::error_of(r.status());
    raw = r.value().raw;
    if (!user_write(p, f->r9, &raw, sizeof(raw))) {
        (void)lens::close(p->handles.domain, r.value());
        return userabi::ERR_FAULT;
    }
    return 0;
}

i64 sys_file_unlink(idt::Frame* f) {
    Process* p = current_process();
    u64 flags = f->r10;
    if ((flags & ~static_cast<u64>(fileabi::UNLINK_FLAGS_MASK)) != 0) return userabi::ERR_INVAL;
    char path[fileabi::MAX_PATH + 1];
    i64 e = fetch_path(p, f->rsi, f->rdx, path);
    if (e) return e;
    bool want_directory = (flags & fileabi::UNLINK_DIRECTORY) != 0;
    TableGuard guard(p->handles.lock);
    lens::Lens dir(f->rdi);
    lens::Result<lens::Lens> probe = lens::open_at(p->handles.domain, dir, path, lens::Right::Read);
    if (!probe.ok()) return fileabi::error_of(probe.status());
    lens::Result<lens::NodeKind> kind = lens::kind_of(p->handles.domain, probe.value());
    (void)lens::close(p->handles.domain, probe.value());
    if (!kind.ok()) return fileabi::error_of(kind.status());
    bool is_directory = kind.value() == lens::NodeKind::Folder;
    if (is_directory != want_directory) return fileabi::ERR_WRONGTYPE;
    lens::Done d = lens::unlink(p->handles.domain, dir, path);
    return d.ok() ? 0 : fileabi::error_of(d.status());
}

i64 sys_file_rename(idt::Frame* f) {
    Process* p = current_process();
    u64 flags = f->r9;
    if ((flags & ~static_cast<u64>(fileabi::RENAME_FLAGS_MASK)) != 0) return userabi::ERR_INVAL;
    char from[fileabi::MAX_PATH + 1];
    char to[fileabi::MAX_PATH + 1];
    i64 e = fetch_path(p, f->rsi, f->rdx, from);
    if (e) return e;
    e = fetch_path(p, f->r10, f->r8, to);
    if (e) return e;
    TableGuard guard(p->handles.lock);
    lens::Lens dir(f->rdi);
    lens::Done d = lens::rename(p->handles.domain, dir, from, dir, to, (flags & fileabi::RENAME_REPLACE) != 0);
    return d.ok() ? 0 : fileabi::error_of(d.status());
}

i64 sys_file_resize(idt::Frame* f) {
    Process* p = current_process();
    TableGuard guard(p->handles.lock);
    lens::Done d = lens::resize(p->handles.domain, lens::Lens(f->rdi), f->rsi);
    return d.ok() ? 0 : fileabi::error_of(d.status());
}

i64 sys_dir_entry(idt::Frame* f) {
    Process* p = current_process();
    DirPick pick{};
    pick.index = f->rsi;
    if (!user_write(p, f->rdx, &pick.entry, sizeof(pick.entry))) return userabi::ERR_FAULT;
    {
        TableGuard guard(p->handles.lock);
        lens::Result<lens::EnumSummary> r = lens::enumerate(p->handles.domain, lens::Lens(f->rdi), &dir_pick, &pick, true);
        if (!r.ok()) return fileabi::error_of(r.status());
    }
    if (!pick.found) return fileabi::DIR_END;
    if (!user_write(p, f->rdx, &pick.entry, sizeof(pick.entry))) return userabi::ERR_FAULT;
    return 0;
}

i64 sys_pipe_create(idt::Frame* f) {
    Process* p = current_process();
    if (f->rdi == 0 || f->rdi > fileabi::MAX_PIPE_BYTES) return userabi::ERR_INVAL;
    u64 pair[2] = { 0, 0 };
    if (!user_write(p, f->rsi, pair, sizeof(pair))) return userabi::ERR_FAULT;
    TableGuard guard(p->handles.lock);
    lens::Lens r, w;
    lens::Done d = lens::make_pipe(p->handles.domain, static_cast<usize>(f->rdi), &r, &w);
    if (!d.ok()) return fileabi::error_of(d.status());
    pair[0] = r.raw;
    pair[1] = w.raw;
    if (!user_write(p, f->rsi, pair, sizeof(pair))) {
        (void)lens::close(p->handles.domain, r);
        (void)lens::close(p->handles.domain, w);
        return userabi::ERR_FAULT;
    }
    return 0;
}

i64 sys_process_create(idt::Frame* f) {
    Process* self = current_process();
    if (f->r10 != 0) return userabi::ERR_INVAL;
    u64 raw = 0;
    if (!user_write(self, f->rdx, &raw, sizeof(raw))) return userabi::ERR_FAULT;
    (void)sweep_detached();
    ProcessObj* obj = static_cast<ProcessObj*>(kheap::alloc_zeroed(sizeof(ProcessObj)));
    if (!obj) return userabi::ERR_LIMIT;
    new (obj) ProcessObj();
    i64 error = 0;
    Process* child = spawn_file(self->handles, lens::Lens(f->rdi), nullptr, &self->handles, lens::Lens(f->rsi), lens::Lens(), &error);
    if (!child) {
        kheap::free(obj);
        return error;
    }
    obj->proc = child;
    obj->refs = 0;
    TableGuard guard(self->handles.lock);
    lens::Result<lens::Lens> made = lens::alloc_slot(self->handles.domain, nullptr, nebula::Node{0, 0}, nebula::Node{0, 0},
                                                     fileabi::PROCESS_RIGHTS, false, 0, "process", obj);
    if (!made.ok()) {
        (void)kill(child);
        __atomic_store_n(&child->detached, 1u, __ATOMIC_RELEASE);
        kheap::free(obj);
        return handleabi::error_of(made.status());
    }
    raw = made.value().raw;
    if (!user_write(self, f->rdx, &raw, sizeof(raw))) {
        (void)lens::close(self->handles.domain, made.value());
        return userabi::ERR_FAULT;
    }
    return 0;
}

i64 sys_process_status(idt::Frame* f) {
    Process* p = current_process();
    i64 status = 0;
    if (!user_write(p, f->rsi, &status, sizeof(status))) return userabi::ERR_FAULT;
    {
        TableGuard guard(p->handles.lock);
        lens::Result<lens::Slot*> sr = lens::slot_of(p->handles.domain, lens::Lens(f->rdi));
        if (!sr.ok()) return handleabi::error_of(sr.status());
        ProcessObj* o = process_of(sr.value());
        if (!o) return userabi::ERR_INVAL;
        if (!sr.value()->rights.has(lens::Right::Read)) return handleabi::ERR_DENIED;
        if (!__atomic_load_n(&o->proc->exited, __ATOMIC_ACQUIRE)) return handleabi::ERR_AGAIN;
        status = o->proc->status;
    }
    if (!user_write(p, f->rsi, &status, sizeof(status))) return userabi::ERR_FAULT;
    return 0;
}

i64 sys_process_kill(idt::Frame* f) {
    Process* p = current_process();
    TableGuard guard(p->handles.lock);
    lens::Result<lens::Slot*> sr = lens::slot_of(p->handles.domain, lens::Lens(f->rdi));
    if (!sr.ok()) return handleabi::error_of(sr.status());
    ProcessObj* o = process_of(sr.value());
    if (!o) return userabi::ERR_INVAL;
    if (!sr.value()->rights.has(lens::Right::Write)) return handleabi::ERR_DENIED;
    return kill(o->proc) ? 0 : handleabi::ERR_CLOSED;
}

}
