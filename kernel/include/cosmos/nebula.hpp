#pragma once
#include <cosmos/types.hpp>
#include <cosmos/nebula_path.hpp>

namespace nebula {

constexpr u32 NAME_BYTES = path::NAME_MAX;
constexpr u32 MAX_REALMS = 8;
constexpr u32 MAX_HANDLES = 16;
constexpr u64 MAX_FILE_BYTES = 16ull << 20;
constexpr u32 MAX_LIST_ENTRIES = 65536;
constexpr u32 MAX_TREE_DEPTH = 16;
constexpr u64 DYNAMIC_SLACK = 4096;

enum class Status : u8 {
    Ok,
    NotFound,
    Exists,
    NotADirectory,
    IsADirectory,
    NotEmpty,
    NoSpace,
    NoMemory,
    Io,
    InvalidPath,
    InvalidName,
    ReadOnly,
    Busy,
    TooLarge,
    NoRealm,
    Unavailable,
    BadHandle,
    CrossRealm,
    Unsupported,
    Corrupt,
    Denied,
};

const char* status_text(Status s);

enum class Kind : u8 { File, Dir };

struct Node {
    u64 id;
    u64 scope;
};

struct Info {
    Kind kind;
    u64 size;
    u64 mtime_ms;
    u32 links;
    bool read_only;
    bool dynamic;
};

using ChildFn = void (*)(const char* name, Kind kind, Node child, void* ctx);

class Realm {
public:
    virtual const char* name() const = 0;
    virtual const char* summary() const = 0;
    virtual bool writable() const = 0;
    virtual bool available() const = 0;
    virtual Node root() = 0;
    virtual Status find(Node dir, const char* name, Node* out) = 0;
    virtual Status info(Node node, Info* out) = 0;
    virtual Status list(Node dir, ChildFn fn, void* ctx) = 0;
    virtual Status read_all(Node file, void* buf, u64 capacity, u64* size) = 0;
    virtual Status write_all(Node file, const void* data, u64 size) = 0;
    virtual Status create_file(Node dir, const char* name, Node* out) = 0;
    virtual Status make_dir(Node dir, const char* name, Node* out) = 0;
    virtual Status remove(Node dir, const char* name) = 0;
    virtual Status rename(Node from_dir, const char* from, Node to_dir, const char* to, bool replace) = 0;
    virtual u64 space_free() { return 0; }
    virtual u64 space_total() { return 0; }
};

bool register_realm(Realm* realm);
Realm* find_realm(const char* name);
u32 realm_count();
Realm* realm_at(u32 index);

constexpr u32 OPEN_READ = 1u << 0;
constexpr u32 OPEN_WRITE = 1u << 1;
constexpr u32 OPEN_CREATE = 1u << 2;
constexpr u32 OPEN_TRUNCATE = 1u << 3;
constexpr u32 OPEN_EXCLUSIVE = 1u << 4;

using Handle = u32;
constexpr Handle NO_HANDLE = 0;

struct OpenFile {
    bool used;
    u16 generation;
    Realm* realm;
    Node node;
    u32 flags;
    u8* data;
    u64 size;
    u64 capacity;
    bool dirty;
};

struct Session {
    path::Path here;
    OpenFile files[MAX_HANDLES];
    bool ready;
};

struct Entry {
    char name[NAME_BYTES + 1];
    Kind kind;
    u64 size;
    u64 mtime_ms;
};

struct Listing {
    Entry* items;
    u32 count;
    u32 capacity;
    bool truncated;
};

struct Resolved {
    Realm* realm;
    path::Path path;
    Node node;
    Node parent;
    bool exists;
    bool is_root;
    char leaf[NAME_BYTES + 1];
};

void init_session(Session& s, const char* realm);
void close_session(Session& s);

Status resolve(Session& s, const char* text, Resolved* out);
Status change_location(Session& s, const char* text);
u32 location_text(const Session& s, char* out, u32 cap);

Status stat(Session& s, const char* text, Info* out);
Status list(Session& s, const char* text, Listing* out, bool with_info);
void release(Listing& l);

Status open(Session& s, const char* text, u32 flags, Handle* out);
Status read_at(Session& s, Handle h, u64 offset, void* buf, u64 length, u64* got);
Status write_at(Session& s, Handle h, u64 offset, const void* data, u64 length);
Status append(Session& s, Handle h, const void* data, u64 length);
Status size_of(Session& s, Handle h, u64* out);
Status resize(Session& s, Handle h, u64 new_size);
Status sync(Session& s, Handle h);
Status close(Session& s, Handle h);

struct Blob {
    u8* data;
    u64 size;
};
Status read_file(Session& s, const char* text, Blob* out);
void release(Blob& b);
Status write_file(Session& s, const char* text, const void* data, u64 size);
Status append_file(Session& s, const char* text, const void* data, u64 size);

Status make_dir(Session& s, const char* text);
Status make_path(Session& s, const char* text);
Status remove(Session& s, const char* text);
Status remove_tree(Session& s, const char* text, u32* removed);
constexpr u32 RENAME_REPLACE = 1u << 0;
Status rename(Session& s, const char* from, const char* to, u32 flags);
Status copy_file(Session& s, const char* from, const char* to);
Status copy_tree(Session& s, const char* from, const char* to, u32* copied);

}
