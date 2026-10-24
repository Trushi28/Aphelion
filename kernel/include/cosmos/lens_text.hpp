#pragma once
#include <cosmos/lens.hpp>

namespace lens {

Domain& boot_domain();

Done mint_boot_roots();

Lens boot_root(const char* realm);
Done mint_boot_console(Stream* console);
Lens boot_console();

bool selftest();

namespace text {

struct Cursor {
    Domain domain;
    Lens roots[nebula::MAX_REALMS];
    Lens console;
    nebula::Realm* realms[nebula::MAX_REALMS];
    u32 root_count;
    Lens stack[MAX_DEPTH + 1];
    u32 depth;
    nebula::path::Path here;
    u32 volume_generation;
    bool ready;
};

struct StatRow {
    nebula::Info info;
    nebula::path::Path path;
    const char* realm;
    bool writable;
};

Done init(Cursor& c, const char* start_realm);
Lens console(const Cursor& c);
void close(Cursor& c);

nebula::Status canonical(Cursor& c, const char* text, nebula::path::Path* out);
nebula::Status change_location(Cursor& c, const char* text);
u32 location_text(const Cursor& c, char* out, u32 cap);

nebula::Status stat(Cursor& c, const char* text, nebula::Info* out);
nebula::Status stat_row(Cursor& c, const char* text, StatRow* out);
nebula::Status list(Cursor& c, const char* text, nebula::Listing* out, bool with_info);

nebula::Status read_file(Cursor& c, const char* text, nebula::Blob* out);
nebula::Status write_file(Cursor& c, const char* text, const void* data, u64 size);
nebula::Status append_file(Cursor& c, const char* text, const void* data, u64 size);

nebula::Status make_dir(Cursor& c, const char* text);
nebula::Status make_path(Cursor& c, const char* text);
nebula::Status remove(Cursor& c, const char* text);
nebula::Status remove_tree(Cursor& c, const char* text, u32* removed);
nebula::Status rename(Cursor& c, const char* from, const char* to, u32 flags);
nebula::Status copy_file(Cursor& c, const char* from, const char* to);
nebula::Status copy_tree(Cursor& c, const char* from, const char* to, u32* copied);

Result<Lens> grant(Cursor& c, const char* text, Rights rights, bool read_only = false);

}
}
