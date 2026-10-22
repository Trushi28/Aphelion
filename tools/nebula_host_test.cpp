#include <cosmos/types.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/stellar.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/nebula.hpp>
#include <cosmos/nebula_realms.hpp>

extern "C" {
int printf(const char*, ...);
int vprintf(const char*, __builtin_va_list);
int puts(const char*);
void* malloc(unsigned long);
void* aligned_alloc(unsigned long, unsigned long);
void* calloc(unsigned long, unsigned long);
void free(void*);
int sched_yield(void);
}

static u8* g_disk;
static u64 g_sectors;

namespace blockdev {
bool read_sector(u64 s, void* b) { if (s >= g_sectors) return false; __builtin_memcpy(b, g_disk + s * 512, 512); return true; }
bool write_sector(u64 s, const void* b) { if (s >= g_sectors) return false; __builtin_memcpy(g_disk + s * 512, b, 512); return true; }
bool read_sectors(u64 s, u64 n, void* b) { if (s + n > g_sectors) return false; __builtin_memcpy(b, g_disk + s * 512, n * 512); return true; }
bool write_sectors(u64 s, u64 n, const void* b) { if (s + n > g_sectors) return false; __builtin_memcpy(g_disk + s * 512, b, n * 512); return true; }
u64 capacity_sectors() { return g_sectors; }
bool flush() { return true; }
}

namespace orbital {
void yield() { sched_yield(); }
void yield_contended() { sched_yield(); }
u64 self_token() { static thread_local char tag; return reinterpret_cast<u64>(&tag); }
}

namespace universe {
u64 alloc(int order) { return reinterpret_cast<u64>(aligned_alloc(4096, PAGE_SIZE << order)); }
void free(u64, int) {}
}

namespace kheap {
void* alloc(usize n) { return malloc(n ? n : 1); }
void* alloc_zeroed(usize n) { return calloc(n ? n : 1, 1); }
void free(void* p) { ::free(p); }
}

static u64 g_clock = 1000;
namespace clock { u64 now_ms() { return g_clock += 7; } }

namespace serial {
void printf(const char*, ...) {}
void writeln(const char*) {}
}

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, msg) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL: %s  (line %d)\n", msg, __LINE__); } } while (0)
#define CHECK_ST(expr, want, msg) do { nebula::Status st_ = (expr); ++g_checks; if (st_ != (want)) { ++g_fail; printf("FAIL: %s  got '%s' want '%s' (line %d)\n", msg, nebula::status_text(st_), nebula::status_text(want), __LINE__); } } while (0)

using nebula::Status;
namespace np = nebula::path;

static bool same(const char* a, const char* b) { return np::equal(a, b); }

static bool resolves(const char* here_realm, const char* here_path, const char* text, const char* expect) {
    np::Path here;
    np::clear(here);
    np::copy_text(here.realm, here_realm, np::text_length(here_realm));
    if (here_path[0]) {
        np::Path tmp;
        if (np::resolve(here, here_path, tmp) != np::Error::Ok) return false;
        here = tmp;
    }
    np::Path out;
    if (np::resolve(here, text, out) != np::Error::Ok) return false;
    char buf[600];
    np::to_text(out, buf, sizeof(buf));
    if (!same(buf, expect)) printf("   resolve '%s' from %s:%s gave '%s' want '%s'\n", text, here_realm, here_path, buf, expect);
    return same(buf, expect);
}

static np::Error resolve_err(const char* text) {
    np::Path here;
    np::clear(here);
    np::copy_text(here.realm, "ram", 3);
    np::Path out;
    return np::resolve(here, text, out);
}

