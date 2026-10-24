#pragma once
#include <cosmos/nebula.hpp>
#include <cosmos/stellar.hpp>

namespace nebula {

constexpr u64 RAM_LIMIT_BYTES = 48ull << 20;
constexpr u32 RAM_MAX_ITEMS = 2048;

Realm* ram_realm();
Realm* sys_realm();
Realm* disk_realm();
Realm* snap_realm();

void disk_set_mounted(bool mounted);

u32 disk_generation();
bool disk_is_mounted();
void disk_set_confirmed_only(bool confirmed_only);
bool disk_is_confirmed_only();
Status disk_snapshot(u64* id);
Status disk_snapshot_delete(u64 id);
Status status_from_stellar(stellar::Status s);

bool register_builtin_realms();
bool selftest();

void sys_set_facts(const char* storage, u32 suns);

}
