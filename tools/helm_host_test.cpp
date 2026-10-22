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
#include <cosmos/helm_parse.hpp>

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
bool read_sector(u64 s, void* b) { if (s >= g_sectors) return false; __builtin_memcpy(b, g_disk + s * 512, 512); return true; }
bool write_sector(u64 s, const void* b) { if (s >= g_sectors) return false; __builtin_memcpy(g_disk + s * 512, b, 512); return true; }
bool read_sectors(u64 s, u64 n, void* b) { if (s + n > g_sectors) return false; __builtin_memcpy(b, g_disk + s * 512, n * 512); return true; }
bool write_sectors(u64 s, u64 n, const void* b) { if (s + n > g_sectors) return false; __builtin_memcpy(g_disk + s * 512, b, n * 512); return true; }
u64 capacity_sectors() { return g_sectors; }
bool flush() { return true; }
bool present() { return true; }
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

static u64 g_clock = 1800000000000ull;
namespace clock { u64 now_ms() { return g_clock += 61000; } }

namespace serial {
void printf(const char*, ...) {}
void writeln(const char*) {}
}

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, msg) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL: %s  (line %d)\n", msg, __LINE__); } } while (0)

static bool same(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static bool contains(const char* hay, const char* needle) {
    u32 n = __builtin_strlen(needle);
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
        u64 n = __builtin_strlen(t);
        if (n > cap) n = cap;
        __builtin_memcpy(buf, t, n);
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
    CHECK(parse_line("ls -l docs") == hp::Error::Ok && g_parsed.word_count == 3 && word_is(0, "ls") && word_is(2, "docs"), "plain words");
    CHECK(parse_line("echo 'a  b' \"c  d\"") == hp::Error::Ok && g_parsed.word_count == 3 && word_is(1, "a  b") && word_is(2, "c  d"), "quotes keep spaces");
    CHECK(parse_line("echo a\\ b") == hp::Error::Ok && g_parsed.word_count == 2 && word_is(1, "a b"), "escaped space");
    CHECK(parse_line("echo ''") == hp::Error::Ok && g_parsed.word_count == 2 && g_parsed.words[1].len == 0, "empty quoted word");
    CHECK(parse_line("echo \"\\u{20B9} \\u{1F600}\"") == hp::Error::Ok && word_is(1, "\xE2\x82\xB9 \xF0\x9F\x98\x80"), "unicode escapes");
    CHECK(parse_line("echo \"\\u{D800}\"") == hp::Error::BadEscape, "surrogate escape refused");
    CHECK(parse_line("echo \"\\u{110000}\"") == hp::Error::BadEscape, "out of range escape refused");
    CHECK(parse_line("echo \"\\u{1B}\"") == hp::Error::BadEscape, "control escape refused");
    CHECK(parse_line("echo \"\\u20B9\"") == hp::Error::BadEscape, "escape without braces refused");
    CHECK(parse_line("echo \"a\\nb\\tc\"") == hp::Error::Ok && word_is(1, "a\nb\tc"), "newline and tab escapes");
    CHECK(parse_line("echo \"unterminated") == hp::Error::Unterminated, "unterminated double quote");
    CHECK(parse_line("echo 'unterminated") == hp::Error::Unterminated, "unterminated single quote");
    CHECK(parse_line("echo trailing\\") == hp::Error::Unterminated, "trailing backslash");
    CHECK(parse_line("a | b | c") == hp::Error::Ok && g_parsed.pipe_count == 1 && g_parsed.pipes[0].stage_count == 3, "three stage pipe");
    CHECK(parse_line("a ; b && c || d") == hp::Error::Ok && g_parsed.pipe_count == 4 &&
              g_parsed.pipes[1].link == hp::Link::Seq && g_parsed.pipes[2].link == hp::Link::And && g_parsed.pipes[3].link == hp::Link::Or, "connectors");
    CHECK(parse_line("a;") == hp::Error::Ok && g_parsed.pipe_count == 1, "trailing semicolon");
    CHECK(parse_line("a &&") == hp::Error::EmptyCommand, "dangling and");
    CHECK(parse_line("a ||") == hp::Error::EmptyCommand, "dangling or");
    CHECK(parse_line("a |") == hp::Error::EmptyCommand, "dangling pipe");
    CHECK(parse_line("| a") == hp::Error::EmptyCommand, "leading pipe");
    CHECK(parse_line("; a") == hp::Error::EmptyCommand, "leading semicolon");
    CHECK(parse_line("a ;; b") == hp::Error::EmptyCommand, "double semicolon");
    CHECK(parse_line("a & b") == hp::Error::BadOperator, "single ampersand");
    CHECK(parse_line("a >") == hp::Error::BadRedirect, "redirect without target");
    CHECK(parse_line("a > > b") == hp::Error::BadRedirect, "redirect into operator");
    CHECK(parse_line("a > x > y") == hp::Error::BadRedirect, "two output redirects");
    CHECK(parse_line("> x") == hp::Error::EmptyCommand, "redirect without command");
    CHECK(parse_line("a > x < y") == hp::Error::Ok, "input and output redirect");
    CHECK(parse_line("a >> x") == hp::Error::Ok && g_parsed.words[1].role == hp::Role::Append, "append role");
    CHECK(parse_line("echo a # tail") == hp::Error::Ok && g_parsed.word_count == 2, "comment after words");
    CHECK(parse_line("echo a#b") == hp::Error::Ok && word_is(1, "a#b"), "hash inside a word");
    CHECK(parse_line("echo \"x # y\"") == hp::Error::Ok && word_is(1, "x # y"), "hash inside quotes");
    CHECK(parse_line("echo a\x01") == hp::Error::BadText, "control byte in line");
    CHECK(parse_line("echo \xFF") == hp::Error::BadText, "invalid UTF-8 in line");
    CHECK(parse_line("echo \xC3") == hp::Error::BadText, "truncated UTF-8 in line");
    CHECK(parse_line("echo \xEF\xBF\xBD") == hp::Error::Ok, "a literal replacement character is fine");
    CHECK(parse_line("echo caf\xC3\xA9 \xE2\x82\xB9\xCE\xA9") == hp::Error::Ok && word_is(1, "caf\xC3\xA9") && word_is(2, "\xE2\x82\xB9\xCE\xA9"), "unicode words");
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
    CHECK(parse_line("echo $x ${y} $? $ \"$z\" '$w'") == hp::Error::Ok && g_parsed.word_count == 7, "variable forms parse");
    CHECK(parse_line("echo ${") == hp::Error::BadEscape, "broken brace variable");
    CHECK(parse_line("echo *.txt a?c") == hp::Error::Ok && g_parsed.words[1].len == 5 && g_parsed.text(g_parsed.words[1])[0] == hp::MARK_STAR, "wildcards marked");
    CHECK(parse_line("echo '*.txt' \"*.txt\" \\*") == hp::Error::Ok && g_parsed.text(g_parsed.words[1])[0] == '*' && g_parsed.text(g_parsed.words[2])[0] == '*' && g_parsed.text(g_parsed.words[3])[0] == '*', "quoted wildcards are literal");
}

static bool completes_to(const char* line, const char* insert, u32 count) {
    helm::Completion c;
    u32 cursor = static_cast<u32>(__builtin_strlen(line));
    bool ok = helm::complete(*g_sh, line, cursor, &c);
    bool good = ok && same(c.insert, insert) && c.count == count;
    if (!good) printf("   complete '%s': ok=%d insert='%s' count=%u (want '%s' %u)\n", line, ok, ok ? c.insert : "", ok ? c.count : 0, insert, count);
    if (ok) helm::release(c);
    return good;
}

static void test_shell() {
    printf("-- shell on ram\n");
    CHECK(out_is("echo hello", "hello\n"), "echo");
    CHECK(out_is("echo -n hi", "hi"), "echo -n");
    CHECK(out_is("echo 'a  b'   \"c  d\"", "a  b c  d\n"), "quotes preserved");
    CHECK(out_is("echo \"caf\\u{e9} \\u{20B9}\"", "caf\xC3\xA9 \xE2\x82\xB9\n"), "unicode escapes");
    CHECK(out_is("echo caf\xC3\xA9 \xCE\xA9\xE2\x82\xB9", "caf\xC3\xA9 \xCE\xA9\xE2\x82\xB9\n"), "unicode passthrough");
    CHECK(out_is("set name world; echo hello $name ${name}s", "hello world worlds\n"), "variables");
    CHECK(out_is("set a=1 2 3; echo \"[$a]\"", "[1 2 3]\n"), "set joins values");
    CHECK(out_is("echo '$name'", "$name\n"), "single quotes block expansion");
    CHECK(out_is("echo $undefined-x", "-x\n"), "undefined variable is empty");
    CHECK(out_is("unset name; echo [$name]", "[]\n"), "unset");
    CHECK(out_is("true; echo $?", "0\n"), "status zero");
    CHECK(out_is("false; echo $?", "1\n"), "status one");
    CHECK(out_is("nosuchcommand; echo $?", "  unknown command: nosuchcommand\n  try help\n127\n"), "unknown command status");
    CHECK(out_is("true && echo yes", "yes\n"), "and runs");
    CHECK(out_is("false && echo yes", ""), "and skips");
    CHECK(out_is("false || echo rescued", "rescued\n"), "or runs");
    CHECK(out_is("true || echo never", ""), "or skips");
    CHECK(out_is("false && echo a || echo b", "b\n"), "and then or");
    CHECK(out_is("echo $cwd", "ram:/\n"), "cwd variable");
    CHECK(out_has("echo 'a", "unterminated quote") && g_status == 2, "syntax error status");

    CHECK(out_is("echo hi > f.txt; cat f.txt", "hi\n"), "redirect and cat");
    CHECK(out_is("echo more >> f.txt; cat f.txt", "hi\nmore\n"), "append");
    CHECK(out_is("echo over > f.txt; cat f.txt", "over\n"), "overwrite");
    CHECK(out_is("cat < f.txt", "over\n"), "input redirect");
    CHECK(out_is("echo b > s.txt; echo a >> s.txt; echo c >> s.txt; sort s.txt", "a\nb\nc\n"), "sort");
    CHECK(out_is("cat s.txt | sort -r", "c\nb\na\n"), "pipe into sort -r");
    CHECK(out_is("cat s.txt | sort | head -n 2", "a\nb\n"), "three stage pipe");
    CHECK(out_is("cat s.txt | grep b", "b\n"), "pipe into grep");
    CHECK(out_is("cat s.txt | count -l", "       3\n"), "pipe into count");
    CHECK(out_is("echo x | cat > piped.txt; cat piped.txt", "x\n"), "pipe then redirect");
    CHECK(out_has("cat < missing.txt", "cannot read missing.txt: not found") && g_status == 1, "missing input file");
    CHECK(out_has("echo x > nodir/f", "cannot write nodir/f: not found") && g_status == 1, "redirect into missing directory");
    CHECK(out_has("echo x > *.txt", "wildcards are not allowed in redirection targets"), "wildcard redirect target");
    CHECK(out_has("echo x > sys:/version", "read-only realm"), "redirect into read-only realm");
    CHECK(out_has("cat", "no input") && g_status == 1, "cat without input");

    CHECK(out_is("echo \"caf\\u{e9} \\u{20B9}\" > \"caf\xC3\xA9 \xE2\x82\xB9.txt\"; cat caf\xC3\xA9\\ \xE2\x82\xB9.txt", "caf\xC3\xA9 \xE2\x82\xB9\n"), "unicode file name end to end");
    CHECK(out_is("echo 1 > \"\xCE\xA9mega\"; ls | grep mega", "\xCE\xA9mega\n"), "ls into grep with unicode");
    CHECK(out_has("echo 1 > \"bad\\u{202E}name\"", "invalid name"), "bidi override refused in names");

    sh("rm -f *");
    CHECK(out_is("touch a.txt b.txt c.log; ls | cat", "a.txt\nb.txt\nc.log\n"), "touch and ls");
    CHECK(out_is("echo *.txt", "a.txt b.txt\n"), "glob star");
    CHECK(out_is("echo ?.log", "c.log\n"), "glob question mark");
    CHECK(out_is("echo '*.txt'", "*.txt\n"), "quoted glob literal");
    CHECK(out_is("echo *.zip", "  no match: *.zip\n") && g_status == 1, "glob without match is an error");
    CHECK(out_is("echo x/*.txt", "  cannot expand x/*.txt: not found\n"), "glob in missing directory");
    CHECK(out_has("echo */a", "wildcards are only allowed in the last path component"), "glob in directory part refused");
    CHECK(out_is("echo ram:*", "ram:a.txt ram:b.txt ram:c.log\n"), "glob after a realm prefix");
    CHECK(out_is("mkdir -p d/e; touch d/e/x1 d/e/x2; echo d/e/x?", "d/e/x1 d/e/x2\n"), "glob in subdirectory");
    sh("touch caf\xC3\xA9 caff cafe; ");
    CHECK(out_is("echo caf?", "cafe caff caf\xC3\xA9\n"), "question mark matches one character, not one byte");
    CHECK(out_is("echo c*\xC3\xA9", "caf\xC3\xA9\n"), "star with unicode tail");
    sh("rm -f caf*");

    CHECK(out_is("pwd", "ram:/\n"), "pwd");
    CHECK(out_is("cd d; pwd", "ram:/d\n"), "cd");
    CHECK(out_is("cd e; pwd; cd ..; cd ..; pwd", "ram:/d/e\nram:/\n"), "cd up");
    CHECK(out_is("cd d; cd -; pwd", "ram:/\n"), "cd back");
    CHECK(out_has("cd a.txt", "not a directory") && g_status == 1, "cd into a file");
    CHECK(out_has("cd nowhere", "not found"), "cd into nothing");
    CHECK(out_is("cd sys:; pwd; cd", "sys:/\n"), "cd to realm");
    sh("cd ram:/");
    CHECK(out_is("pwd", "ram:/\n"), "back in ram");

    CHECK(out_is("mkdir tmp; cd tmp; touch inner; cd ..; ls tmp | cat", "inner\n"), "ls of a directory");
    CHECK(out_has("mkdir tmp", "already exists"), "mkdir twice");
    CHECK(out_has("rm tmp", "is a directory"), "rm refuses directories");
    CHECK(out_has("rmdir tmp", "not empty"), "rmdir refuses full directories");
    CHECK(out_is("rm -r tmp; ls | grep -c tmp", "0\n"), "rm -r");
    CHECK(out_is("rm -f nothing-here; echo $?", "0\n"), "rm -f ignores missing");
    CHECK(out_has("rm nothing-here", "not found") && g_status == 1, "rm missing");
    CHECK(out_has("rm -r /", "not allowed"), "rm -r of the root is refused");
    CHECK(out_has("rm -r ram:/", "not allowed"), "rm -r of a realm root is refused");

    CHECK(out_is("echo data > src; cp src dst; cat dst", "data\n"), "cp");
    CHECK(out_has("cp src dst", "already exists; use -f"), "cp refuses to overwrite");
    CHECK(out_is("echo new > src; cp -f src dst; cat dst", "new\n"), "cp -f");
    CHECK(out_is("mkdir box; cp src box; cat box/src", "new\n"), "cp into directory");
    CHECK(out_has("cp box box2", "is a directory: use cp -r"), "cp refuses directories without -r");
    CHECK(out_is("cp -r box box2; cat box2/src", "new\n"), "cp -r");
    CHECK(out_is("cp -r box box2; cat box2/box/src", "new\n"), "cp -r into an existing directory nests the copy");
    CHECK(out_has("cp -r box box2", "never merged"), "cp -r onto an existing copy is refused");
    CHECK(out_has("cp src a.txt b.txt", "must be an existing directory"), "several sources need a directory");
    CHECK(out_is("mkdir many; cp src dst a.txt many; ls many | count -l", "       3\n"), "several sources");
    CHECK(out_is("mv src moved; cat moved", "new\n"), "mv");
        CHECK(out_has("cat src", "not found"), "old name is gone");
    CHECK(out_has("mv moved dst", "already exists; use -f"), "mv refuses to overwrite");
    CHECK(out_is("mv -f moved dst; cat dst", "new\n"), "mv -f");
    CHECK(out_is("mv dst box; cat box/dst", "new\n"), "mv into directory");
    CHECK(out_is("mv box renamed; cat renamed/dst", "new\n"), "mv a directory");
    CHECK(out_has("mv renamed renamed/inside", "not supported"), "mv a directory into itself");

    CHECK(out_has("stat a.txt", "kind     file"), "stat file");
    CHECK(out_has("stat renamed", "directory"), "stat directory");
    CHECK(out_has("stat sys:/version", "read-only"), "stat read-only realm");
    CHECK(out_has("ls -l renamed | cat", "dst"), "ls -l");
    CHECK(out_has("ls -l a.txt", "a.txt"), "ls -l of a file");
    CHECK(out_has("tree renamed", "dst"), "tree");
    CHECK(out_has("mkdir -p t1/t2/t3; touch t1/t2/t3/leaf; tree -L 2 t1 | cat", "t2") && !contains(g_out, "leaf"), "tree depth limit");
    CHECK(out_has("tree t1 | cat", "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 leaf"), "tree draws branches");
    CHECK(out_is("tree -d t1 | cat", "t1\n\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 t2\n    \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 t3\n2 directories\n"), "tree -d");
    CHECK(out_is("find t1 -name 'l*'", "t1/t2/t3/leaf\n"), "find by name");
    CHECK(out_is("find t1 -type d", "t1/t2\nt1/t2/t3\n"), "find by type");
    CHECK(out_is("find t1 -name nothing", "") && g_status == 1, "find with no hits");
    CHECK(out_has("realms", "disk"), "realms");
    CHECK(out_has("df", "snap"), "df");

    CHECK(out_is("echo \"one\\ntwo\\nthree\\nfour\" > lines; head -n 2 lines", "one\ntwo\n"), "head");
    CHECK(out_is("tail -n 2 lines", "three\nfour\n"), "tail");
    CHECK(out_is("tail -n 10 lines | count -l", "       4\n"), "tail more than available");
    CHECK(out_is("grep o lines", "one\ntwo\nfour\n"), "grep");
    CHECK(out_is("grep -v o lines", "three\n"), "grep -v");
    CHECK(out_is("grep -c o lines", "3\n"), "grep -c");
    CHECK(out_is("grep -n thr lines", "3:three\n"), "grep -n");
    CHECK(out_is("grep -i ONE lines", "one\n"), "grep -i");
    CHECK(out_is("grep zzz lines", "") && g_status == 1, "grep with no match");
    CHECK(out_is("echo \"caf\\u{e9}\" | grep \"\\u{e9}\"", "caf\xC3\xA9\n"), "grep a Unicode needle");
    CHECK(out_is("count lines", "       4       4      19 lines\n"), "count all");
    CHECK(out_is("echo \"\\u{20B9}\\u{20B9}\" | count -m", "       3\n"), "count characters includes the newline");
    CHECK(out_is("echo \"\\u{20B9}\\u{20B9}\" | count -c", "       7\n"), "count bytes");
    CHECK(out_is("echo \"b\\na\\nb\\nc\\nb\" | sort -u", "a\nb\nc\n"), "sort -u");
    CHECK(out_is("echo \"10\\n9\\n100\\n-5\" | sort -n", "-5\n9\n10\n100\n"), "sort -n");
    CHECK(out_is("echo \"a\\na\\nb\\na\" | uniq", "a\nb\na\n"), "uniq");
    CHECK(out_is("echo \"a\\na\\nb\" | uniq -c", "     2 a\n     1 b\n"), "uniq -c");
    CHECK(out_is("echo AB | hex", "00000000  41 42 0a                                          AB.\n"), "hex");
    CHECK(out_is("echo AB | hex -s 1 -n 1", "00000001  42                                                B\n"), "hex window");
    CHECK(out_is("cat -n lines | head -n 1", "     1  one\n"), "cat -n");

    CHECK(out_is("echo \"echo from script\\nset v 7\\necho v=\\$v\" > s.hm; run s.hm", "from script\nv=7\n"), "run a script");
    CHECK(out_has("echo \"echo one\\nfalse\\necho two\" > bad.hm; run bad.hm", "stopped at line 2") && !contains(g_out, "two"), "script stops on failure");
    CHECK(out_has("run -k bad.hm", "two"), "run -k continues");
    CHECK(out_has("echo \"run loop.hm\" > loop.hm; run loop.hm", "nested too deeply"), "script recursion is bounded");

    CHECK(out_has("help", "files and realms"), "help lists tables");
    CHECK(out_has("help ls", "ls [-l]"), "help for one command");
    CHECK(out_has("help syntax", "keys"), "help syntax");
    CHECK(out_has("help nosuch", "no such command") && g_status == 1, "help for nothing");
    CHECK(out_has("ls -z", "usage: ls") && g_status == 2, "bad option");
    CHECK(out_has("cp onlyone", "usage: cp") && g_status == 2, "usage error status");

    CHECK(out_is("cat sys:/version", "Aphelion\n"), "sys realm readable");
}

static void test_completion() {
    printf("-- completion\n");
    sh("rm -r -f *; touch alpha.txt alps beta; mkdir apple; touch \"with space.txt\"");
    CHECK(completes_to("ca", "cat ", 1), "command prefix");
    CHECK(completes_to("cd al", "alp", 2), "two files with a common prefix");
    CHECK(completes_to("cd app", "apple/", 1), "unique directory gets a slash");
    CHECK(completes_to("cat be", "beta ", 1), "unique file gets a space");
    CHECK(completes_to("cat alpha.t", "alpha.txt ", 1), "unique extension");
    CHECK(completes_to("cat with", "with\\ space.txt ", 1), "spaces are escaped");
    CHECK(completes_to("cat \"with", "\"with space.txt\" ", 1), "completion inside quotes");
    CHECK(completes_to("echo hi | gre", "grep ", 1), "command after a pipe");
    CHECK(completes_to("cat x; unse", "unset ", 1), "command after a semicolon");
    CHECK(completes_to("cat < be", "beta ", 1), "path after a redirect");
    CHECK(completes_to("cat sys:/ver", "sys:/version ", 1), "path in another realm");
    CHECK(completes_to("cat sys:", "sys:version ", 1), "completing right after a realm colon");
    CHECK(completes_to("cd sn", "snap:/", 1), "realm name completes with a slash");
    { helm::Completion e; CHECK(!helm::complete(*g_sh, "cat apple/", 10, &e), "empty directory has nothing to offer"); }
    helm::Completion c;
    CHECK(!helm::complete(*g_sh, "cat zzz", 7, &c), "no candidates");
    sh("touch caf\xC3\xA9 cafz");
    CHECK(completes_to("cat caf", "caf", 2), "common prefix stops before a multibyte split");
    { helm::Completion e; bool ok = helm::complete(*g_sh, "cat caf\xC3", 8, &e); CHECK(ok && same(e.insert, "caf\xC3\xA9 ") && e.count == 1, "partial UTF-8 still completes"); if (ok) helm::release(e); }
    sh("rm -r -f *");
}

static u32 g_poll_calls;
static bool poll_always(void*) { ++g_poll_calls; return true; }

static void test_interrupt_and_limits() {
    printf("-- interrupt and limits\n");
    nebula::Handle h;
    CHECK(nebula::open(g_sh->session, "big.txt", nebula::OPEN_WRITE | nebula::OPEN_CREATE, &h) == nebula::Status::Ok, "open big file");
    for (u32 i = 0; i < 200; ++i) nebula::append(g_sh->session, h, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n", 65);
    CHECK(nebula::close(g_sh->session, h) == nebula::Status::Ok, "close big file");
    g_sh->interrupt_poll = &poll_always;
    g_poll_calls = 0;
    sh("grep 0 big.txt | count -l");
    CHECK(g_status == 130 && contains(g_out, "interrupted") && g_poll_calls >= 1, "interrupt stops a pipeline and reports it");
    CHECK(!contains(g_out, "     200"), "nothing downstream ran after the interrupt");
    sh("sort big.txt");
    CHECK(g_status == 130, "interrupt during sort");
    sh("tree .");
    CHECK(g_status == 130 || g_status == 0, "tree notices the poll or has nothing to walk");
    g_sh->interrupt_poll = nullptr;
    g_sh->aborted = false;
    CHECK(out_is("echo ok", "ok\n"), "shell works after an interrupt");
    sh("rm big.txt");

    sh("echo x > a");
    char line[1024];
    u32 n = 0;
    const char* pre = "echo ";
    for (u32 i = 0; pre[i]; ++i) line[n++] = pre[i];
    for (u32 i = 0; i < 520; ++i) line[n++] = 'z';
    line[n] = 0;
    sh(line);
    CHECK(contains(g_out, "too long") || g_status == 0, "very long word handled");

    sh("set v1 1; set v2 2; set v3 3");
    char vline[64];
    bool all = true;
    for (int i = 0; i < 30; ++i) {
        vline[0] = 's'; vline[1] = 'e'; vline[2] = 't'; vline[3] = ' '; vline[4] = 'q';
        vline[5] = static_cast<char>('a' + i % 26);
        vline[6] = static_cast<char>('a' + i / 26);
        vline[7] = ' '; vline[8] = '1'; vline[9] = 0;
        sh(vline);
        if (g_status != 0) all = false;
    }
    CHECK(!all, "variable table is bounded");
    CHECK(out_has("set 1bad x", "cannot set"), "bad variable name");
    sh("unset v1 v2 v3");
    for (int i = 0; i < 30; ++i) {
        char u[32] = "unset q";
        u[7] = static_cast<char>('a' + i % 26);
        u[8] = static_cast<char>('a' + i / 26);
        u[9] = 0;
        sh(u);
    }
}

static void test_disk() {
    printf("-- disk and snapshots from the shell\n");
    sh("cd disk:/; rm -r -f *");
    CHECK(out_is("pwd", "disk:/\n"), "cd disk");
    CHECK(out_is("mkdir -p proj/src; echo v1 > proj/src/main; echo n1 > notes", ""), "build a tree on disk");
    CHECK(out_has("snapshot", "snapshot 1 taken"), "take a snapshot");
    CHECK(out_has("snapshot list", "snap:/"), "list snapshots");
    CHECK(out_is("echo v2 > proj/src/main; cat snap:/1/proj/src/main", "v1\n"), "snapshot keeps the old contents");
    CHECK(out_has("ls snap:/ | cat", "1"), "snapshots are listed as directories");
    CHECK(out_is("cp -r snap:/1/proj restored; cat restored/src/main", "v1\n"), "restore a tree from a snapshot");
    CHECK(out_has("rm snap:/1/notes", "read-only") && g_status == 1, "snapshots cannot be modified");
    CHECK(out_is("cp notes ram:/n.copy; cat ram:/n.copy", "n1\n"), "copy disk to ram");
    CHECK(out_is("mv ram:/n.copy disk:/n.moved; cat disk:/n.moved; ls ram:/ | grep -c n.copy", "n1\n0\n"), "move across realms");
    CHECK(out_has("check", "volume is consistent"), "check");
    CHECK(out_has("check -d", "volume is consistent"), "deep check");
    CHECK(out_is("sync; echo $?", "0\n"), "sync");
    CHECK(out_has("snapshot rm 99", "not found") && g_status == 1, "delete missing snapshot");
    CHECK(out_is("snapshot rm 1; ls snap:/ | grep -c 1", "0\n"), "delete snapshot");
    nebula::disk_set_mounted(false);
    CHECK(out_has("ls disk:/", "not available"), "unmounted disk reports unavailable");
    CHECK(out_has("snapshot", "no disk volume"), "snapshot without a volume");
    nebula::disk_set_mounted(true);
    CHECK(out_has("mkfs erase", "refusing to erase") && g_status == 1, "mkfs refuses a mounted volume");
    nebula::disk_set_mounted(false);
    nebula::disk_set_confirmed_only(true);
    CHECK(out_has("mkfs erase", "typed confirmation") && g_status == 1, "mkfs refuses when the disk needs a typed confirmation");
    nebula::disk_set_confirmed_only(false);
    CHECK(out_has("mkfs", "usage: mkfs erase") && g_status == 2, "mkfs needs the erase word");
    CHECK(out_has("mkfs wipe", "usage: mkfs erase") && g_status == 2, "mkfs rejects other words");
    __builtin_memset(g_disk, 0xAB, 64 * 512);
    CHECK(out_has("mkfs erase", "refusing to touch it") && g_status == 1, "mkfs refuses a foreign disk");
    CHECK(g_disk[0] == 0xAB && g_disk[64 * 512 - 1] == 0xAB, "the refused disk was left untouched");
    __builtin_memset(g_disk, 0, 64 * 512);
    CHECK(out_has("mkfs erase", "new Stellar volume created") && g_status == 0, "mkfs formats a blank disk");
    CHECK(nebula::disk_is_mounted(), "mkfs leaves the volume mounted");
    CHECK(out_is("ls disk:/ | grep -c .", "0\n"), "the new volume is empty");
    CHECK(out_is("echo fresh > disk:/f; cat disk:/f", "fresh\n"), "the new volume is writable");
    CHECK(out_has("check", "volume is consistent"), "the new volume checks clean");
    sh("rm -r -f *; cd ram:/");
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

    static helm::Shell shell;
    g_sh = &shell;
    helm::init(shell, helm::Sink{&sink_write, nullptr}, false, 40, "ram");

    test_parser();
    test_shell();
    test_completion();
    test_interrupt_and_limits();
    test_disk();
    helm::shutdown(shell);

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