static void test_paths() {
    CHECK(resolves("ram", "", "a/b", "ram:/a/b"), "relative from root");
    CHECK(resolves("ram", "x/y", "z", "ram:/x/y/z"), "relative from nested");
    CHECK(resolves("ram", "x/y", "/z", "ram:/z"), "absolute keeps realm");
    CHECK(resolves("ram", "x/y", "disk:/q", "disk:/q"), "realm switch");
    CHECK(resolves("ram", "x/y", "disk:", "disk:/"), "realm only");
    CHECK(resolves("ram", "x/y", "..", "ram:/x"), "dotdot");
    CHECK(resolves("ram", "x/y", "../../..", "ram:/"), "dotdot clamps at root");
    CHECK(resolves("ram", "x/y", "./a/./b/../c", "ram:/x/y/a/c"), "dot and dotdot mix");
    CHECK(resolves("ram", "", "a//b///c/", "ram:/a/b/c"), "repeated slashes");
    CHECK(resolves("ram", "", "caf\xC3\xA9/\xE2\x82\xB9/\xCE\xA9", "ram:/caf\xC3\xA9/\xE2\x82\xB9/\xCE\xA9"), "unicode components");
    CHECK(resolves("ram", "", "\xF0\x9F\x98\x80", "ram:/\xF0\x9F\x98\x80"), "four byte scalar");
    CHECK(resolves("ram", "", "./a:b", "ram:/a:b"), "colon after dot-slash is a name");
    CHECK(resolve_err("") == np::Error::Empty, "empty text");
    CHECK(resolve_err("Disk:/x") == np::Error::BadRealm, "uppercase realm");
    CHECK(resolve_err("1x:/x") == np::Error::BadRealm, "realm starting with digit");
    CHECK(resolve_err("a:b") == np::Error::Ok, "colon before any slash names a realm");
    CHECK(resolve_err("A:b") == np::Error::BadRealm, "uppercase before colon is a bad realm");
    CHECK(resolve_err("a\x01") == np::Error::BadName, "control char in name");
    CHECK(resolve_err("a\x7f") == np::Error::BadName, "DEL in name");
    CHECK(resolve_err("\xC2\x85x") == np::Error::BadName, "C1 control in name");
    CHECK(resolve_err("a\xE2\x80\xAEz") == np::Error::BadName, "bidi override in name");
    CHECK(resolve_err("a\xE2\x80\xA8z") == np::Error::BadName, "line separator in name");
    CHECK(resolve_err("\xEF\xBB\xBFname") == np::Error::BadName, "BOM in name");
    CHECK(resolve_err("\xC0\xAF") == np::Error::BadName, "overlong encoding");
    CHECK(resolve_err("\xED\xA0\x80") == np::Error::BadName, "surrogate encoding");
    CHECK(resolve_err("\xF4\x90\x80\x80") == np::Error::BadName, "above U+10FFFF");
    CHECK(resolve_err("ab\xE2\x82") == np::Error::BadName, "truncated sequence");
    CHECK(resolve_err("\xFF") == np::Error::BadName, "invalid lead byte");
    char fifty1[64], fifty2[64];
    for (int i = 0; i < 51; ++i) fifty1[i] = 'a';
    fifty1[51] = 0;
    for (int i = 0; i < 52; ++i) fifty2[i] = 'a';
    fifty2[52] = 0;
    CHECK(resolve_err(fifty1) == np::Error::Ok, "51 byte name fits");
    CHECK(resolve_err(fifty2) == np::Error::NameTooLong, "52 byte name rejected");
    char deep[200];
    int n = 0;
    for (int i = 0; i < 33; ++i) { deep[n++] = 'd'; deep[n++] = '/'; }
    deep[n] = 0;
    CHECK(resolve_err(deep) == np::Error::TooDeep, "33 components too deep");
    char longtext[600];
    for (int i = 0; i < 599; ++i) longtext[i] = (i % 4 == 3) ? '/' : 'x';
    longtext[599] = 0;
    CHECK(resolve_err(longtext) == np::Error::TooLong, "text over 512 bytes");
}

static u8 g_scratch[1 << 16];

static Status write_text(nebula::Session& s, const char* path, const char* text) {
    return nebula::write_file(s, path, text, np::text_length(text));
}

static bool content_is(nebula::Session& s, const char* path, const char* want) {
    nebula::Blob b;
    Status st = nebula::read_file(s, path, &b);
    if (st != Status::Ok) return false;
    u64 n = np::text_length(want);
    bool ok = b.size == n && (n == 0 || memcmp(b.data, want, n) == 0);
    nebula::release(b);
    return ok;
}

static void list_string(nebula::Session& s, const char* path, char* out, u32 cap) {
    nebula::Listing l;
    out[0] = 0;
    Status st = nebula::list(s, path, &l, false);
    if (st != Status::Ok) { np::copy_text(out, nebula::status_text(st), np::text_length(nebula::status_text(st))); return; }
    u32 n = 0;
    for (u32 i = 0; i < l.count; ++i) {
        if (i) out[n++] = ',';
        for (u32 k = 0; l.items[i].name[k]; ++k) out[n++] = l.items[i].name[k];
        if (l.items[i].kind == nebula::Kind::Dir) out[n++] = '/';
    }
    out[n] = 0;
    (void)cap;
    nebula::release(l);
}

static bool listing_is(nebula::Session& s, const char* path, const char* want) {
    char buf[1024];
    list_string(s, path, buf, sizeof(buf));
    if (!same(buf, want)) printf("   listing of %s is '%s' want '%s'\n", path, buf, want);
    return same(buf, want);
}

