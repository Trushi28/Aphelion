#pragma once
#include <cosmos/lens_types.hpp>
#include <cosmos/nebula.hpp>

namespace lens {

constexpr u32 MAX_LENSES = 128;
constexpr u32 MAX_DEPTH = nebula::path::MAX_COMPONENTS;

enum class NodeKind : u8 {
    Blob,
    Folder,
    Probe,
    Stream,
};

const char* kind_name(NodeKind k);

struct Lens {
    u64 raw;

    constexpr Lens() : raw(0) {}
    constexpr explicit Lens(u64 r) : raw(r) {}
    constexpr bool is_null() const { return raw == 0; }
    constexpr bool operator==(const Lens&) const = default;
};

class Stream;

struct Slot {
    bool used;
    bool read_only;
    NodeKind kind;
    u8 depth;
    bool retired;
    u32 generation;
    Rights rights;
    nebula::Realm* realm;
    nebula::Node node;
    nebula::Node root;
    nebula::OpenFile buffer;
    Stream* stream;
    char label[nebula::NAME_BYTES + 1];
};

constexpr u32 MAX_EPOCHS = 32;

struct Domain {
    Slot slots[MAX_LENSES];
    u32 live;
    u64 epochs[MAX_EPOCHS];
};

Result<Lens> mint(Domain& d, const char* text, Rights rights, bool read_only = false);
void seal();
bool sealed();
void test_unseal();

Result<Lens> open_at(Domain& d, Lens dir, const char* rel_path, Rights rights);

Result<Lens> narrow(Domain& d, Lens src, Rights keep, bool make_read_only = false);

Result<Lens> delegate(Domain& from, Lens src, Domain& to, Rights rights);

Done close(Domain& d, Lens l);
void close_all(Domain& d);

Result<Rights> rights_of(Domain& d, Lens l);
Result<NodeKind> kind_of(Domain& d, Lens l);
Result<bool> read_only_of(Domain& d, Lens l);
Result<u32> depth_of(Domain& d, Lens l);
Result<nebula::Info> stat(Domain& d, Lens l);

Result<u32> label_of(Domain& d, Lens l, char* out, u32 cap);
bool is_live(Domain& d, Lens l);
u32 live_count(const Domain& d);

Result<usize> read(Domain& d, Lens l, u64 offset, Bytes dst);
Result<usize> write(Domain& d, Lens l, u64 offset, ConstBytes src);
Done append(Domain& d, Lens l, ConstBytes src);
Done resize(Domain& d, Lens l, u64 new_size);
Done sync(Domain& d, Lens l);
Result<u64> size(Domain& d, Lens l);

using VisitFn = bool (*)(void* ctx, const nebula::Entry& entry);
struct EnumSummary {
    u32 visited;
    bool truncated;
};
Result<EnumSummary> enumerate(Domain& d, Lens dir, VisitFn fn, void* ctx, bool with_info = false);

Done collect(Domain& d, Lens dir, nebula::Listing* out, bool with_info = false, bool sorted = true);

Result<nebula::Blob> slurp(Domain& d, Lens l);

Result<Lens> create(Domain& d, Lens dir, const char* rel, Rights rights);
Done mkdir(Domain& d, Lens dir, const char* rel, bool parents = false);
Done unlink(Domain& d, Lens dir, const char* rel);
Done remove_tree(Domain& d, Lens dir, const char* rel, u32* removed = nullptr);
Done rename(Domain& d, Lens from_dir, const char* from, Lens to_dir, const char* to, bool replace = false);
Done copy(Domain& d, Lens src, Lens dst_dir, const char* dst_rel);
Done copy_tree(Domain& d, Lens src, Lens dst_dir, const char* dst_rel, u32* copied = nullptr);

Done put(Domain& d, Lens dir, const char* rel, ConstBytes data);

Result<Epoch> freeze(Domain& d, Lens dir);
Result<Lens> at(Domain& d, Lens l, Epoch e);

Result<u32> restore(Domain& d, Lens past, Lens live_dir, const char* live_rel);
Done release_epoch(Domain& d, Epoch e);
Result<Epoch> epoch_of(Domain& d, Lens l);
u32 epoch_count(const Domain& d);

class Stream {
public:
    virtual const char* name() const = 0;
    virtual Result<usize> read(Bytes dst) = 0;
    virtual Result<usize> write(ConstBytes src) = 0;
    virtual Readiness ready() = 0;

    virtual void retain() = 0;
    virtual void release() = 0;
};

void stream_event();

Result<Lens> mint_stream(Domain& d, Stream* stream, Rights rights);

Done make_pipe(Domain& d, usize capacity, Lens* read_end, Lens* write_end);

struct Wait {
    Lens lens;
    Readiness want;
    Readiness got;
};

Result<u32> await(Domain& d, Wait* waits, u32 count, u64 timeout_ms);

class Batch {
public:
    Batch();
    ~Batch();
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    Done note(Done r);
    template <typename T>
    Result<T> note(Result<T> r) {
        if (!r.ok()) fail(r.status());
        return r;
    }
    void fail(Status why);
    bool failed() const { return failed_; }
    Status first_error() const { return first_; }

    Done commit();
    void abort();
    bool open() const { return open_; }

    static bool covers(Domain& d, Lens l);

private:
    bool open_;
    bool failed_;
    Status first_;
    bool began_[nebula::MAX_REALMS];
};

Status from_nebula(nebula::Status s);

Result<Slot*> slot_of(Domain& d, Lens l);

struct Comp {
    const char* p;
    u32 n;
};
struct RelPath {
    Comp comps[nebula::path::MAX_COMPONENTS];
    u32 count;
};

Status parse_relative(const char* text, RelPath* out);

Status walk(nebula::Realm* realm, nebula::Node from, const RelPath& rp, u32 count, nebula::Node* out);

Result<Lens> alloc_slot(Domain& d, nebula::Realm* realm, nebula::Node node, nebula::Node root, Rights rights,
                        bool read_only, u32 depth, const char* label, Stream* stream = nullptr);
NodeKind kind_from_info(const nebula::Info& inf);

}
