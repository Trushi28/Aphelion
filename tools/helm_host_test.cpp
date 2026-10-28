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
#include <cosmos/helm.hpp>
#include <cosmos/lens_text.hpp>
#include <cosmos/helm_parse.hpp>
#include <cosmos/helm_rows.hpp>

extern "C" {
int printf(const char*, ...);
void* malloc(unsigned long);
void* aligned_alloc(unsigned long, unsigned long);
void* calloc(unsigned long, unsigned long);
void free(void*);
int sched_yield(void);
}

static u8* g_disk;
static u64 g_sectors;

namespace blockdev {
bool read_sector(u64 s, void* b) { if (s >= g_sectors) return false; mem::copy(b, g_disk + s * 512, 512); return true; }
bool write_sector(u64 s, const void* b) { if (s >= g_sectors) return false; mem::copy(g_disk + s * 512, b, 512); return true; }
bool read_sectors(u64 s, u64 n, void* b) { if (s + n > g_sectors) return false; mem::copy(b, g_disk + s * 512, n * 512); return true; }
bool write_sectors(u64 s, u64 n, const void* b) { if (s + n > g_sectors) return false; mem::copy(g_disk + s * 512, b, n * 512); return true; }
u64 capacity_sectors() { return g_sectors; }
bool flush() { return true; }
bool present() { return true; }
}

namespace orbital {
void yield() { sched_yield(); }
void yield_contended() { sched_yield(); }
u64 self_token() { static thread_local char tag; return reinterpret_cast<u64>(&tag); }
bool WaitQueue::wait_until_for(bool (*)(void*), void*, u64) { return false; }
u32 WaitQueue::wake_all() { return 0; }
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

static u64 g_clock = 1800000000000ull;
namespace clock { u64 now_ms() { return g_clock += 61000; } u64 uptime_ms() { return 0; } }

namespace serial {
void printf(const char*, ...) {}
void writeln(const char*) {}
}

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, msg) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL: %s  (line %d)\n", msg, __LINE__); } } while (0)