static void suite(const char* realm) {
    printf("-- suite on %s:\n", realm);
    nebula::Session s;
    nebula::init_session(s, realm);
    char loc[128];

    nebula::location_text(s, loc, sizeof(loc));
    char want_root[32];
    np::copy_text(want_root, realm, np::text_length(realm));
    u32 rl = np::text_length(realm);
    want_root[rl] = ':'; want_root[rl + 1] = '/'; want_root[rl + 2] = 0;
    CHECK(same(loc, want_root), "starts at the realm root");

    CHECK_ST(nebula::make_dir(s, "docs"), Status::Ok, "mkdir");
    CHECK_ST(nebula::make_dir(s, "docs"), Status::Exists, "mkdir twice");
    CHECK_ST(nebula::make_dir(s, "a/b/c"), Status::NotFound, "mkdir without parent");
    CHECK_ST(nebula::make_path(s, "a/b/c"), Status::Ok, "mkdir -p");
    CHECK_ST(nebula::make_path(s, "a/b/c"), Status::Ok, "mkdir -p twice");
    CHECK_ST(write_text(s, "docs/readme.txt", "hello"), Status::Ok, "write new file");
    CHECK(content_is(s, "docs/readme.txt", "hello"), "read back");
    CHECK_ST(write_text(s, "docs/readme.txt", "hello, world"), Status::Ok, "overwrite");
    CHECK(content_is(s, "docs/readme.txt", "hello, world"), "overwrite readback");
    CHECK_ST(write_text(s, "docs/empty", ""), Status::Ok, "empty file");
    CHECK(content_is(s, "docs/empty", ""), "empty readback");
    CHECK_ST(write_text(s, "docs", "x"), Status::IsADirectory, "write over directory");
    CHECK_ST(write_text(s, "docs/readme.txt/x", "x"), Status::NotADirectory, "file as directory");
    CHECK_ST(write_text(s, "/", "x"), Status::IsADirectory, "write to root");
    CHECK_ST(write_text(s, "missing/x", "x"), Status::NotFound, "write under missing parent");

    CHECK_ST(write_text(s, "docs/caf\xC3\xA9 \xE2\x82\xB9.txt", "unicode"), Status::Ok, "unicode file name with space");
    CHECK_ST(write_text(s, "docs/\xCE\xA9mega", "omega"), Status::Ok, "greek file name");
    CHECK_ST(write_text(s, "docs/\xF0\x9F\x98\x80", "smile"), Status::Ok, "emoji file name");
    CHECK_ST(write_text(s, "docs/bad\x01name", "x"), Status::InvalidName, "control char in file name");
    CHECK(content_is(s, "docs/caf\xC3\xA9 \xE2\x82\xB9.txt", "unicode"), "unicode file read back");
    CHECK(listing_is(s, "docs", "caf\xC3\xA9 \xE2\x82\xB9.txt,empty,readme.txt,\xCE\xA9mega,\xF0\x9F\x98\x80"), "listing sorted by bytes");
    CHECK(listing_is(s, "/", "a/,docs/"), "directories first");

    nebula::Info inf;
    CHECK_ST(nebula::stat(s, "docs/readme.txt", &inf), Status::Ok, "stat file");
    CHECK(inf.kind == nebula::Kind::File && inf.size == 12, "stat size");
    CHECK_ST(nebula::stat(s, "docs", &inf), Status::Ok, "stat dir");
    CHECK(inf.kind == nebula::Kind::Dir, "stat dir kind");
    CHECK_ST(nebula::stat(s, "nope", &inf), Status::NotFound, "stat missing");

    CHECK_ST(nebula::change_location(s, "docs"), Status::Ok, "cd");
    nebula::location_text(s, loc, sizeof(loc));
    char want_docs[40];
    np::copy_text(want_docs, want_root, np::text_length(want_root));
    u32 wl = np::text_length(want_docs);
    want_docs[wl] = 'd'; want_docs[wl + 1] = 'o'; want_docs[wl + 2] = 'c'; want_docs[wl + 3] = 's'; want_docs[wl + 4] = 0;
    CHECK(same(loc, want_docs), "location after cd");
    CHECK(content_is(s, "readme.txt", "hello, world"), "relative read after cd");
    CHECK(content_is(s, "../docs/readme.txt", "hello, world"), "dotdot read");
    CHECK_ST(nebula::change_location(s, "readme.txt"), Status::NotADirectory, "cd into file");
    CHECK_ST(nebula::change_location(s, "nope"), Status::NotFound, "cd missing");
    CHECK_ST(nebula::change_location(s, ".."), Status::Ok, "cd ..");

    CHECK_ST(nebula::rename(s, "docs/empty", "docs/void", 0), Status::Ok, "rename file");
    CHECK_ST(nebula::stat(s, "docs/empty", &inf), Status::NotFound, "old name gone");
    CHECK_ST(nebula::rename(s, "docs/void", "docs/readme.txt", 0), Status::Exists, "rename onto existing");
    CHECK_ST(nebula::rename(s, "docs/void", "docs/readme.txt", nebula::RENAME_REPLACE), Status::Ok, "rename replacing");
    CHECK(content_is(s, "docs/readme.txt", ""), "replaced content");
    CHECK_ST(nebula::rename(s, "a", "a/b/inside", 0), Status::Unsupported, "move directory into itself");
    CHECK_ST(nebula::rename(s, "docs/readme.txt", "a/moved.txt", 0), Status::Ok, "move across directories");
    CHECK(content_is(s, "a/moved.txt", ""), "moved content");
    CHECK_ST(nebula::rename(s, "a/b", "docs/b2", 0), Status::Ok, "move directory");
    CHECK_ST(nebula::stat(s, "docs/b2/c", &inf), Status::Ok, "moved directory keeps children");
    CHECK_ST(nebula::rename(s, "/", "x", 0), Status::Denied, "rename root");

    CHECK_ST(write_text(s, "src.txt", "copy me"), Status::Ok, "write source");
    CHECK_ST(nebula::copy_file(s, "src.txt", "dst.txt"), Status::Ok, "copy file");
    CHECK(content_is(s, "dst.txt", "copy me") && content_is(s, "src.txt", "copy me"), "copy keeps both");
    CHECK_ST(nebula::copy_file(s, "src.txt", "docs"), Status::Ok, "copy into directory");
    CHECK(content_is(s, "docs/src.txt", "copy me"), "copy into directory name");
    CHECK_ST(nebula::copy_file(s, "docs", "x"), Status::IsADirectory, "copy of directory as file");
    CHECK_ST(nebula::copy_file(s, "src.txt", "src.txt"), Status::Exists, "copy onto itself");

    u32 copied = 0;
    CHECK_ST(write_text(s, "docs/b2/c/leaf.txt", "leaf"), Status::Ok, "deep file");
    CHECK_ST(nebula::copy_tree(s, "docs", "backup", &copied), Status::Ok, "copy tree");
    CHECK(content_is(s, "backup/b2/c/leaf.txt", "leaf"), "deep file copied");
    CHECK(content_is(s, "backup/caf\xC3\xA9 \xE2\x82\xB9.txt", "unicode"), "unicode file copied");
    CHECK(copied == 5, "copied file count");
    CHECK_ST(nebula::copy_tree(s, "docs", "docs/inner", nullptr), Status::Unsupported, "copy tree into itself");

    CHECK_ST(nebula::remove(s, "docs"), Status::NotEmpty, "rmdir of full directory");
    CHECK_ST(nebula::remove(s, "src.txt"), Status::Ok, "remove file");
    CHECK_ST(nebula::remove(s, "src.txt"), Status::NotFound, "remove twice");
    CHECK_ST(nebula::remove(s, "/"), Status::Denied, "remove root");
    u32 removed = 0;
    CHECK_ST(nebula::remove_tree(s, "backup", &removed), Status::Ok, "remove tree");
    CHECK(removed == 8, "removed count");
    CHECK_ST(nebula::stat(s, "backup", &inf), Status::NotFound, "tree gone");

    nebula::Handle h = 0, h2 = 0, h3 = 0;
    CHECK_ST(nebula::open(s, "io.bin", nebula::OPEN_READ, &h), Status::NotFound, "open missing for read");
    CHECK_ST(nebula::open(s, "io.bin", nebula::OPEN_READ | nebula::OPEN_WRITE | nebula::OPEN_CREATE, &h), Status::Ok, "create via open");
    CHECK_ST(nebula::open(s, "io.bin", nebula::OPEN_WRITE | nebula::OPEN_CREATE | nebula::OPEN_EXCLUSIVE, &h2), Status::Exists, "exclusive create");
    CHECK_ST(nebula::open(s, "io.bin", nebula::OPEN_WRITE, &h2), Status::Busy, "second writer is refused");
    CHECK_ST(nebula::open(s, "io.bin", nebula::OPEN_READ, &h2), Status::Ok, "reader beside writer");
    CHECK_ST(nebula::write_at(s, h, 0, "0123456789", 10), Status::Ok, "write at 0");
    CHECK_ST(nebula::write_at(s, h, 4, "ABCD", 4), Status::Ok, "overwrite middle");
    u64 got = 0;
    char rb[32];
    CHECK_ST(nebula::read_at(s, h, 2, rb, 8, &got), Status::Ok, "partial read");
    CHECK(got == 8 && memcmp(rb, "23ABCD89", 8) == 0, "partial read bytes");
    CHECK_ST(nebula::read_at(s, h, 100, rb, 8, &got), Status::Ok, "read past end");
    CHECK(got == 0, "read past end is empty");
    CHECK_ST(nebula::read_at(s, h, 8, rb, 8, &got), Status::Ok, "read crossing end");
    CHECK(got == 2, "read crossing end is short");
    CHECK_ST(nebula::write_at(s, h, 20, "Z", 1), Status::Ok, "write beyond end");
    u64 sz = 0;
    CHECK_ST(nebula::size_of(s, h, &sz), Status::Ok, "size_of");
    CHECK(sz == 21, "size after sparse write");
    CHECK_ST(nebula::read_at(s, h, 10, rb, 11, &got), Status::Ok, "read the hole");
    bool hole_zero = got == 11;
    for (int i = 0; i < 10; ++i) hole_zero = hole_zero && rb[i] == 0;
    CHECK(hole_zero && rb[10] == 'Z', "hole reads as zero");
    CHECK_ST(nebula::append(s, h, "++", 2), Status::Ok, "append");
    CHECK_ST(nebula::resize(s, h, 5), Status::Ok, "shrink");
    CHECK_ST(nebula::resize(s, h, 8), Status::Ok, "grow");
    CHECK_ST(nebula::read_at(s, h, 0, rb, 8, &got), Status::Ok, "read after resize");
    CHECK(got == 8 && memcmp(rb, "0123A\0\0\0", 8) == 0, "grow zero fills the shrunk bytes");
    CHECK_ST(nebula::write_at(s, h2, 0, "x", 1), Status::Denied, "write on read handle");
    CHECK_ST(nebula::read_at(s, h2, 0, rb, 4, &got), Status::Ok, "reader sees disk contents");
    CHECK(got == 0, "reader sees nothing before the writer syncs");
    CHECK_ST(nebula::sync(s, h), Status::Ok, "sync");
    CHECK_ST(nebula::close(s, h2), Status::Ok, "close reader");
    CHECK_ST(nebula::close(s, h), Status::Ok, "close writer");
    CHECK_ST(nebula::close(s, h), Status::BadHandle, "double close");
    CHECK_ST(nebula::read_at(s, h, 0, rb, 4, &got), Status::BadHandle, "stale handle");
    CHECK(content_is(s, "io.bin", "0123A\1\1\1") == false, "content differs from a wrong guess");
    nebula::Blob blob;
    CHECK_ST(nebula::read_file(s, "io.bin", &blob), Status::Ok, "read io.bin");
    CHECK(blob.size == 8 && memcmp(blob.data, "0123A\0\0\0", 8) == 0, "persisted contents");
    nebula::release(blob);

    CHECK_ST(nebula::open(s, "io.bin", nebula::OPEN_WRITE | nebula::OPEN_TRUNCATE, &h), Status::Ok, "open truncate");
    CHECK_ST(nebula::close(s, h), Status::Ok, "close truncated");
    CHECK(content_is(s, "io.bin", ""), "truncate empties the file");
    CHECK_ST(nebula::append_file(s, "log.txt", "one\n", 4), Status::Ok, "append_file creates");
    CHECK_ST(nebula::append_file(s, "log.txt", "two\n", 4), Status::Ok, "append_file appends");
    CHECK(content_is(s, "log.txt", "one\ntwo\n"), "append_file contents");

    nebula::Handle many[nebula::MAX_HANDLES + 1];
    u32 opened = 0;
    for (u32 i = 0; i < nebula::MAX_HANDLES + 1; ++i) {
        Status st = nebula::open(s, "log.txt", nebula::OPEN_READ, &many[i]);
        if (st == Status::Ok) ++opened;
        else CHECK(st == Status::Busy, "handle exhaustion is Busy");
    }
    CHECK(opened == nebula::MAX_HANDLES, "exactly MAX_HANDLES opens");
    for (u32 i = 0; i < opened; ++i) nebula::close(s, many[i]);

    CHECK_ST(nebula::open(s, "unflushed.txt", nebula::OPEN_WRITE | nebula::OPEN_CREATE, &h3), Status::Ok, "open to leave dangling");
    CHECK_ST(nebula::write_at(s, h3, 0, "kept", 4), Status::Ok, "write to dangling handle");
    nebula::close_session(s);
    CHECK(content_is(s, "unflushed.txt", "kept"), "close_session flushes");

    CHECK_ST(nebula::remove_tree(s, "a", nullptr), Status::Ok, "cleanup a");
    CHECK_ST(nebula::remove_tree(s, "docs", nullptr), Status::Ok, "cleanup docs");
    CHECK_ST(nebula::remove(s, "dst.txt"), Status::Ok, "cleanup dst");
    CHECK_ST(nebula::remove(s, "io.bin"), Status::Ok, "cleanup io");
    CHECK_ST(nebula::remove(s, "log.txt"), Status::Ok, "cleanup log");
    CHECK_ST(nebula::remove(s, "unflushed.txt"), Status::Ok, "cleanup unflushed");
    CHECK(listing_is(s, "/", ""), "realm is empty after cleanup");
    nebula::close_session(s);
}

