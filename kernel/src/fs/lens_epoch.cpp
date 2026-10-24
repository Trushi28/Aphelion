#include <cosmos/lens.hpp>
#include <cosmos/nebula_realms.hpp>

namespace lens {

namespace {

constexpr Rights VIEW_RIGHTS = Right::Read | Right::Enumerate;

u32 epoch_index(const Domain& d, Epoch e) {
    if (!e.valid()) return MAX_EPOCHS;
    for (u32 i = 0; i < MAX_EPOCHS; ++i)
        if (d.epochs[i] == e.raw()) return i;
    return MAX_EPOCHS;
}

bool is_view(const Slot* s) { return s->realm == nebula::snap_realm(); }

}

Result<Epoch> freeze(Domain& d, Lens dir) {
    Result<Slot*> sr = slot_of(d, dir);
    if (!sr.ok()) return Result<Epoch>::fail(sr.status());
    Slot* s = sr.value();
    if (s->kind != NodeKind::Folder) return Result<Epoch>::fail(Status::NotADirectory);
    if (!s->rights.has(Right::Freeze)) return Result<Epoch>::fail(Status::Denied);
    if (s->realm != nebula::disk_realm()) return Result<Epoch>::fail(Status::Unsupported);
    if (!s->realm->available()) return Result<Epoch>::fail(Status::Unavailable);

    u32 slot = MAX_EPOCHS;
    for (u32 i = 0; i < MAX_EPOCHS; ++i)
        if (d.epochs[i] == 0) { slot = i; break; }
    if (slot == MAX_EPOCHS) return Result<Epoch>::fail(Status::TooManyEpochs);

    stellar::Status why = stellar::Status::Ok;
    u64 id = stellar::snapshot_subtree(s->node.id, &why);
    if (id == stellar::INVALID_STAR) {
        if (why == stellar::Status::TooManySnapshots) return Result<Epoch>::fail(Status::TooManyEpochs);
        return Result<Epoch>::fail(from_nebula(nebula::status_from_stellar(why)));
    }
    d.epochs[slot] = id;
    return Result<Epoch>::success(Epoch(id));
}

Result<Lens> at(Domain& d, Lens l, Epoch e) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return Result<Lens>::fail(sr.status());
    Slot* s = sr.value();
    if (epoch_index(d, e) == MAX_EPOCHS) return Result<Lens>::fail(Status::NoSuchEpoch);
    if (!s->rights.has_any(VIEW_RIGHTS)) return Result<Lens>::fail(Status::Denied);
    if (s->realm != nebula::disk_realm()) return Result<Lens>::fail(Status::Unsupported);
    if (!s->realm->available()) return Result<Lens>::fail(Status::Unavailable);

    stellar::Status why = stellar::Status::Ok;
    u64 scope = stellar::snapshot_scope(e.raw(), &why);
    if (scope == stellar::INVALID_STAR) return Result<Lens>::fail(from_nebula(nebula::status_from_stellar(why)));
    if (s->node.id == stellar::ROOT_STAR && scope != stellar::ROOT_STAR) return Result<Lens>::fail(Status::NotFound);

    nebula::Node past{s->node.id, e.raw()};
    char label[nebula::NAME_BYTES + 1];
    u32 n = 0;
    for (; s->label[n] && n < nebula::NAME_BYTES; ++n) label[n] = s->label[n];
    label[n] = 0;
    Rights rights = s->rights & VIEW_RIGHTS;

    return alloc_slot(d, nebula::snap_realm(), past, past, rights, true, 0, label);
}

Result<u32> restore(Domain& d, Lens past, Lens live_dir, const char* live_rel) {
    Result<Slot*> pr = slot_of(d, past);
    if (!pr.ok()) return Result<u32>::fail(pr.status());
    if (!is_view(pr.value())) return Result<u32>::fail(Status::Unsupported);
    if (pr.value()->kind == NodeKind::Folder) {
        u32 copied = 0;
        Done r = copy_tree(d, past, live_dir, live_rel, &copied);
        if (!r.ok()) return Result<u32>::fail(r.status());
        return Result<u32>::success(copied);
    }
    Done r = copy(d, past, live_dir, live_rel);
    if (!r.ok()) return Result<u32>::fail(r.status());
    return Result<u32>::success(1);
}

Done release_epoch(Domain& d, Epoch e) {
    u32 i = epoch_index(d, e);
    if (i == MAX_EPOCHS) return Done::fail(Status::NoSuchEpoch);
    stellar::Status why = stellar::Status::Ok;
    if (!stellar::delete_snapshot(e.raw(), &why)) {
        if (why == stellar::Status::NoSuchSnapshot) d.epochs[i] = 0;
        return Done::fail(from_nebula(nebula::status_from_stellar(why)));
    }
    d.epochs[i] = 0;
    return Done::success();
}

Result<Epoch> epoch_of(Domain& d, Lens l) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return Result<Epoch>::fail(sr.status());
    Slot* s = sr.value();
    return Result<Epoch>::success(is_view(s) ? Epoch(s->node.scope) : Epoch::none());
}

u32 epoch_count(const Domain& d) {
    u32 n = 0;
    for (u32 i = 0; i < MAX_EPOCHS; ++i)
        if (d.epochs[i]) ++n;
    return n;
}

}
