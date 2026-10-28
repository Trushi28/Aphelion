#pragma once
#include <cosmos/types.hpp>
#include <cosmos/nebula.hpp>
#include <cosmos/lens_text.hpp>
#include <cosmos/helm_parse.hpp>
#include <cosmos/utf8.hpp>

namespace helm {

namespace rows { struct Table; }

namespace palette {
constexpr u32 AMBER = 0xFFB347;
constexpr u32 EMBER = 0xFF7B3A;
constexpr u32 ICE = 0x8FD3FF;
constexpr u32 MINT = 0xC0FFC0;
constexpr u32 SAND = 0xE0D080;
constexpr u32 MIST = 0xB0B0C0;
constexpr u32 TEXT = 0xD9DEE8;
}

struct Facts {
    u32 suns;
    const char* storage;
    bool keyboard_ready;
    bool keyboard_irq;
    bool serial_present;
    bool serial_irq;
    bool usb_mode;
    bool disk_mounted;
    const char* disk_note;
};

void set_facts(const Facts& f);
const Facts& facts();
bool spawn();

struct Sink {
    void (*fn)(void* ctx, const char* data, u32 n);
    void* ctx;
};

class Writer {
public:
    Writer(Sink sink, bool color, bool terminal) : sink_(sink), color_(color), terminal_(terminal) {}
    ~Writer() { flush(); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void flush() {
        if (n_ && sink_.fn) sink_.fn(sink_.ctx, buf_, n_);
        n_ = 0;
    }
    void put(char c) {
        if (n_ == sizeof(buf_)) flush();
        buf_[n_++] = c;
        ++total_;
    }
    void str(const char* s) { while (*s) put(*s++); }
    void bytes(const char* s, u64 len) { for (u64 i = 0; i < len; ++i) put(s[i]); }
    void cp(u32 c) {
        char e[4];
        u32 k = utf8::encode(c, e);
        for (u32 i = 0; i < k; ++i) put(e[i]);
    }
    void num(u64 v) {
        char t[24];
        u32 i = 0;
        if (!v) t[i++] = '0';
        while (v) { t[i++] = static_cast<char>('0' + v % 10); v /= 10; }
        while (i) put(t[--i]);
    }
    void pad_num(u64 v, u32 width) {
        u32 digits = 1;
        for (u64 x = v; x >= 10; x /= 10) ++digits;
        for (u32 i = digits; i < width; ++i) put(' ');
        num(v);
    }
    void hex(u64 v, u32 digits) {
        static const char* d = "0123456789abcdef";
        for (u32 i = digits; i-- > 0;) put(d[(v >> (i * 4)) & 15]);
    }
    void spaces(u32 n) { for (u32 i = 0; i < n; ++i) put(' '); }
    void repeat(const char* s, u32 count) { for (u32 i = 0; i < count; ++i) str(s); }
    void pad_to(const char* s, u32 cells) {
        str(s);
        u32 c = utf8::cells(s, static_cast<u32>(cstr::length(s)));
        if (c < cells) spaces(cells - c);
    }
    void rgb(u32 c) {
        if (!color_) return;
        str("\x1b[38;2;");
        num((c >> 16) & 255); put(';'); num((c >> 8) & 255); put(';'); num(c & 255); put('m');
    }
    void reset() { if (color_) str("\x1b[0m"); }
    bool color() const { return color_; }
    bool terminal() const { return terminal_; }
    u64 total() const { return total_; }

private:
    Sink sink_;
    bool color_;
    bool terminal_;
    u32 n_ = 0;
    u64 total_ = 0;
    char buf_[2048];
};

constexpr u32 MAX_VARS = 24;
constexpr u32 VAR_NAME = 24;
constexpr u32 VAR_VALUE = 160;
constexpr u32 MAX_SCRIPT_DEPTH = 4;

struct Var {
    bool used;
    char name[VAR_NAME];
    char value[VAR_VALUE];
};

struct Shell {
    lens::text::Cursor cursor;
    nebula::path::Path previous;
    bool has_previous;
    Var vars[MAX_VARS];
    int status;
    Sink terminal;
    bool color;
    u32 cols;
    bool (*interrupt_poll)(void* ctx);
    void* interrupt_ctx;
    u32 poll_count;
    bool aborted;
    u32 depth;
    bool quit;

    bool poll() {
        if (aborted) return true;
        if (!interrupt_poll) return false;
        if ((++poll_count & 63) != 0) return false;
        if (interrupt_poll(interrupt_ctx)) aborted = true;
        return aborted;
    }
};

void init(Shell& sh, Sink terminal, bool color, u32 cols, const char* start_realm);
void shutdown(Shell& sh);

struct Named {
    const char* key;
    const char* value;
};

struct Io {
    Writer& out;
    Writer& err;
    const u8* in;
    u64 in_size;
    bool has_in;
    const rows::Table* in_rows;
    rows::Table* out_rows;
    const Named* named;
    u32 named_count;
};

using CommandFn = int (*)(Shell& sh, Io& io, u32 argc, char** argv);

constexpr u32 CMD_ROWS = 1u << 0;
constexpr u32 CMD_ANY = 1u << 1;
constexpr u32 CMD_LITERAL = 1u << 2;

struct Command {
    const char* name;
    const char* usage;
    const char* summary;
    CommandFn fn;
    const char* keys = "";
    u32 flags = 0;
};

struct Table {
    const char* title;
    const Command* commands;
    u32 count;
};

constexpr u32 MAX_TABLES = 8;
bool register_table(const Table* table);
const Command* find_command(const char* name);
u32 table_count();
const Table* table_at(u32 index);

void register_builtin_commands();
void register_fs_commands();
void register_text_commands();
void register_row_commands();
void register_sys_commands();
void print_splash(Writer& w);
void print_about(Writer& w);

int run_line(Shell& sh, const char* line);

const char* var_get(const Shell& sh, const char* name);
bool var_set(Shell& sh, const char* name, const char* value);
bool var_unset(Shell& sh, const char* name);
bool var_reserved(const char* name);

bool glob_match(const char* pattern, const char* name);
void mark_wildcards(const char* in, char* out, u32 cap);

struct Completion {
    u32 start;
    char insert[320];
    u32 count;
    char* names;
    u32 names_bytes;
    bool unique;
    bool is_command;
};

bool complete(Shell& sh, const char* line, u32 cursor, Completion* out);
void release(Completion& c);

enum class Flag : u8 { Absent, Yes, No, Bad };

const char* opt(const Io& io, const char* key);
Flag opt_flag(const Io& io, const char* key);
bool opt_yes(const Io& io, const char* key);
bool opt_u64(const Io& io, const char* key, u64* out);
bool opt_bool(Io& io, const char* key, bool* out);
bool opt_num(Io& io, const char* key, u64* out);

bool parse_u64(const char* s, u64* out);

using InputFn = void (*)(const char* name, const u8* data, u64 size, void* ctx);
int for_each_input(Shell& sh, Io& io, u32 argc, char** argv, u32 first, InputFn fn, void* ctx);

void write_safe(Writer& w, const u8* data, u64 size);
void print_status(Io& io, const char* what, const char* subject, nebula::Status st);
void print_error(Io& io, const char* message);
int code_of(nebula::Status st);

void format_time(u64 ms, char* out);
void print_size(Writer& w, u64 bytes);

}