static void test_limits() {
    printf("-- limits and failures\n");
    nebula::Session s;
    nebula::init_session(s, "ram");
    nebula::Handle h;
    CHECK_ST(nebula::open(s, "big", nebula::OPEN_WRITE | nebula::OPEN_CREATE, &h), Status::Ok, "open big");
    CHECK_ST(nebula::write_at(s, h, nebula::MAX_FILE_BYTES, "x", 1), Status::TooLarge, "write past 16 MiB");
    CHECK_ST(nebula::resize(s, h, nebula::MAX_FILE_BYTES + 1), Status::TooLarge, "resize past 16 MiB");
    CHECK_ST(nebula::resize(s, h, nebula::MAX_FILE_BYTES), Status::Ok, "resize to exactly 16 MiB");
    CHECK_ST(nebula::sync(s, h), Status::Ok, "sync 16 MiB");
    CHECK_ST(nebula::close(s, h), Status::Ok, "close big");
    nebula::Info inf;
    CHECK(nebula::stat(s, "big", &inf) == Status::Ok && inf.size == nebula::MAX_FILE_BYTES, "16 MiB file stored");
    CHECK_ST(nebula::copy_file(s, "big", "big2"), Status::Ok, "second 16 MiB");
    CHECK_ST(nebula::copy_file(s, "big", "big3"), Status::Ok, "third 16 MiB");
    CHECK_ST(nebula::copy_file(s, "big", "big4"), Status::NoSpace, "ram limit reached");
    CHECK_ST(nebula::stat(s, "big4", &inf), Status::NotFound, "failed copy leaves no empty file");
    nebula::Realm* ram = nebula::find_realm("ram");
    CHECK(ram->space_free() < 1u << 20, "free space accounts for contents");
    CHECK_ST(nebula::remove(s, "big2"), Status::Ok, "free some");
    CHECK(ram->space_free() >= nebula::MAX_FILE_BYTES, "space returns after remove");
    CHECK_ST(nebula::remove(s, "big"), Status::Ok, "rm big");
    CHECK_ST(nebula::remove(s, "big3"), Status::Ok, "rm big3");
    CHECK(ram->space_free() == nebula::RAM_LIMIT_BYTES, "all space back");

    for (u32 i = 0; i < 40; ++i) {
        char name[16] = "d";
        name[1] = static_cast<char>('0' + i / 10);
        name[2] = static_cast<char>('0' + i % 10);
        name[3] = 0;
        CHECK_ST(nebula::make_dir(s, name), Status::Ok, "many dirs");
    }
    char buf[2048];
    list_string(s, "/", buf, sizeof(buf));
    CHECK(np::text_length(buf) == 40 * 4 + 39, "many entries listed");
    { u32 L = np::text_length(buf); CHECK(buf[0] == 'd' && buf[1] == '0' && buf[2] == '0' && buf[L - 1] == '/' && buf[L - 2] == '9' && buf[L - 3] == '3', "many entries sorted"); }
    for (u32 i = 0; i < 40; ++i) {
        char name[16] = "d";
        name[1] = static_cast<char>('0' + i / 10);
        name[2] = static_cast<char>('0' + i % 10);
        name[3] = 0;
        CHECK_ST(nebula::remove(s, name), Status::Ok, "remove many dirs");
    }

    Status st = Status::Ok;
    u32 made = 0;
    for (u32 i = 0; i < nebula::RAM_MAX_ITEMS + 8 && st == Status::Ok; ++i) {
        char name[16];
        u32 v = i, n = 0;
        name[n++] = 'f';
        char d[8]; u32 dl = 0;
        if (!v) d[dl++] = '0';
        while (v) { d[dl++] = static_cast<char>('0' + v % 10); v /= 10; }
        while (dl) name[n++] = d[--dl];
        name[n] = 0;
        st = nebula::write_file(s, name, "x", 1);
        if (st == Status::Ok) ++made;
    }
    CHECK(st == Status::NoSpace, "node table exhaustion is NoSpace");
    CHECK(made == nebula::RAM_MAX_ITEMS - 1, "node table holds exactly the configured items");
    CHECK_ST(nebula::remove_tree(s, "f0", nullptr), Status::Ok, "free one node");
    CHECK_ST(nebula::write_file(s, "again", "x", 1), Status::Ok, "node reusable");
    for (u32 i = 0; i < nebula::RAM_MAX_ITEMS; ++i) {
        char name[16];
        u32 v = i, n = 0;
        name[n++] = 'f';
        char d[8]; u32 dl = 0;
        if (!v) d[dl++] = '0';
        while (v) { d[dl++] = static_cast<char>('0' + v % 10); v /= 10; }
        while (dl) name[n++] = d[--dl];
        name[n] = 0;
        nebula::remove(s, name);
    }
    nebula::remove(s, "again");

    nebula::Resolved r;
    CHECK_ST(nebula::resolve(s, "nosuch:/x", &r), Status::NoRealm, "unknown realm");
    CHECK_ST(nebula::resolve(s, "BAD:/x", &r), Status::InvalidPath, "malformed realm");
    nebula::close_session(s);
}

