#include "internal.hpp"
#include <cosmos/handleabi.hpp>
#include <cosmos/serial.hpp>

namespace userland::impl {

namespace {

i64 resolve_memobj(Process* p, u64 raw, bool for_map, MemRef* out) {
    TableGuard guard(p->handles.lock);
    lens::Result<lens::Slot*> sr = lens::slot_of(p->handles.domain, lens::Lens(raw));
    if (!sr.ok()) return handleabi::error_of(sr.status());
    lens::Slot* s = sr.value();
    memobj::Object* o = s->kind == lens::NodeKind::Stream ? memobj::from_stream(s->stream) : nullptr;
    if (!o) return userabi::ERR_INVAL;
    if (for_map) {
        if (!s->rights.has(lens::Right::Read)) return handleabi::ERR_DENIED;
        out->max = vmregion::PROT_READ | (s->rights.has(lens::Right::Write) ? vmregion::PROT_WRITE : 0u) |
                   (s->rights.has(lens::Right::Execute) ? vmregion::PROT_EXEC : 0u);
    }
    o->retain();
    out->object = o;
    return 0;
}

bool vm_args_ok(u64 pages, u64 prot) { return pages != 0 && pages <= vmregion::MAX_MAPPED_PAGES && prot <= 0xFFFFFFFFull; }

}

i64 sys_vmo_create(idt::Frame* f) {
    Process* p = current_process();
    if (f->rsi != 0 || f->rdi == 0) return userabi::ERR_INVAL;
    if (f->rdi > memobj::MAX_PAGES) return handleabi::ERR_TOOBIG;
    u64 raw = 0;
    if (!user_write(p, f->rdx, &raw, sizeof(raw))) return userabi::ERR_FAULT;
    memobj::Object* o = memobj::create(static_cast<u32>(f->rdi));
    if (!o) return userabi::ERR_LIMIT;
    {
        TableGuard guard(p->handles.lock);
        lens::Result<lens::Lens> r = lens::alloc_slot(p->handles.domain, nullptr, nebula::Node{0, 0}, nebula::Node{0, 0},
                                                      memobj::HANDLE_RIGHTS, false, 0, "memobj", o);
        if (!r.ok()) {
            memobj::discard(o);
            return handleabi::error_of(r.status());
        }
        raw = r.value().raw;
        if (!user_write(p, f->rdx, &raw, sizeof(raw))) {
            (void)lens::close(p->handles.domain, r.value());
            return userabi::ERR_FAULT;
        }
    }
    return 0;
}

i64 sys_vmo_size(idt::Frame* f) {
    Process* p = current_process();
    MemRef ref;
    i64 e = resolve_memobj(p, f->rdi, false, &ref);
    if (e) return e;
    return static_cast<i64>(ref.object->pages);
}

i64 sys_vm_map(idt::Frame* f) {
    Process* p = current_process();
    u64 addr = f->rsi;
    u64 offset = f->rdx;
    u64 pages = f->r10;
    u64 prot = f->r8;
    if (!vm_args_ok(pages, prot) || offset > memobj::MAX_PAGES) return userabi::ERR_INVAL;
    MemRef ref;
    i64 e = resolve_memobj(p, f->rdi, true, &ref);
    if (e) return e;
    memobj::Object* o = ref.object;
    if (pages > o->pages || offset > o->pages - pages) return userabi::ERR_INVAL;
    vmregion::Status ps = vmregion::check_prot(static_cast<u32>(prot), ref.max);
    if (ps != vmregion::Status::Ok) return handleabi::vm_error(ps);

    VmGuard vg(p->vm);
    for (u64 i = 0; i < pages; ++i)
        if (memobj::poisoned(o, static_cast<u32>(offset + i))) return userabi::ERR_FAULT;
    u64 base = addr;
    vmregion::Status st = addr ? p->regions.check_free(addr, static_cast<u32>(pages)) : p->regions.find_gap(static_cast<u32>(pages), &base);
    if (st != vmregion::Status::Ok) return handleabi::vm_error(st);
    vmregion::Region r{ base, static_cast<u32>(pages), static_cast<u32>(offset), static_cast<u32>(prot), ref.max, o };
    st = p->regions.insert(r);
    if (st != vmregion::Status::Ok) return handleabi::vm_error(st);
    o->retain();

    u64 flags = vmregion::pte_flags(static_cast<u32>(prot));
    u64 mapped = 0;
    for (; mapped < pages; ++mapped) {
        if (!constellation::space_map(p->space, base + mapped * PAGE, memobj::frame(o, static_cast<u32>(offset + mapped)), PAGE, flags)) break;
    }
    if (mapped != pages) {
        if (mapped) constellation::space_unmap(p->space, base, mapped * PAGE);
        StreamOps ops;
        p->regions.unmap(base, static_cast<u32>(pages), ops);
        return userabi::ERR_LIMIT;
    }
    return static_cast<i64>(base);
}

i64 sys_vm_unmap(idt::Frame* f) {
    Process* p = current_process();
    if (f->rsi == 0 || f->rsi > vmregion::MAX_MAPPED_PAGES) return userabi::ERR_INVAL;
    u64 base = f->rdi;
    u32 pages = static_cast<u32>(f->rsi);
    DeferredOps ops;
    VmGuard vg(p->vm);
    vmregion::Status st = p->regions.unmap(base, pages, ops);
    if (st != vmregion::Status::Ok) return handleabi::vm_error(st);
    if (!constellation::space_unmap(p->space, base, static_cast<u64>(pages) * PAGE)) {
        serial::printf("[userland] pid %u: page tables disagree with the region table, memory kept alive\n", p->pid);
        return userabi::ERR_FAULT;
    }
    ops.flush();
    return 0;
}

i64 sys_vm_protect(idt::Frame* f) {
    Process* p = current_process();
    if (!vm_args_ok(f->rsi, f->rdx)) return userabi::ERR_INVAL;
    u64 base = f->rdi;
    u32 pages = static_cast<u32>(f->rsi);
    u32 prot = static_cast<u32>(f->rdx);
    StreamOps ops;
    VmGuard vg(p->vm);
    vmregion::Status st = p->regions.protect(base, pages, prot, ops);
    if (st != vmregion::Status::Ok) return handleabi::vm_error(st);
    if (!constellation::space_protect(p->space, base, static_cast<u64>(pages) * PAGE, vmregion::pte_flags(prot))) {
        serial::printf("[userland] pid %u: page tables disagree with the region table after protect\n", p->pid);
        return userabi::ERR_FAULT;
    }
    return 0;
}

}

static_assert(vmregion::PTE_WRITABLE == constellation::WRITABLE && vmregion::PTE_NO_EXECUTE == constellation::NO_EXECUTE,
              "the region table's page flags must match the VMM");

static_assert(userabi::USER_LIMIT == constellation::USER_LIMIT, "the native ABI and the VMM must agree on the user limit");
