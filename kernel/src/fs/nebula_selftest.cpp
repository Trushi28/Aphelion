#include <cosmos/nebula_realms.hpp>
#include <cosmos/serial.hpp>

namespace nebula {

namespace {

bool g_ok;

void expect(bool cond, const char* what) {
    if (cond) return;
    g_ok = false;
    serial::printf("[nebula] selftest failed: %s\n", what);
}

void expect_status(Status got, Status want, const char* what) {
    if (got == want) return;
    g_ok = false;
    serial::printf("[nebula] selftest failed: %s (got '%s', wanted '%s')\n", what, status_text(got), status_text(want));
}

bool content_is(Session& s, const char* path, const char* want) {
    Blob b;
    if (read_file(s, path, &b) != Status::Ok) return false;
    u64 n = path::text_length(want);
    bool ok = b.size == n && (n == 0 || __builtin_memcmp(b.data, want, static_cast<usize>(n)) == 0);
    release(b);
    return ok;
}

bool names_are(Session& s, const char* path, const char* a, const char* b) {
    Listing l;
    if (list(s, path, &l, false) != Status::Ok) return false;
    bool ok = l.count == (b ? 2u : 1u) && path::equal(l.items[0].name, a) && (!b || path::equal(l.items[1].name, b));
    release(l);
    return ok;
}

void test_paths() {
    path::Path here;
    path::clear(here);
    path::copy_text(here.realm, "ram", 3);
    path::Path out;
    expect(path::resolve(here, "a/b/../c", out) == path::Error::Ok && out.count == 2 && path::equal(out.comps[1], "c"), "path dotdot");
    expect(path::resolve(here, "disk:/x", out) == path::Error::Ok && path::equal(out.realm, "disk"), "path realm switch");
    expect(path::resolve(here, "caf\xC3\xA9/\xE2\x82\xB9", out) == path::Error::Ok && out.count == 2, "path unicode");
    expect(path::resolve(here, "bad\xFF", out) == path::Error::BadName, "path invalid utf-8");
    expect(path::resolve(here, "a\xE2\x80\xAE" "b", out) == path::Error::BadName, "path bidi override");
}

void test_ram() {
    Session s;
    init_session(s, "ram");
    expect_status(make_path(s, "selftest/sub/\xCE\xA9"), Status::Ok, "ram mkdir -p");
    expect_status(write_file(s, "selftest/sub/caf\xC3\xA9.txt", "hello", 5), Status::Ok, "ram write");
    expect(content_is(s, "selftest/sub/caf\xC3\xA9.txt", "hello"), "ram read back");
    expect(names_are(s, "selftest/sub", "\xCE\xA9", "caf\xC3\xA9.txt"), "ram listing order");

    Handle h = 0;
    expect_status(open(s, "selftest/io", OPEN_READ | OPEN_WRITE | OPEN_CREATE, &h), Status::Ok, "ram open");
    expect_status(write_at(s, h, 0, "0123456789", 10), Status::Ok, "ram write_at");
    expect_status(write_at(s, h, 4, "AB", 2), Status::Ok, "ram overwrite");
    char buf[8];
    u64 got = 0;
    expect_status(read_at(s, h, 3, buf, 5, &got), Status::Ok, "ram read_at");
    expect(got == 5 && __builtin_memcmp(buf, "3AB67", 5) == 0, "ram partial read");
    expect_status(close(s, h), Status::Ok, "ram close");
    expect(content_is(s, "selftest/io", "0123AB6789"), "ram write-back");

    expect_status(rename(s, "selftest/io", "selftest/sub/io2", 0), Status::Ok, "ram rename");
    expect_status(rename(s, "selftest", "selftest/sub/inside", 0), Status::Unsupported, "ram rename into itself");
    u32 copied = 0;
    expect_status(copy_tree(s, "selftest", "selftest-copy", &copied), Status::Ok, "ram copy_tree");
    expect(copied == 2 && content_is(s, "selftest-copy/sub/caf\xC3\xA9.txt", "hello"), "ram copy contents");
    u32 removed = 0;
    expect_status(remove_tree(s, "selftest-copy", &removed), Status::Ok, "ram remove_tree copy");
    expect(removed == 5, "ram removal count");
    expect_status(remove_tree(s, "selftest", &removed), Status::Ok, "ram remove_tree");
    Info inf;
    expect_status(stat(s, "selftest", &inf), Status::NotFound, "ram gone");
    close_session(s);
}

void test_sys() {
    if (!find_realm("sys")) return;
    Session s;
    init_session(s, "sys");
    Blob b;
    expect_status(read_file(s, "version", &b), Status::Ok, "sys read version");
    expect(b.size > 0, "sys version is not empty");
    release(b);
    expect_status(write_file(s, "version", "x", 1), Status::ReadOnly, "sys is read-only");
    Info inf;
    expect_status(stat(s, "version", &inf), Status::Ok, "sys stat");
    expect(inf.read_only, "sys files report read-only");
    close_session(s);
}

void test_disk() {
    if (!disk_is_mounted()) {
        serial::writeln("[nebula] no mounted volume: the disk and snap realms were not exercised");
        return;
    }
    Session s;
    init_session(s, "disk");
    remove_tree(s, "nebula-selftest", nullptr);
    expect_status(make_path(s, "nebula-selftest/dir"), Status::Ok, "disk mkdir");
    expect_status(write_file(s, "nebula-selftest/dir/note.txt", "one", 3), Status::Ok, "disk write");
    u64 snap = 0;
    expect_status(disk_snapshot(&snap), Status::Ok, "disk snapshot");
    expect_status(write_file(s, "nebula-selftest/dir/note.txt", "two", 3), Status::Ok, "disk overwrite");
    expect(content_is(s, "nebula-selftest/dir/note.txt", "two"), "disk live contents");
    char path[96];
    u32 n = 0;
    const char* pre = "snap:/";
    for (u32 i = 0; pre[i]; ++i) path[n++] = pre[i];
    char digits[24];
    u32 d = 0;
    for (u64 v = snap; v; v /= 10) digits[d++] = static_cast<char>('0' + v % 10);
    while (d) path[n++] = digits[--d];
    const char* tail = "/nebula-selftest/dir/note.txt";
    for (u32 i = 0; tail[i]; ++i) path[n++] = tail[i];
    path[n] = 0;
    expect(content_is(s, path, "one"), "snapshot keeps old contents");
    expect_status(write_file(s, path, "x", 1), Status::ReadOnly, "snap is read-only");
    expect_status(copy_file(s, "nebula-selftest/dir/note.txt", "ram:/nebula-note"), Status::Ok, "disk to ram copy");
    expect(content_is(s, "ram:/nebula-note", "two"), "disk to ram contents");
    expect_status(remove(s, "ram:/nebula-note"), Status::Ok, "ram cleanup");
    expect_status(remove_tree(s, "nebula-selftest", nullptr), Status::Ok, "disk cleanup");
    expect_status(disk_snapshot_delete(snap), Status::Ok, "snapshot cleanup");
    stellar::CheckReport rep{};
    expect(stellar::check(&rep, false) && rep.ok(), "volume is consistent afterwards");
    close_session(s);
}

}

bool selftest() {
    g_ok = true;
    test_paths();
    test_ram();
    test_sys();
    test_disk();
    return g_ok;
}

}