static void test_disk_specific() {
    printf("-- disk, snap and cross-realm\n");
    nebula::Session s;
    nebula::init_session(s, "disk");
    CHECK_ST(write_text(s, "notes.txt", "version one"), Status::Ok, "write notes");
    CHECK_ST(nebula::make_path(s, "proj/src"), Status::Ok, "mkdir proj");
    CHECK_ST(write_text(s, "proj/src/main.c", "int main;"), Status::Ok, "write main");
    CHECK(listing_is(s, "snap:/", ""), "no snapshots yet");

    u64 id1 = 0;
    CHECK_ST(nebula::disk_snapshot(&id1), Status::Ok, "snapshot");
    CHECK(id1 != 0, "snapshot id");
    CHECK_ST(write_text(s, "notes.txt", "version two"), Status::Ok, "change notes");
    CHECK_ST(write_text(s, "proj/src/extra.c", "new"), Status::Ok, "add file");
    CHECK_ST(nebula::remove(s, "proj/src/main.c"), Status::Ok, "delete main");

    char idtext[32];
    u32 n = 0;
    {
        char t[24]; u32 k = 0; u64 v = id1;
        while (v) { t[k++] = static_cast<char>('0' + v % 10); v /= 10; }
        while (k) idtext[n++] = t[--k];
        idtext[n] = 0;
    }
    char path[128];
    auto join = [&](const char* tail) {
        const char* pre = "snap:/";
        u32 p = 0;
        for (u32 i = 0; pre[i]; ++i) path[p++] = pre[i];
        for (u32 i = 0; idtext[i]; ++i) path[p++] = idtext[i];
        for (u32 i = 0; tail[i]; ++i) path[p++] = tail[i];
        path[p] = 0;
        return static_cast<const char*>(path);
    };

    { char want[40]; u32 k = np::text_length(idtext); np::copy_text(want, idtext, k); want[k] = '/'; want[k + 1] = 0; CHECK(listing_is(s, "snap:/", want), "snapshot listed"); }
    CHECK(content_is(s, join("/notes.txt"), "version one"), "snapshot keeps old contents");
    CHECK(content_is(s, "notes.txt", "version two"), "live has new contents");
    CHECK(content_is(s, join("/proj/src/main.c"), "int main;"), "deleted file lives in snapshot");
    CHECK(listing_is(s, join("/proj/src"), "main.c"), "snapshot directory listing");
    CHECK(listing_is(s, "proj/src", "extra.c"), "live directory listing");
    CHECK_ST(write_text(s, join("/notes.txt"), "x"), Status::ReadOnly, "snap is read-only for writes");
    CHECK_ST(nebula::remove(s, join("/notes.txt")), Status::ReadOnly, "snap is read-only for removal");
    CHECK_ST(nebula::make_dir(s, join("/newdir")), Status::ReadOnly, "snap is read-only for mkdir");
    nebula::Info inf;
    CHECK_ST(nebula::stat(s, "snap:/9999", &inf), Status::NotFound, "unknown snapshot");
    CHECK_ST(nebula::stat(s, "snap:/abc", &inf), Status::NotFound, "non numeric snapshot");
    CHECK_ST(nebula::stat(s, "snap:/01", &inf), Status::NotFound, "leading zero snapshot");

    CHECK_ST(nebula::copy_tree(s, join("/proj"), "disk:/restored", nullptr), Status::Ok, "restore from snapshot");
    CHECK(content_is(s, "disk:/restored/src/main.c", "int main;"), "restored file");
    CHECK_ST(nebula::copy_file(s, join("/notes.txt"), "disk:/notes.txt"), Status::Ok, "restore single file");
    CHECK(content_is(s, "notes.txt", "version one"), "single restore content");

    CHECK_ST(nebula::copy_file(s, "notes.txt", "ram:/notes.copy"), Status::Ok, "disk to ram");
    CHECK(content_is(s, "ram:/notes.copy", "version one"), "ram copy");
    CHECK_ST(write_text(s, "ram:/fresh", "from ram"), Status::Ok, "write ram");
    CHECK_ST(nebula::copy_file(s, "ram:/fresh", "disk:/fresh"), Status::Ok, "ram to disk");
    CHECK(content_is(s, "disk:/fresh", "from ram"), "disk copy");
    CHECK_ST(nebula::copy_tree(s, "disk:/proj", "ram:/proj", nullptr), Status::Ok, "tree disk to ram");
    CHECK(content_is(s, "ram:/proj/src/extra.c", "new"), "tree copied to ram");
    CHECK_ST(nebula::rename(s, "ram:/fresh", "disk:/fresh2", 0), Status::CrossRealm, "rename across realms");
    CHECK_ST(nebula::remove_tree(s, "ram:/proj", nullptr), Status::Ok, "clean ram tree");
    CHECK_ST(nebula::remove(s, "ram:/fresh"), Status::Ok, "clean ram fresh");
    CHECK_ST(nebula::remove(s, "ram:/notes.copy"), Status::Ok, "clean ram copy");

    CHECK_ST(nebula::disk_snapshot_delete(id1), Status::Ok, "delete snapshot");
    CHECK(listing_is(s, "snap:/", ""), "snapshot gone");
    CHECK_ST(nebula::disk_snapshot_delete(id1), Status::NotFound, "delete twice");

    u64 a = 0, b = 0;
    CHECK_ST(nebula::disk_snapshot(&a), Status::Ok, "snapshot a");
    CHECK_ST(nebula::disk_snapshot(&b), Status::Ok, "snapshot b");
    CHECK(b > a && a > id1, "ids keep growing after deletes");
    CHECK_ST(nebula::disk_snapshot_delete(a), Status::Ok, "delete a");
    u64 ids[8];
    CHECK(stellar::snapshot_ids(ids, 8) == 1 && ids[0] == b, "snapshot_ids lists survivors");
    CHECK_ST(nebula::disk_snapshot_delete(b), Status::Ok, "delete b");

    CHECK_ST(nebula::make_dir(s, "dir with \xE2\x82\xB9"), Status::Ok, "unicode directory on disk");
    CHECK_ST(write_text(s, "dir with \xE2\x82\xB9/\xCE\xA9", "omega"), Status::Ok, "unicode file in unicode directory");
    CHECK(content_is(s, "dir with \xE2\x82\xB9/\xCE\xA9", "omega"), "unicode path round trip");
    CHECK_ST(nebula::remove(s, "dir with \xE2\x82\xB9"), Status::NotEmpty, "unlink of full directory is refused by Stellar");

    stellar::CheckReport rep;
    CHECK(stellar::check(&rep, true), "stellar check passes after all of this");

    nebula::disk_set_mounted(false);
    CHECK_ST(nebula::stat(s, "disk:/notes.txt", &inf), Status::Unavailable, "unmounted disk");
    CHECK_ST(nebula::stat(s, "snap:/", &inf), Status::Unavailable, "unmounted snap");
    nebula::disk_set_mounted(true);
    CHECK_ST(nebula::stat(s, "disk:/notes.txt", &inf), Status::Ok, "mounted again");
    nebula::close_session(s);
}

