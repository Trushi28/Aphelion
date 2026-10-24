#include <cosmos/lens.hpp>
#include <cosmos/nebula_realms.hpp>

namespace lens {

namespace {

bool joins_batch(nebula::Realm* r) { return r != nebula::disk_realm() && r->transactional(); }

void begin_realms(bool* began) {
    for (u32 i = 0; i < nebula::realm_count() && i < nebula::MAX_REALMS; ++i) {
        nebula::Realm* r = nebula::realm_at(i);
        began[i] = r && joins_batch(r) && r->txn_begin();
    }
}

void end_realms(const bool* began, bool keep) {
    for (u32 i = 0; i < nebula::realm_count() && i < nebula::MAX_REALMS; ++i) {
        if (!began[i]) continue;
        nebula::Realm* r = nebula::realm_at(i);
        if (keep) r->txn_commit();
        else r->txn_abort();
    }
}

}

Batch::Batch() : open_(true), failed_(false), first_(Status::Ok), began_{} {
    stellar::begin_batch();
    begin_realms(began_);
}

Batch::~Batch() {
    if (open_) abort();
}

void Batch::fail(Status why) {
    if (!failed_) {
        failed_ = true;
        first_ = why == Status::Ok ? Status::BatchFailed : why;
    }
}

Done Batch::note(Done r) {
    if (!r.ok()) fail(r.status());
    return r;
}

void Batch::abort() {
    if (!open_) return;
    open_ = false;
    end_realms(began_, false);
    stellar::abort_batch();
}

Done Batch::commit() {
    if (!open_) return Done::fail(Status::BatchFailed);
    if (failed_) {
        abort();
        return Done::fail(first_);
    }
    open_ = false;
    stellar::Status why = stellar::Status::Ok;
    if (stellar::end_batch(&why)) {
        end_realms(began_, true);
        return Done::success();
    }
    end_realms(began_, false);
    Status st = from_nebula(nebula::status_from_stellar(why));
    return Done::fail(st == Status::Ok ? Status::Io : st);
}

bool Batch::covers(Domain& d, Lens l) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return false;
    nebula::Realm* r = sr.value()->realm;
    return r == nebula::disk_realm() || r->transactional() || !r->writable();
}

}