static bool same(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static bool contains(const char* hay, const char* needle) {
    u32 n = cstr::length(needle);
    for (u32 i = 0; hay[i]; ++i) {
        u32 k = 0;
        while (k < n && hay[i + k] == needle[k]) ++k;
        if (k == n) return true;
    }
    return n == 0;
}

static char g_out[1 << 18];
static u32 g_out_len;
static void sink_write(void*, const char* d, u32 n) {
    for (u32 i = 0; i < n && g_out_len + 1 < sizeof(g_out); ++i) g_out[g_out_len++] = d[i];
    g_out[g_out_len] = 0;
}

static helm::Shell* g_sh;
static int g_status;

static const char* sh(const char* line) {
    g_out_len = 0;
    g_out[0] = 0;
    g_status = helm::run_line(*g_sh, line);
    return g_out;
}

static bool out_is(const char* line, const char* want) {
    const char* got = sh(line);
    if (!same(got, want)) {
        printf("   '%s'\n     got  '%s'\n     want '%s'\n", line, got, want);
        return false;
    }
    return true;
}

static bool out_has(const char* line, const char* want) {
    const char* got = sh(line);
    if (!contains(got, want)) {
        printf("   '%s'\n     got  '%s'\n     missing '%s'\n", line, got, want);
        return false;
    }
    return true;
}

class FakeSys final : public nebula::Realm {
public:
    const char* name() const override { return "sys"; }
    const char* summary() const override { return "stand-in for the kernel sys realm"; }
    bool writable() const override { return false; }
    bool available() const override { return true; }
    nebula::Node root() override { return {0, 1}; }
    nebula::Status find(nebula::Node d, const char* n, nebula::Node* out) override {
        if (d.id != 0 || !same(n, "version")) return nebula::Status::NotFound;
        *out = {1, 1};
        return nebula::Status::Ok;
    }
    nebula::Status info(nebula::Node n, nebula::Info* out) override {
        out->kind = n.id == 0 ? nebula::Kind::Dir : nebula::Kind::File;
        out->size = n.id == 0 ? 0 : 8;
        out->mtime_ms = 0;
        out->links = 1;
        out->read_only = true;
        out->dynamic = n.id != 0;
        return nebula::Status::Ok;
    }
    nebula::Status list(nebula::Node, nebula::ChildFn fn, void* ctx) override { fn("version", nebula::Kind::File, {1, 1}, ctx); return nebula::Status::Ok; }
    nebula::Status read_all(nebula::Node, void* buf, u64 cap, u64* size) override {
        const char* t = "Aphelion\n";
        u64 n = cstr::length(t);
        if (n > cap) n = cap;
        mem::copy(buf, t, n);
        *size = n;
        return nebula::Status::Ok;
    }
    nebula::Status write_all(nebula::Node, const void*, u64) override { return nebula::Status::ReadOnly; }
    nebula::Status create_file(nebula::Node, const char*, nebula::Node*) override { return nebula::Status::ReadOnly; }
    nebula::Status make_dir(nebula::Node, const char*, nebula::Node*) override { return nebula::Status::ReadOnly; }
    nebula::Status remove(nebula::Node, const char*) override { return nebula::Status::ReadOnly; }
    nebula::Status rename(nebula::Node, const char*, nebula::Node, const char*, bool) override { return nebula::Status::ReadOnly; }
};

static FakeSys g_fake_sys;

namespace hp = helm::parse;
namespace hr = helm::rows;

static hp::Parsed g_parsed;
static hp::Error parse_line(const char* line) { return hp::parse(line, g_parsed); }

static bool word_is(u32 index, const char* want) {
    if (index >= g_parsed.word_count) return false;
    const char* got = g_parsed.text(g_parsed.words[index]);
    if (!same(got, want)) printf("   word %u is '%s' want '%s'\n", index, got, want);
    return same(got, want);
}

static void test_parser() {
    printf("-- parser\n");
    CHECK(parse_line("") == hp::Error::Ok && g_parsed.pipe_count == 0, "empty line");
    CHECK(parse_line("   \t ") == hp::Error::Ok && g_parsed.pipe_count == 0, "blank line");
    CHECK(parse_line("# nothing") == hp::Error::Ok && g_parsed.pipe_count == 0, "comment only");
    CHECK(parse_line("chart depth: 2 docs") == hp::Error::Ok && g_parsed.word_count == 4 && word_is(0, "chart") && word_is(1, "depth:") && word_is(3, "docs"), "plain words");
    CHECK(parse_line("say 'a  b' \"c  d\"") == hp::Error::Ok && g_parsed.word_count == 3 && word_is(1, "a  b") && word_is(2, "c  d"), "quotes keep spaces");
    CHECK(parse_line("say a\\ b") == hp::Error::Ok && g_parsed.word_count == 2 && word_is(1, "a b"), "escaped space");
    CHECK(parse_line("say ''") == hp::Error::Ok && g_parsed.word_count == 2 && g_parsed.words[1].len == 0 && g_parsed.words[1].quoted, "empty quoted word");
    CHECK(parse_line("say 'depth:' depth:") == hp::Error::Ok && g_parsed.words[1].quoted && !g_parsed.words[2].quoted, "quoting is recorded for option detection");
    CHECK(parse_line("say \"\\u{20B9} \\u{1F600}\"") == hp::Error::Ok && word_is(1, "\xE2\x82\xB9 \xF0\x9F\x98\x80"), "unicode escapes");
    CHECK(parse_line("say \"\\u{D800}\"") == hp::Error::BadEscape, "surrogate escape refused");
    CHECK(parse_line("say \"\\u{110000}\"") == hp::Error::BadEscape, "out of range escape refused");
    CHECK(parse_line("say \"\\u{1B}\"") == hp::Error::BadEscape, "control escape refused");
    CHECK(parse_line("say \"\\u20B9\"") == hp::Error::BadEscape, "escape without braces refused");
    CHECK(parse_line("say \"a\\nb\\tc\"") == hp::Error::Ok && word_is(1, "a\nb\tc"), "newline and tab escapes");
    CHECK(parse_line("say \"unterminated") == hp::Error::Unterminated, "unterminated double quote");
    CHECK(parse_line("say 'unterminated") == hp::Error::Unterminated, "unterminated single quote");
    CHECK(parse_line("say trailing\\") == hp::Error::Unterminated, "trailing backslash");
    CHECK(parse_line("a | b | c") == hp::Error::Ok && g_parsed.pipe_count == 1 && g_parsed.pipes[0].stage_count == 3, "three stage pipe");
    CHECK(parse_line("a ; b => c !> d") == hp::Error::Ok && g_parsed.pipe_count == 4 && g_parsed.pipes[0].link == hp::Link::First &&
              g_parsed.pipes[1].link == hp::Link::Seq && g_parsed.pipes[2].link == hp::Link::Then && g_parsed.pipes[3].link == hp::Link::Else, "connectors");
    CHECK(parse_line("a;") == hp::Error::Ok && g_parsed.pipe_count == 1, "trailing semicolon");
    CHECK(parse_line("a =>") == hp::Error::EmptyCommand, "dangling then");
    CHECK(parse_line("a !>") == hp::Error::EmptyCommand, "dangling else");
    CHECK(parse_line("a |") == hp::Error::EmptyCommand, "dangling pipe");
    CHECK(parse_line("| a") == hp::Error::EmptyCommand, "leading pipe");
    CHECK(parse_line("; a") == hp::Error::EmptyCommand, "leading semicolon");
    CHECK(parse_line("=> a") == hp::Error::EmptyCommand, "leading then");
    CHECK(parse_line("a ;; b") == hp::Error::EmptyCommand, "double semicolon");
    CHECK(parse_line("a => => b") == hp::Error::EmptyCommand, "double then");
    CHECK(parse_line("where size => 3") == hp::Error::Ok && g_parsed.pipe_count == 2, "a standalone arrow always separates commands");
    CHECK(parse_line("where size '=>' 3") == hp::Error::Ok && g_parsed.pipe_count == 1 && g_parsed.word_count == 4, "a quoted arrow is a word");
    CHECK(parse_line("a >b =>c !=d x&y <z") == hp::Error::Ok && g_parsed.pipe_count == 1 && g_parsed.word_count == 6, "no other punctuation is special");
    CHECK(parse_line("say a # tail") == hp::Error::Ok && g_parsed.word_count == 2, "comment after words");
    CHECK(parse_line("say a#b") == hp::Error::Ok && word_is(1, "a#b"), "hash inside a word");
    CHECK(parse_line("say \"x # y\"") == hp::Error::Ok && word_is(1, "x # y"), "hash inside quotes");
    CHECK(parse_line("say a\x01") == hp::Error::BadText, "control byte in line");
    CHECK(parse_line("say \xFF") == hp::Error::BadText, "invalid UTF-8 in line");
    CHECK(parse_line("say \xC3") == hp::Error::BadText, "truncated UTF-8 in line");
    CHECK(parse_line("say \xEF\xBF\xBD") == hp::Error::Ok, "a literal replacement character is fine");
    CHECK(parse_line("say caf\xC3\xA9 \xE2\x82\xB9\xCE\xA9") == hp::Error::Ok && word_is(1, "caf\xC3\xA9") && word_is(2, "\xE2\x82\xB9\xCE\xA9"), "unicode words");
    char big[1100];
    for (int i = 0; i < 1099; ++i) big[i] = 'a';
    big[1099] = 0;
    CHECK(parse_line(big) == hp::Error::TooLong, "line over 1024 bytes");
    char many[600];
    u32 n = 0;
    for (int i = 0; i < 100; ++i) { many[n++] = 'x'; many[n++] = ' '; }
    many[n] = 0;
    CHECK(parse_line(many) == hp::Error::TooMany, "more than 96 words");
    char stages[200];
    n = 0;
    for (int i = 0; i < 17; ++i) { stages[n++] = 'x'; stages[n++] = ' '; stages[n++] = '|'; stages[n++] = ' '; }
    stages[n++] = 'x';
    stages[n] = 0;
    CHECK(parse_line(stages) == hp::Error::TooMany, "more than 16 stages");
    CHECK(parse_line("say @x @{y} @status @ \"@z\" '@w'") == hp::Error::Ok && g_parsed.word_count == 7, "value forms parse");
    CHECK(parse_line("say @{") == hp::Error::BadEscape, "broken brace value");
    CHECK(parse_line("say @{}") == hp::Error::BadEscape, "empty brace value");
    CHECK(parse_line("say me@example.com $HOME ${x}") == hp::Error::Ok && word_is(1, "me@example.com") && word_is(2, "$HOME") && word_is(3, "${x}"), "an @ inside a word and a dollar are plain text");
    CHECK(parse_line("say a\\@b \"c\\@d\"") == hp::Error::Ok && word_is(1, "a@b") && word_is(2, "c@d"), "escaped at sign");
    CHECK(parse_line("say *.txt a?c") == hp::Error::Ok && g_parsed.words[1].len == 5 && g_parsed.text(g_parsed.words[1])[0] == hp::MARK_STAR, "wildcards marked");
    CHECK(parse_line("say '*.txt' \"*.txt\" \\*") == hp::Error::Ok && g_parsed.text(g_parsed.words[1])[0] == '*' && g_parsed.text(g_parsed.words[2])[0] == '*' && g_parsed.text(g_parsed.words[3])[0] == '*', "quoted wildcards are literal");
}

static bool completes_to(const char* line, const char* insert, u32 count) {
    helm::Completion c;
    u32 cursor = static_cast<u32>(cstr::length(line));
    bool ok = helm::complete(*g_sh, line, cursor, &c);
    bool good = ok && same(c.insert, insert) && c.count == count;
    if (!good) printf("   complete '%s': ok=%d insert='%s' count=%u (want '%s' %u)\n", line, ok, ok ? c.insert : "", ok ? c.count : 0, insert, count);
    if (ok) helm::release(c);
    return good;
}

static void test_shell() {
    printf("-- shell on ram\n");
    CHECK(out_is("say hello", "hello\n"), "say");
    CHECK(out_is("say hi newline: no", "hi"), "an option after the words");
    CHECK(out_is("say newline: no hi", "hi"), "an option before the words");
    CHECK(out_is("say 'a  b'   \"c  d\"", "a  b c  d\n"), "quotes preserved");
    CHECK(out_is("say \"caf\\u{e9} \\u{20B9}\"", "caf\xC3\xA9 \xE2\x82\xB9\n"), "unicode escapes");
    CHECK(out_is("say caf\xC3\xA9 \xCE\xA9\xE2\x82\xB9", "caf\xC3\xA9 \xCE\xA9\xE2\x82\xB9\n"), "unicode passthrough");
    CHECK(out_is("bind name world; say hello @name @{name}s", "hello world worlds\n"), "bound values");
    CHECK(out_is("bind a 1 2 3; say \"[@a]\"", "[1 2 3]\n"), "bind joins values");
    CHECK(out_is("say '@name'", "@name\n"), "single quotes block expansion");
    CHECK(out_is("say me@example.com", "me@example.com\n"), "an @ inside a word is not a value");
    CHECK(out_is("say $HOME ${x}", "$HOME ${x}\n"), "a dollar means nothing here");
    CHECK(out_is("say @undefined-x", "-x\n"), "unbound value is empty");
    CHECK(out_is("unbind name; say [@name]", "[]\n"), "unbind");
    CHECK(out_is("bind star '*.txt'; say @star", "*.txt\n"), "a bound wildcard stays literal in say");
    CHECK(out_is("ok; say @status", "0\n"), "status zero");
    CHECK(out_is("fail; say @status", "1\n"), "status one");
    CHECK(out_is("nosuchcommand; say @status", "  unknown command: nosuchcommand\n  try help\n127\n"), "unknown command status");
    CHECK(out_has("bind status 5", "cannot be bound") && g_status == 1, "reserved name");
    CHECK(out_has("bind here x", "cannot be bound") && g_status == 1, "reserved name here");
    CHECK(out_is("ok => say yes", "yes\n"), "then runs");
    CHECK(out_is("fail => say yes", ""), "then skips");
    CHECK(out_is("fail !> say rescued", "rescued\n"), "else runs");
    CHECK(out_is("ok !> say never", ""), "else skips");
    CHECK(out_is("fail => say a !> say b", "b\n"), "then and else");
    CHECK(out_is("ok => fail !> say c", "c\n"), "else after a failed then");
    CHECK(out_is("say @here", "ram:/\n"), "here value");
    CHECK(out_has("say 'a", "unterminated quote") && g_status == 2, "syntax error status");

    CHECK(out_is("say hi | etch f.txt; beacon f.txt", "hi\n"), "etch and beacon");
    CHECK(out_is("say more | etch f.txt append: yes; beacon f.txt", "hi\nmore\n"), "append");
    CHECK(out_is("say over | etch f.txt; beacon f.txt", "over\n"), "overwrite");
    CHECK(out_is("say b | etch s.txt; say a | etch s.txt append: yes; say c | etch s.txt append: yes; beacon s.txt | order", "a\nb\nc\n"), "order text");
    CHECK(out_is("beacon s.txt | order down: yes", "c\nb\na\n"), "order down");
    CHECK(out_is("beacon s.txt | order | first 2", "a\nb\n"), "three stage pipe");
    CHECK(out_is("beacon s.txt | seek b | pick text", "b\n"), "seek then pick");
    CHECK(out_is("say x | beacon | etch piped.txt; beacon piped.txt", "x\n"), "pipe through beacon then etch");
    CHECK(out_has("beacon", "no input") && g_status == 1, "beacon without input");
    CHECK(out_has("etch f", "nothing to write") && g_status == 1, "etch without input");
    CHECK(out_has("say x | etch nodir/f", "cannot write nodir/f: not found") && g_status == 1, "etch into a missing directory");
    CHECK(out_has("say x | etch sys:/version", "read-only realm"), "etch into a read-only realm");
    CHECK(out_has("say x | etch", "usage: etch") && g_status == 2, "etch needs a file");
    CHECK(out_has("say x | etch append: maybe f", "yes or no") && g_status == 2, "bad yes/no value");

    CHECK(out_is("say \"caf\\u{e9} \\u{20B9}\" | etch \"caf\xC3\xA9 \xE2\x82\xB9.txt\"; beacon caf\xC3\xA9\\ \xE2\x82\xB9.txt", "caf\xC3\xA9 \xE2\x82\xB9\n"), "unicode file name end to end");
    CHECK(out_is("say 1 | etch \"\xCE\xA9mega\"; chart | where name ~ '*mega' | pick name", "\xCE\xA9mega\n"), "chart into where with unicode");
    CHECK(out_has("say 1 | etch \"bad\\u{202E}name\"", "invalid name"), "bidi override refused in names");

    sh("fade tree: yes missing: yes *");
    CHECK(out_is("spark a.txt b.txt c.log; chart | pick name", "a.txt\nb.txt\nc.log\n"), "spark and chart");
    CHECK(out_is("say *.txt", "a.txt b.txt\n"), "glob star");
    CHECK(out_is("say ?.log", "c.log\n"), "glob question mark");
    CHECK(out_is("say '*.txt'", "*.txt\n"), "quoted glob literal");
    CHECK(out_is("say *.zip", "  no match: *.zip\n") && g_status == 1, "glob without match is an error");
    CHECK(out_is("say x/*.txt", "  cannot expand x/*.txt: not found\n"), "glob in missing directory");
    CHECK(out_has("say */a", "wildcards are only allowed in the last path component"), "glob in directory part refused");
    CHECK(out_is("say ram:*", "ram:a.txt ram:b.txt ram:c.log\n"), "glob after a realm prefix");
    CHECK(out_is("plant parents: yes d/e; spark d/e/x1 d/e/x2; say d/e/x?", "d/e/x1 d/e/x2\n"), "glob in subdirectory");
    sh("spark caf\xC3\xA9 caff cafe");
    CHECK(out_is("say caf?", "cafe caff caf\xC3\xA9\n"), "question mark matches one character, not one byte");
    CHECK(out_is("say c*\xC3\xA9", "caf\xC3\xA9\n"), "star with unicode tail");
    sh("fade missing: yes caf*");

    CHECK(out_is("here", "ram:/\n"), "here");
    CHECK(out_is("go d; here", "ram:/d\n"), "go");
    CHECK(out_is("go e; here; go ..; go ..; here", "ram:/d/e\nram:/\n"), "go up");
    CHECK(out_is("go d; back; here", "ram:/\n"), "back");
    CHECK(out_is("back; here; back; here", "ram:/d\nram:/\n"), "back swaps with the last place");
    CHECK(out_has("go a.txt", "not a directory") && g_status == 1, "go into a file");
    CHECK(out_has("go nowhere", "not found"), "go into nothing");
    CHECK(out_is("go sys:; here; go", "sys:/\n"), "go to a realm");
    sh("go ram:/");
    CHECK(out_is("here", "ram:/\n"), "back in ram");

    CHECK(out_is("plant tmp; go tmp; spark inner; go ..; chart tmp | pick name", "inner\n"), "chart of a directory");
    CHECK(out_has("plant tmp", "already exists"), "plant twice");
    CHECK(out_has("fade tmp", "is not empty") && g_status == 1, "fade refuses full directories");
    CHECK(out_has("fade tree: yes tmp; chart | where name = tmp", "0 rows"), "fade tree");
    CHECK(out_is("fade missing: yes nothing-here; say @status", "0\n"), "fade missing: yes");
    CHECK(out_has("fade nothing-here", "not found") && g_status == 1, "fade of nothing");
    CHECK(out_has("fade tree: yes /", "not allowed"), "fading the root is refused");
    CHECK(out_has("fade tree: yes ram:/", "not allowed"), "fading a realm root is refused");
    CHECK(out_has("fade -r x", "cannot fade -r"), "dash words are plain paths");

    CHECK(out_is("say data | etch src; twin src dst; beacon dst", "data\n"), "twin");
    CHECK(out_has("twin src dst", "already exists; add replace: yes"), "twin refuses to overwrite");
    CHECK(out_is("say new | etch src; twin src dst replace: yes; beacon dst", "new\n"), "twin replace");
    CHECK(out_is("plant box; twin src box; beacon box/src", "new\n"), "twin into directory");
    CHECK(out_has("twin box box2", "is a directory; add tree: yes"), "twin refuses directories without tree");
    CHECK(out_is("twin tree: yes box box2; beacon box2/src", "new\n"), "twin tree");
    CHECK(out_is("twin tree: yes box box2; beacon box2/box/src", "new\n"), "twin tree into an existing directory nests the copy");
    CHECK(out_has("twin tree: yes box box2", "never merged"), "twin tree onto an existing copy is refused");
    CHECK(out_has("twin src a.txt b.txt", "must be an existing directory"), "several sources need a directory");
    CHECK(out_is("plant many; twin src dst a.txt many; chart many | tally | etch c.out; beacon c.out", "rows\n   3\n"), "several sources");
    CHECK(out_is("drift src moved; beacon moved", "new\n"), "drift");
    CHECK(out_has("beacon src", "not found"), "old name is gone");
    CHECK(out_has("drift moved dst", "already exists; add replace: yes"), "drift refuses to overwrite");
    CHECK(out_is("drift replace: yes moved dst; beacon dst", "new\n"), "drift replace");
    CHECK(out_is("drift dst box; beacon box/dst", "new\n"), "drift into directory");
    CHECK(out_is("drift box renamed; beacon renamed/dst", "new\n"), "drift a directory");
    CHECK(out_has("drift renamed renamed/inside", "not supported"), "drift a directory into itself");
    CHECK(out_has("twin onlyone", "usage: twin") && g_status == 2, "usage error status");

    CHECK(out_has("probe a.txt", "writable"), "probe file");
    CHECK(out_has("probe renamed", "dir"), "probe directory");
    CHECK(out_has("probe sys:/version", "read-only"), "probe read-only realm");
    CHECK(out_has("probe nothing", "cannot probe nothing: not found") && g_status == 1, "probe missing");
    CHECK(out_is("probe a.txt renamed | pick kind", "file\ndir\n"), "probe rows");
    CHECK(out_has("chart renamed", "dst"), "chart");
    CHECK(out_has("plant parents: yes t1/t2/t3; spark t1/t2/t3/leaf; outline depth: 2 t1", "t2") && !contains(g_out, "leaf"), "outline depth limit");
    CHECK(out_has("outline t1", "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 leaf"), "outline draws branches");
    CHECK(out_is("outline only: dir t1", "t1\n\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 t2/\n    \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 t3/\n2 directories\n"), "outline only dirs");
    CHECK(out_is("chart depth: 9 t1 | pick name", "t2\nt2/t3\nt2/t3/leaf\n"), "chart depth lists relative names");
    CHECK(out_is("chart depth: 9 only: dir t1 | pick name", "t2\nt2/t3\n"), "chart only: dir");
    CHECK(out_is("chart depth: 9 t1 | where name ~ *leaf | pick name", "t2/t3/leaf\n"), "where with an unquoted wildcard stays literal");
    CHECK(out_is("chart depth: 9 t1 | where kind = file | pick name", "t2/t3/leaf\n"), "where on a text column");
    CHECK(out_has("chart t1 | where nothing = 1", "no column 'nothing'") && g_status == 1, "where on a missing column");
    CHECK(out_has("chart", "name") && contains(g_out, "kind") && contains(g_out, "size") && contains(g_out, "changed") && contains(g_out, "rows"), "chart prints a table with a header and a count");
    CHECK(out_has("realms", "disk"), "realms");
    CHECK(out_has("realms", "snap"), "realms lists snap");
    CHECK(out_is("realms | where realm = sys | pick access", "read-only\n"), "realms rows are typed and filterable");

    CHECK(out_is("say \"one\\ntwo\\nthree\\nfour\" | etch lines; beacon lines | first 2", "one\ntwo\n"), "first");
    CHECK(out_is("beacon lines | last 2", "three\nfour\n"), "last");
    CHECK(out_is("beacon lines | last 10 | tally | etch c.out; beacon c.out", "rows\n   4\n"), "last more than available");
    CHECK(out_is("seek o lines | pick text", "one\ntwo\nfour\n"), "seek");
    CHECK(out_is("seek not: yes o lines | pick text", "three\n"), "seek not");
    CHECK(out_is("seek thr lines | pick line | etch c.out; beacon c.out", "line\n   3\n"), "seek reports line numbers");
    CHECK(out_is("seek fold: yes ONE lines | pick text", "one\n"), "seek fold");
    CHECK(out_is("seek count: yes o lines | etch c.out; beacon c.out", "matches\n      3\n"), "seek count");
    CHECK(out_has("seek zzz lines", "0 rows") && g_status == 1, "seek with no match");
    CHECK(out_is("say \"caf\\u{e9}\" | seek \"\\u{e9}\" | pick text", "caf\xC3\xA9\n"), "seek a Unicode needle");
    CHECK(out_is("say one | etch other.txt; seek o lines other.txt | pick file | distinct", "lines\nother.txt\n"), "several files add a file column");
    CHECK(out_has("seek", "usage: seek") && g_status == 2, "seek needs text");
    CHECK(out_has("tally lines | etch c.out; beacon c.out", "19"), "tally a file");
    CHECK(out_is("say \"\\u{20B9}\\u{20B9}\" | tally | etch c.out; beacon c.out", "lines  words  chars  bytes\n    1      1      3      7\n"), "tally text counts characters and bytes");
    CHECK(out_is("say \"10\\n9\\n100\\n-5\" | order number: yes", "-5\n9\n10\n100\n"), "order number: yes");
    CHECK(out_is("say \"b\\na\\nb\\nc\\nb\" | order | distinct", "a\nb\nc\n"), "order then distinct");
    CHECK(out_is("say \"a\\na\\nb\\na\" | distinct", "a\nb\n"), "distinct keeps the first of every value, adjacent or not");
    CHECK(out_is("say \"a\\nb\\na\" | tally by: text | where text = a | pick rows | etch c.out; beacon c.out", "rows\n   2\n"), "tally by a column of text");
    CHECK(out_is("say AB | prism", "00000000  41 42 0a                                          AB.\n"), "prism");
    CHECK(out_is("say AB | prism from: 1 take: 1", "00000001  42                                                B\n"), "prism window");
    CHECK(out_is("beacon numbered: yes lines | first 1", "     1  one\n"), "beacon numbered");

    CHECK(out_is("say \"say from script\\nbind v 7\\nsay v=\\@v\" | etch s.hm; launch s.hm", "from script\nv=7\n"), "launch a script");
    CHECK(out_has("say \"say one\\nfail\\nsay two\" | etch bad.hm; launch bad.hm", "stopped at line 2") && !contains(g_out, "two"), "script stops on failure");
    CHECK(out_has("launch keep: yes bad.hm", "two"), "launch keep: yes continues");
    CHECK(out_has("say \"launch loop.hm\" | etch loop.hm; launch loop.hm", "nested too deeply"), "script recursion is bounded");

    CHECK(out_has("help", "files and realms"), "help lists tables");
    CHECK(out_has("help chart", "chart [path]  depth: N"), "help for one command");
    CHECK(out_has("help syntax", "options") && contains(g_out, "=>") && contains(g_out, "@name"), "help syntax");
    CHECK(out_has("help nosuch", "no such command") && g_status == 1, "help for nothing");
    CHECK(out_has("chart dpth: 2", "does not take the option dpth:") && contains(g_out, "depth: only:") && g_status == 1, "unknown option names the valid ones");
    CHECK(out_has("go foo:", "go does not take the option foo:") && contains(g_out, "it takes none"), "a command without options says so");
    CHECK(out_has("chart depth:", "depth: needs a value") && g_status == 1, "option without a value");
    CHECK(out_has("chart depth: 2 depth: 3", "given twice"), "option given twice");
    CHECK(out_has("chart depth: 0", "depth: takes a number from 1 to 16") && g_status == 2, "option range");
    CHECK(out_has("chart depth: many", "takes a whole number") && g_status == 2, "option number");
    CHECK(out_has("chart only: both", "only: takes dir or file") && g_status == 2, "option choice");
    CHECK(out_is("say 'hello:'", "hello:\n"), "a quoted key word is plain text");
    CHECK(out_has("say hello:", "does not take the option hello:"), "an unquoted unknown key word is refused");
    CHECK(out_has("cat x", "unknown command: cat"), "unix names are gone");

    CHECK(out_is("beacon sys:/version", "Aphelion\n"), "sys realm readable");
}

static void test_completion() {
    printf("-- completion\n");
    sh("fade tree: yes missing: yes *; spark alpha.txt alps beta; plant apple; spark \"with space.txt\"");
    CHECK(completes_to("bea", "beacon ", 1), "command prefix");
    CHECK(completes_to("go al", "alp", 2), "two files with a common prefix");
    CHECK(completes_to("go app", "apple/", 1), "unique directory gets a slash");
    CHECK(completes_to("beacon be", "beta ", 1), "unique file gets a space");
    CHECK(completes_to("beacon alpha.t", "alpha.txt ", 1), "unique extension");
    CHECK(completes_to("beacon with", "with\\ space.txt ", 1), "spaces are escaped");
    CHECK(completes_to("beacon \"with", "\"with space.txt\" ", 1), "completion inside quotes");
    CHECK(completes_to("say hi | pri", "prism ", 1), "command after a pipe");
    CHECK(completes_to("say x; unb", "unbind ", 1), "command after a semicolon");
    CHECK(completes_to("say x => bin", "bind ", 1), "command after then");
    CHECK(completes_to("say x !> unb", "unbind ", 1), "command after else");
    CHECK(completes_to("chart depth: 2 ap", "apple/", 1), "path after an option");
    CHECK(completes_to("beacon sys:/ver", "sys:/version ", 1), "path in another realm");
    CHECK(completes_to("beacon sys:", "sys:version ", 1), "completing right after a realm colon");
    CHECK(completes_to("go sn", "snap:/", 1), "realm name completes with a slash");
    { helm::Completion e; CHECK(!helm::complete(*g_sh, "beacon apple/", 13, &e), "empty directory has nothing to offer"); }
    helm::Completion c;
    CHECK(!helm::complete(*g_sh, "beacon zzz", 10, &c), "no candidates");
    sh("spark caf\xC3\xA9 cafz");
    CHECK(completes_to("beacon caf", "caf", 2), "common prefix stops before a multibyte split");
    { helm::Completion e; bool ok = helm::complete(*g_sh, "beacon caf\xC3", 11, &e); CHECK(ok && same(e.insert, "caf\xC3\xA9 ") && e.count == 1, "partial UTF-8 still completes"); if (ok) helm::release(e); }
    sh("spark 'at@sign' 'hash#tag'");
    CHECK(completes_to("beacon at", "at\\@sign ", 1), "an at sign in a name is escaped");
    sh("fade tree: yes missing: yes *");
}

static void test_lens_location() {
    printf("-- lens-backed location\n");
    CHECK(out_is("plant parents: yes ram:/lt/a/b; go ram:/lt/a/b; here", "ram:/lt/a/b\n"), "here after go into a nested directory");
    CHECK(out_is("go ../..; here", "ram:/lt\n"), "go .. walks up the parent lenses");
    CHECK(out_is("say hi | etch a/f; beacon a/f", "hi\n"), "relative path from the current lens");
    CHECK(out_is("go a/b; beacon ../f; back; here", "hi\nram:/lt\n"), "relative read through .. and back");
    CHECK(out_is("go ../../../..; here", "ram:/\n"), "go above the root stops at the root");
    CHECK(out_has("go ram:/lt; spark ./c:d; chart", "c:d"), "a file name with a colon");
    CHECK(out_has("probe sys:/version", "read-only"), "a probe reports its read-only realm");
    CHECK(out_has("say x | etch sys:/version", "read-only"), "a write to a probe is refused");
    CHECK(out_is("go ram:/lt/a; drift ../a ../a2; here", "ram:/lt/a2\n"), "moving the current directory: the prompt follows it");
    CHECK(out_is("beacon f", "hi\n"), "and relative paths still work");
    CHECK(out_is("fade tree: yes ../a2; here", "ram:/lt\n"), "removing the current directory climbs to what survives");
    sh("go ram:/; fade tree: yes ram:/lt");
}

static u32 g_poll_calls;
static bool poll_always(void*) { ++g_poll_calls; return true; }

static void test_interrupt_and_limits() {
    printf("-- interrupt and limits\n");
    static char big[200 * 65];
    for (u32 i = 0; i < 200; ++i) mem::copy(big + i * 65, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n", 65);
    CHECK(lens::text::write_file(g_sh->cursor, "big.txt", big, sizeof(big)) == nebula::Status::Ok, "write big file");
    g_sh->interrupt_poll = &poll_always;
    g_poll_calls = 0;
    sh("seek 0 big.txt | tally");
    CHECK(g_status == 130 && contains(g_out, "interrupted") && g_poll_calls >= 1, "interrupt stops a pipeline and reports it");
    CHECK(!contains(g_out, "rows"), "nothing downstream ran after the interrupt");
    sh("beacon big.txt | last 200");
    CHECK(g_status == 130, "interrupt during a row command");
    sh("outline .");
    CHECK(g_status == 130 || g_status == 0, "outline notices the poll or has nothing to walk");
    g_sh->interrupt_poll = nullptr;
    g_sh->aborted = false;
    CHECK(out_is("say ok", "ok\n"), "shell works after an interrupt");
    sh("fade big.txt");

    sh("say x | etch a");
    char line[1024];
    u32 n = 0;
    const char* pre = "say ";
    for (u32 i = 0; pre[i]; ++i) line[n++] = pre[i];
    for (u32 i = 0; i < 520; ++i) line[n++] = 'z';
    line[n] = 0;
    sh(line);
    CHECK(contains(g_out, "too long") || g_status == 0, "very long word handled");

    sh("bind v1 1; bind v2 2; bind v3 3");
    char vline[64];
    bool all = true;
    for (int i = 0; i < 30; ++i) {
        vline[0] = 'b'; vline[1] = 'i'; vline[2] = 'n'; vline[3] = 'd'; vline[4] = ' '; vline[5] = 'q';
        vline[6] = static_cast<char>('a' + i % 26);
        vline[7] = static_cast<char>('a' + i / 26);
        vline[8] = ' '; vline[9] = '1'; vline[10] = 0;
        sh(vline);
        if (g_status != 0) all = false;
    }
    CHECK(!all, "bound name table is bounded");
    CHECK(out_has("bind 1bad x", "cannot bind"), "bad bound name");
    CHECK(out_is("bind | where name = v2 | pick value", "2\n"), "bind lists names as rows");
    sh("unbind v1 v2 v3");
    for (int i = 0; i < 30; ++i) {
        char u[32] = "unbind q";
        u[8] = static_cast<char>('a' + i % 26);
        u[9] = static_cast<char>('a' + i / 26);
        u[10] = 0;
        sh(u);
    }
}

static void test_disk() {
    printf("-- disk and epochs from the shell\n");
    sh("go disk:/; fade tree: yes missing: yes *");
    CHECK(out_is("here", "disk:/\n"), "go disk");
    CHECK(out_is("plant parents: yes proj/src; say v1 | etch proj/src/main; say n1 | etch notes", ""), "build a tree on disk");
    CHECK(out_has("epoch take", "epoch 1 taken"), "take an epoch");
    CHECK(out_has("epoch list", "snap:/1"), "list epochs");
    CHECK(out_has("epoch", "1 row"), "epoch lists by default");
    CHECK(out_is("say v2 | etch proj/src/main; beacon snap:/1/proj/src/main", "v1\n"), "an epoch keeps the old contents");
    CHECK(out_has("chart snap:/ | pick name", "1"), "epochs are listed as directories");
    CHECK(out_is("twin tree: yes snap:/1/proj restored; beacon restored/src/main", "v1\n"), "restore a tree from an epoch");
    CHECK(out_has("fade snap:/1/notes", "read-only") && g_status == 1, "epochs cannot be modified");
    CHECK(out_is("twin notes ram:/n.copy; beacon ram:/n.copy", "n1\n"), "twin disk to ram");
    CHECK(out_is("drift ram:/n.copy disk:/n.moved; beacon disk:/n.moved", "n1\n"), "drift across realms");
    CHECK(out_has("chart ram:/ | where name = n.copy", "0 rows"), "the drifted file left ram");
    CHECK(out_has("audit", "volume is consistent"), "audit");
    CHECK(out_has("audit deep: yes", "volume is consistent"), "deep audit");
    CHECK(out_is("settle; say @status", "0\n"), "settle");
    CHECK(out_has("epoch drop 99", "not found") && g_status == 1, "drop a missing epoch");
    CHECK(out_has("epoch drop 1; epoch", "0 rows"), "drop an epoch");
    CHECK(out_has("epoch bogus", "usage: epoch") && g_status == 2, "epoch usage");
    nebula::disk_set_mounted(false);
    CHECK(out_has("chart disk:/", "not available"), "unmounted disk reports unavailable");
    CHECK(out_has("epoch take", "no disk volume"), "epoch without a volume");
    nebula::disk_set_mounted(true);
    CHECK(out_has("kindle erase", "refusing to erase") && g_status == 1, "kindle refuses a mounted volume");
    nebula::disk_set_mounted(false);
    nebula::disk_set_confirmed_only(true);
    CHECK(out_has("kindle erase", "typed confirmation") && g_status == 1, "kindle refuses when the disk needs a typed confirmation");
    nebula::disk_set_confirmed_only(false);
    CHECK(out_has("kindle", "usage: kindle erase") && g_status == 2, "kindle needs the erase word");
    CHECK(out_has("kindle wipe", "usage: kindle erase") && g_status == 2, "kindle rejects other words");
    mem::fill(g_disk, 0xAB, 64 * 512);
    CHECK(out_has("kindle erase", "refusing to touch it") && g_status == 1, "kindle refuses a foreign disk");
    CHECK(g_disk[0] == 0xAB && g_disk[64 * 512 - 1] == 0xAB, "the refused disk was left untouched");
    mem::zero(g_disk, 64 * 512);
    CHECK(out_has("kindle erase", "new Stellar volume created") && g_status == 0, "kindle formats a blank disk");
    CHECK(nebula::disk_is_mounted(), "kindle leaves the volume mounted");
    CHECK(out_has("chart disk:/", "0 rows"), "the new volume is empty");
    CHECK(out_is("say fresh | etch disk:/f; beacon disk:/f", "fresh\n"), "the new volume is writable");
    CHECK(out_has("audit", "volume is consistent"), "the new volume audits clean");
    sh("fade tree: yes missing: yes *; go ram:/");
}

class TestConsole final : public lens::Stream {
public:
    char out[64];
    u32 out_len = 0;
    const char* name() const override { return "console"; }
    lens::Result<usize> read(lens::Bytes) override { return lens::Result<usize>::fail(lens::Status::NotReady); }
    lens::Result<usize> write(lens::ConstBytes src) override {
        for (usize i = 0; i < src.len && out_len + 1 < sizeof(out); ++i) out[out_len++] = static_cast<char>(src.ptr[i]);
        out[out_len] = 0;
        return lens::Result<usize>::success(src.len);
    }
    lens::Readiness ready() override { return lens::Ready::Writable; }
    void retain() override {}
    void release() override {}
};
static TestConsole g_console;

static void test_console_lens() {
    printf("-- console lens\n");
    lens::Domain& d = g_sh->cursor.domain;
    lens::Lens con = lens::text::console(g_sh->cursor);
    CHECK(!con.is_null(), "the shell holds a console lens");
    CHECK(lens::kind_of(d, con).value() == lens::NodeKind::Stream, "it is a stream");
    CHECK(lens::rights_of(d, con).value() == (lens::Right::Read | lens::Right::Write), "with read and write, and no authority to hand it on");
    lens::ConstBytes hi = lens::text_bytes("hi");
    CHECK(lens::write(d, con, 0, hi).ok() && same(g_console.out, "hi"), "a write through the lens reaches the console");
    char b[4];
    CHECK(lens::read(d, con, 0, lens::Bytes(reinterpret_cast<u8*>(b), 4)).status() == lens::Status::NotReady, "a read with no input is not ready");
    lens::Wait w{con, lens::Ready::Writable, {}};
    CHECK(lens::await(d, &w, 1, 0).ok() && w.got.has(lens::Ready::Writable), "await sees it writable");
    lens::Result<lens::Lens> narrowed = lens::narrow(d, con, lens::Right::Write);
    CHECK(narrowed.ok() && lens::read(d, narrowed.value(), 0, lens::Bytes(reinterpret_cast<u8*>(b), 4)).status() == lens::Status::Denied, "a narrowed copy cannot read");
    CHECK(lens::close(d, narrowed.value()).ok(), "and closes");
}

static void test_row_values() {
    printf("-- row values\n");
    u64 v = 0;
    CHECK(hr::parse_value(hr::Kind::Int, "-42", &v) && static_cast<i64>(v) == -42, "negative int");
    CHECK(!hr::parse_value(hr::Kind::Int, "4x", &v) && !hr::parse_value(hr::Kind::Int, "", &v) && !hr::parse_value(hr::Kind::Int, "-", &v), "bad ints");
    CHECK(hr::parse_value(hr::Kind::Size, "2KiB", &v) && v == 2048, "KiB size");
    CHECK(hr::parse_value(hr::Kind::Size, "2kb", &v) && v == 2048, "kb is binary too");
    CHECK(hr::parse_value(hr::Kind::Size, "3M", &v) && v == 3ull << 20, "M size");
    CHECK(hr::parse_value(hr::Kind::Size, "17", &v) && v == 17, "bare size");
    CHECK(hr::parse_value(hr::Kind::Size, "17B", &v) && v == 17, "B size");
    CHECK(!hr::parse_value(hr::Kind::Size, "2XB", &v) && !hr::parse_value(hr::Kind::Size, "KiB", &v), "bad sizes");
    CHECK(!hr::parse_value(hr::Kind::Size, "99999999999TiB", &v), "size overflow refused");
    CHECK(hr::parse_value(hr::Kind::Time, "2026-10-01", &v) && v == 1790812800000ull, "date");
    u64 w = 0;
    CHECK(hr::parse_value(hr::Kind::Time, "2026-10-01 12:30", &w) && w == v + (12 * 3600 + 30 * 60) * 1000ull, "date and minute");
    CHECK(hr::parse_value(hr::Kind::Time, "2026-10-01T12:30:15", &w) && w == v + (12 * 3600 + 30 * 60 + 15) * 1000ull, "date, minute and second");
    CHECK(!hr::parse_value(hr::Kind::Time, "2026-10-01 12", &w), "an hour without a minute is refused");
    CHECK(!hr::parse_value(hr::Kind::Time, "2026-13-01", &w) && !hr::parse_value(hr::Kind::Time, "2026-02-30", &w) && !hr::parse_value(hr::Kind::Time, "1969-12-31", &w), "impossible dates");
    CHECK(!hr::parse_value(hr::Kind::Time, "2026-10", &w) && !hr::parse_value(hr::Kind::Time, "tomorrow", &w), "partial dates");
    CHECK(hr::parse_value(hr::Kind::Flag, "yes", &v) && v == 1 && hr::parse_value(hr::Kind::Flag, "Off", &v) && v == 0, "flags");
    CHECK(!hr::parse_value(hr::Kind::Flag, "maybe", &v), "bad flag");

    hr::Table t;
    hr::begin(t);
    CHECK(hr::add_column(t, "name", hr::Kind::Text) && hr::add_column(t, "n", hr::Kind::Int) && hr::add_column(t, "size", hr::Kind::Size), "columns");
    const char* names[] = {"b", "a", "c", "a"};
    i64 nums[] = {3, -1, 2, 7};
    for (int i = 0; i < 4; ++i) {
        u32 r = hr::add_row(t);
        hr::set_text(t, r, 0, names[i]);
        hr::set_num(t, r, 1, static_cast<u64>(nums[i]));
        if (i == 1) hr::set_null(t, r, 2); else hr::set_num(t, r, 2, 100u * (i + 1));
    }
    CHECK(!hr::add_column(t, "late", hr::Kind::Text), "no columns after rows");
    hr::Table sorted;
    u32 by_n = 1;
    CHECK(hr::sort_rows(sorted, t, &by_n, 1, false, false), "sort");
    CHECK(static_cast<i64>(hr::num_at(sorted, 0, 1)) == -1 && static_cast<i64>(hr::num_at(sorted, 3, 1)) == 7, "ints sort signed");
    hr::Table down;
    u32 by_size = 2;
    CHECK(hr::sort_rows(down, t, &by_size, 1, true, false), "sort down");
    CHECK(hr::is_null(down, 3, 2) && hr::num_at(down, 0, 2) == 400, "nulls sort last even descending");
    hr::Table stable;
    u32 by_name = 0;
    CHECK(hr::sort_rows(stable, t, &by_name, 1, false, false), "sort by text");
    CHECK(static_cast<i64>(hr::num_at(stable, 0, 1)) == -1 && static_cast<i64>(hr::num_at(stable, 1, 1)) == 7, "the sort is stable");
    char cell[64];
    hr::format_cell(t, 0, 2, cell, sizeof(cell));
    CHECK(same(cell, "100 B"), "size formatting");
    hr::format_cell(t, 1, 2, cell, sizeof(cell));
    CHECK(same(cell, "-"), "null formatting");
    hr::set_num(t, 0, 2, 1536);
    hr::format_cell(t, 0, 2, cell, sizeof(cell));
    CHECK(same(cell, "1.5 KiB"), "KiB formatting");
    hr::release(t);
    hr::release(sorted);
    hr::release(down);
    hr::release(stable);

    hr::Table text;
    const char raw[] = "one\ntwo\n\nlast";
    CHECK(hr::from_text(text, reinterpret_cast<const u8*>(raw), sizeof(raw) - 1) && text.rows == 4 && text.body == 0, "text becomes one row per line");
    u32 n = 0;
    CHECK(same(hr::text_at(text, 2, 0, &n), "") && n == 0 && same(hr::text_at(text, 3, 0, &n), "last"), "empty and final lines");
    hr::release(text);

    hr::Table limited;
    hr::begin(limited);
    hr::add_column(limited, "x", hr::Kind::Text);
    static char chunk[1 << 16];
    for (u32 i = 0; i < sizeof(chunk); ++i) chunk[i] = 'x';
    bool refused = false;
    for (u32 i = 0; i < 200 && !refused; ++i) {
        u32 r = hr::add_row(limited);
        if (r == ~0u) { refused = true; break; }
        hr::set_text(limited, r, 0, chunk, sizeof(chunk));
        if (limited.failed) refused = true;
    }
    CHECK(refused && limited.failed, "a table stops growing at its byte limit");
    hr::release(limited);
}

static void test_typed_rows() {
    printf("-- typed rows\n");
    sh("go ram:/; fade tree: yes missing: yes rt; plant rt; go rt");
    static char two_k[2000];
    static char five_k[5000];
    for (u32 i = 0; i < sizeof(two_k); ++i) two_k[i] = 'a';
    for (u32 i = 0; i < sizeof(five_k); ++i) five_k[i] = 'b';
    CHECK(lens::text::write_file(g_sh->cursor, "m2k", two_k, sizeof(two_k)) == nebula::Status::Ok, "write m2k");
    CHECK(lens::text::write_file(g_sh->cursor, "m5k", five_k, sizeof(five_k)) == nebula::Status::Ok, "write m5k");
    CHECK(lens::text::write_file(g_sh->cursor, "tiny", "abc", 3) == nebula::Status::Ok, "write tiny");
    CHECK(out_is("plant sub", ""), "make a directory");

    CHECK(out_is("chart | pick name", "sub\nm2k\nm5k\ntiny\n"), "chart lists names");
    CHECK(out_is("chart | where size > 1KiB | pick name", "m2k\nm5k\n"), "sizes compare as sizes");
    CHECK(out_is("chart | where size >= 5000 | pick name", "m5k\n"), "plain numbers are bytes");
    CHECK(out_is("chart | where size < 10 | pick name", "tiny\n"), "directories have no size and never match");
    CHECK(out_is("chart | where size gt 1KiB | where size le 2000 | pick name", "m2k\n"), "word operators and chained where");
    CHECK(out_is("chart | where kind = dir | pick name", "sub\n"), "where on kind");
    CHECK(out_is("chart | where kind != dir | tally | etch ram:/c.out; beacon ram:/c.out", "rows\n   3\n"), "tally counts rows");
    CHECK(out_is("chart | order size down: yes | first 1 | pick name", "m5k\n"), "order a size column down");
    CHECK(out_is("chart | order size | first 1 | pick name", "tiny\n"), "order a size column up");
    CHECK(out_is("chart | order size down: yes | last 1 | pick name", "sub\n"), "nulls are always last");
    CHECK(out_is("chart | order name down: yes | first 1 | pick name", "tiny\n"), "order text down");
    CHECK(out_is("chart | where changed > 2000-01-01 | tally | etch ram:/c.out; beacon ram:/c.out", "rows\n   4\n"), "times compare as times");
    CHECK(out_has("chart | where changed < 2000-01-01", "0 rows"), "an empty result says so");
    CHECK(out_is("chart | where changed > '2000-01-01 00:00' | where changed < 2100-01-01 | tally | etch ram:/c.out; beacon ram:/c.out", "rows\n   4\n"), "a time with a clock part");
    CHECK(out_is("chart | tally by: kind | where kind = file | pick rows | etch ram:/c.out; beacon ram:/c.out", "rows\n   3\n"), "tally by a column");
    CHECK(out_is("chart | pick name | seek m | pick text", "m2k\nm5k\n"), "rows flatten into text for a text command");
    CHECK(out_is("chart | pick name | beacon", "sub\nm2k\nm5k\ntiny\n"), "beacon of rows prints the name column");
    CHECK(out_is("chart | etch ram:/listing; beacon ram:/listing", "sub\nm2k\nm5k\ntiny\n"), "etch of rows writes the name column");
    CHECK(out_is("chart | where name ~ 'm*' | pick name size | pick name", "m2k\nm5k\n"), "pick then pick");
    CHECK(out_is("chart | where name ~ m* | pick name", "m2k\nm5k\n"), "unquoted wildcard in where is literal");
    CHECK(out_is("chart | distinct kind | pick kind", "dir\nfile\n"), "distinct keeps the first row of each value");
    CHECK(out_has("chart | where size > 1XB", "not a valid size") && g_status == 2, "bad size value");
    CHECK(out_has("chart | where changed > yesterday", "not a valid time") && g_status == 2, "bad time value");
    CHECK(out_has("chart | where size ~ 1*", "text columns only") && g_status == 2, "glob on a number");
    CHECK(out_has("chart | where nothing = 1", "no column 'nothing'") && g_status == 1, "missing column lists the real ones");
    CHECK(contains(g_out, "name kind size changed"), "and names them");
    CHECK(out_has("chart | where size", "usage: where") && g_status == 2, "where needs three words");
    CHECK(out_has("chart | where size between 1", "usage: where") && g_status == 2, "unknown operator");
    CHECK(out_has("where size > 1", "no rows") && g_status == 1, "where with nothing piped in");
    CHECK(out_has("chart | first x", "usage: first") && g_status == 2, "first needs a number");
    CHECK(out_has("chart | pick", "usage: pick") && g_status == 2, "pick needs columns");
    CHECK(out_has("chart | pick nothing", "no column 'nothing'") && g_status == 1, "pick of a missing column");

    const char* shown = sh("chart");
    CHECK(contains(shown, "name") && contains(shown, "kind") && contains(shown, "size") && contains(shown, "changed"), "a table has a header");
    CHECK(contains(shown, "1.9 KiB") && contains(shown, "4.8 KiB") && contains(shown, "3 B"), "sizes are readable");
    CHECK(contains(shown, "sub/") && contains(shown, "4 rows"), "directories are marked and rows counted");
    printf("%s", shown);
    shown = sh("chart | where size > 1KiB");
    CHECK(contains(shown, "2 rows") && !contains(shown, "tiny"), "a filtered table counts its rows");
    sh("go ram:/; fade tree: yes rt");
}

int main() {
    g_sectors = 131072;
    g_disk = static_cast<u8*>(calloc(g_sectors, 512));
    stellar::init(0);
    CHECK(stellar::format(g_sectors), "format");
    nebula::disk_set_mounted(true);
    CHECK(nebula::register_realm(nebula::ram_realm()), "register ram");
    CHECK(nebula::register_realm(&g_fake_sys), "register sys stand-in");
    CHECK(nebula::register_realm(nebula::disk_realm()), "register disk");
    CHECK(nebula::register_realm(nebula::snap_realm()), "register snap");
    helm::register_builtin_commands();
    helm::register_fs_commands();
    helm::register_text_commands();
    helm::register_row_commands();

    CHECK(lens::mint_boot_roots().ok(), "mint the boot roots");
    CHECK(lens::mint_boot_console(&g_console).ok(), "mint the console");
    lens::seal();
    static helm::Shell shell;
    g_sh = &shell;
    helm::init(shell, helm::Sink{&sink_write, nullptr}, false, 40, "ram");

    test_parser();
    test_row_values();
    test_shell();
    test_completion();
    test_lens_location();
    test_interrupt_and_limits();
    test_disk();
    test_console_lens();
    test_typed_rows();
    helm::shutdown(shell);

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