static void test_no_space_cleanup() {
    printf("-- disk full\n");
    free(g_disk);
    g_sectors = 6144;
    g_disk = static_cast<u8*>(calloc(g_sectors, 512));
    stellar::init(0);
    CHECK(stellar::format(g_sectors), "format small volume");
    nebula::disk_set_mounted(true);
    nebula::Session s;
    nebula::init_session(s, "disk");
    u32 chunk = 1 << 20;
    u8* big = static_cast<u8*>(calloc(chunk * 4, 1));
    CHECK_ST(nebula::write_file(s, "huge", big, chunk * 4), Status::NoSpace, "file larger than the volume");
    nebula::Info inf;
    CHECK_ST(nebula::stat(s, "huge", &inf), Status::NotFound, "no empty leftover after failed create");
    nebula::Handle h;
    CHECK_ST(nebula::open(s, "huge2", nebula::OPEN_READ | nebula::OPEN_WRITE | nebula::OPEN_CREATE, &h), Status::Ok, "open to fill");
    CHECK_ST(nebula::write_at(s, h, 0, big, chunk * 4), Status::Ok, "buffered write succeeds in memory");
    CHECK_ST(nebula::close(s, h), Status::NoSpace, "close reports the write-back failure");
    free(big);
    CHECK_ST(nebula::remove(s, "huge2"), Status::Ok, "remove the empty file after failed flush");
    CHECK(stellar::check(&(*new (malloc(sizeof(stellar::CheckReport))) stellar::CheckReport{}), true), "volume still consistent");
}

class DynamicRealm final : public nebula::Realm {
public:
    const char* name() const override { return "dyn"; }
    const char* summary() const override { return "changing size"; }
    bool writable() const override { return false; }
    bool available() const override { return true; }
    nebula::Node root() override { return {0, 1}; }
    Status find(nebula::Node d, const char* n, nebula::Node* out) override {
        if (d.id != 0 || !same(n, "tick")) return Status::NotFound;
        *out = {1, 1};
        return Status::Ok;
    }
    Status info(nebula::Node n, nebula::Info* out) override {
        out->kind = n.id == 0 ? nebula::Kind::Dir : nebula::Kind::File;
        out->size = n.id == 0 ? 0 : 5;
        out->mtime_ms = 0;
        out->links = 1;
        out->read_only = true;
        out->dynamic = n.id != 0;
        return Status::Ok;
    }
    Status list(nebula::Node, nebula::ChildFn fn, void* ctx) override { fn("tick", nebula::Kind::File, {1, 1}, ctx); return Status::Ok; }
    Status read_all(nebula::Node, void* buf, u64 cap, u64* size) override {
        const char* text = ++reads_ % 2 ? "much longer than five" : "tiny";
        u64 n = np::text_length(text);
        if (n > cap) n = cap;
        memcpy(buf, text, n);
        *size = n;
        return Status::Ok;
    }
    Status write_all(nebula::Node, const void*, u64) override { return Status::ReadOnly; }
    Status create_file(nebula::Node, const char*, nebula::Node*) override { return Status::ReadOnly; }
    Status make_dir(nebula::Node, const char*, nebula::Node*) override { return Status::ReadOnly; }
    Status remove(nebula::Node, const char*) override { return Status::ReadOnly; }
    Status rename(nebula::Node, const char*, nebula::Node, const char*, bool) override { return Status::ReadOnly; }
    u32 reads_ = 0;
};

static void test_dynamic() {
    printf("-- dynamic realms\n");
    static DynamicRealm dyn;
    CHECK(nebula::register_realm(&dyn), "register dyn");
    CHECK(!nebula::register_realm(&dyn), "duplicate realm name refused");
    nebula::Session s;
    nebula::init_session(s, "dyn");
    nebula::Blob b;
    CHECK_ST(nebula::read_file(s, "tick", &b), Status::Ok, "read grows beyond reported size");
    CHECK(b.size == 21, "dynamic read uses what was read");
    nebula::release(b);
    CHECK_ST(nebula::read_file(s, "tick", &b), Status::Ok, "read shrinks below reported size");
    CHECK(b.size == 4, "dynamic short read accepted");
    nebula::release(b);
    nebula::Handle h;
    CHECK_ST(nebula::open(s, "tick", nebula::OPEN_READ, &h), Status::Ok, "open dynamic");
    u64 got;
    char rb[32];
    CHECK_ST(nebula::read_at(s, h, 0, rb, 32, &got), Status::Ok, "read dynamic handle");
    CHECK(got == 21, "dynamic handle contents");
    nebula::close(s, h);
    CHECK_ST(nebula::open(s, "tick", nebula::OPEN_WRITE, &h), Status::ReadOnly, "dynamic realm refuses write open");
}

static void test_status_text() {
    for (u32 i = 0; i <= static_cast<u32>(Status::Denied); ++i) {
        const char* t = nebula::status_text(static_cast<Status>(i));
        CHECK(t && t[0] && !same(t, "unknown"), "every status has text");
    }
}

int main() {
    test_paths();
    test_status_text();

    g_sectors = 131072;
    g_disk = static_cast<u8*>(calloc(g_sectors, 512));
    stellar::init(0);
    CHECK(stellar::format(g_sectors), "format");
    nebula::disk_set_mounted(true);
    CHECK(nebula::register_realm(nebula::ram_realm()), "register ram");
    CHECK(nebula::register_realm(nebula::disk_realm()), "register disk");
    CHECK(nebula::register_realm(nebula::snap_realm()), "register snap");
    CHECK(nebula::realm_count() == 3, "realm count");

    CHECK(nebula::selftest(), "built-in selftest on a fresh volume");
    suite("ram");
    suite("disk");
    test_limits();
    test_disk_specific();
    test_dynamic();
    test_no_space_cleanup();

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
