#include <cosmos/helm_rows.hpp>
#include <cosmos/kheap.hpp>

namespace helm {

namespace {

bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

u32 length(const char* s) { return static_cast<u32>(cstr::length(s)); }

int cmd_say(Shell&, Io& io, u32 argc, char** argv) {
    bool newline = true;
    if (!opt_bool(io, "newline", &newline)) return 2;
    for (u32 i = 1; i < argc; ++i) {
        if (i > 1) io.out.put(' ');
        io.out.str(argv[i]);
    }
    if (newline) io.out.put('\n');
    return 0;
}

int cmd_ok(Shell&, Io&, u32, char**) { return 0; }
int cmd_fail(Shell&, Io&, u32, char**) { return 1; }

int cmd_bind(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc == 1) {
        rows::Table& out = *io.out_rows;
        rows::begin(out);
        rows::add_column(out, "name", rows::Kind::Text);
        rows::add_column(out, "value", rows::Kind::Text);
        for (u32 i = 0; i < MAX_VARS; ++i) {
            if (!sh.vars[i].used) continue;
            u32 r = rows::add_row(out);
            if (r == ~0u) break;
            rows::set_text(out, r, 0, sh.vars[i].name);
            rows::set_text(out, r, 1, sh.vars[i].value);
        }
        return 0;
    }
    char value[VAR_VALUE + 8];
    u32 vn = 0;
    for (u32 i = 2; i < argc; ++i) {
        u32 al = length(argv[i]);
        u32 extra = i > 2 ? 1 : 0;
        if (vn + extra + al + 1 >= sizeof(value)) { print_error(io, "value is too long"); return 1; }
        if (extra) value[vn++] = ' ';
        mem::copy(value + vn, argv[i], al);
        vn += al;
    }
    value[vn] = 0;
    if (var_reserved(argv[1])) {
        print_error(io, "status and here are provided by the shell and cannot be bound");
        return 1;
    }
    if (!var_set(sh, argv[1], value)) {
        print_error(io, "cannot bind that name (letters, digits and _; at most 23 characters; 24 names; values under 160 bytes)");
        return 1;
    }
    return 0;
}

int cmd_unbind(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc < 2) { print_error(io, "usage: unbind name..."); return 2; }
    for (u32 i = 1; i < argc; ++i) var_unset(sh, argv[i]);
    return 0;
}

void help_syntax(Io& io) {
    Writer& w = io.out;
    auto head = [&](const char* t) { w.rgb(palette::AMBER); w.str(t); w.reset(); w.put('\n'); };
    auto row = [&](const char* k, const char* v) {
        w.str("  ");
        w.rgb(palette::ICE);
        w.pad_to(k, 24);
        w.rgb(palette::MIST);
        w.str(v);
        w.reset();
        w.put('\n');
    };
    head("paths");
    row("realm:/a/b", "absolute path inside a realm (ram, disk, snap, sys)");
    row("/a/b", "absolute path in the current realm");
    row("a/b, ./a, ..", "relative to the current location");
    row("./a:b", "a name that contains a colon");
    head("options");
    row("chart depth: 3", "a named option is  key: value  and may come anywhere after the command");
    row("fade tree: yes", "yes/no options take yes, no, true, false, on, off, 1 or 0");
    row("say 'depth:'", "quote a word that ends in a colon to use it as plain text");
    head("rows");
    row("chart | where size > 1KiB", "commands such as chart, probe, orbit and realms pass typed rows down a pipe");
    row("| order size down: yes", "sizes (2KiB), times (2026-10-01) and numbers compare as what they are");
    row("| pick name size", "keep columns; first, last, distinct and tally work on rows too");
    row("| etch listing.txt", "etch writes whatever arrives to a file; append: yes adds to it");
    head("combining");
    row("a | b", "send the output of a into b");
    row("a ; b", "run both");
    row("a => b", "run b only if a succeeded");
    row("a !> b", "run b only if a failed");
    head("quoting");
    row("'text'", "literal, nothing inside is special");
    row("\"text @x\"", "values and escapes: \\n \\t \\e \\\" \\\\ \\@ \\u{20B9}");
    row("a\\ b", "backslash escapes one character");
    row("# comment", "ignored when it starts a word");
    head("values");
    row("@name, @{name}", "a bound value (bind name text); @status is the last result, @here the location");
    row("* ?", "wildcards in the last path component; ? is one character, not one byte");
    head("keys");
    row("Tab", "complete commands, realms, files; press twice to list");
    row("Up, Down", "history");
    row("Ctrl+A/E/U/K/W", "start, end, delete to start, delete to end, delete word");
    row("Ctrl+Left/Right", "move by word");
    row("Ctrl+C", "abandon the line or interrupt a running command");
    row("Ctrl+L", "clear the screen");
    row("PageUp, PageDown", "scroll back through earlier output; typing returns to the bottom");
    row("Shift+Up, Shift+Down", "scroll one line");
}

int cmd_help(Shell&, Io& io, u32 argc, char** argv) {
    Writer& w = io.out;
    if (argc >= 2 && same(argv[1], "syntax")) { help_syntax(io); return 0; }
    if (argc >= 2) {
        const Command* c = find_command(argv[1]);
        if (!c) {
            print_error(io, "no such command; run help to list them");
            return 1;
        }
        w.str("  ");
        w.rgb(palette::ICE);
        w.str(c->usage);
        w.reset();
        w.str("\n  ");
        w.rgb(palette::MIST);
        w.str(c->summary);
        w.reset();
        w.put('\n');
        if (c->flags & CMD_ROWS) {
            w.str("  ");
            w.rgb(palette::MIST);
            w.str("works on rows; text piped in becomes one row per line");
            w.reset();
            w.put('\n');
        }
        return 0;
    }
    u32 name_width = 0;
    for (u32 t = 0; t < table_count(); ++t) {
        const Table* tb = table_at(t);
        for (u32 i = 0; i < tb->count; ++i) {
            u32 c = utf8::cells(tb->commands[i].name, length(tb->commands[i].name));
            if (c > name_width) name_width = c;
        }
    }
    for (u32 t = 0; t < table_count(); ++t) {
        const Table* tb = table_at(t);
        w.rgb(palette::AMBER);
        w.str(tb->title);
        w.reset();
        w.put('\n');
        for (u32 i = 0; i < tb->count; ++i) {
            w.str("  ");
            w.rgb(palette::ICE);
            w.pad_to(tb->commands[i].name, name_width + 2);
            w.rgb(palette::MIST);
            w.str(tb->commands[i].summary);
            w.reset();
            w.put('\n');
        }
    }
    w.rgb(palette::MIST);
    w.str("help <command> for its options, help syntax for options, rows, combining, quoting and keys\n");
    w.reset();
    return 0;
}

int cmd_launch(Shell& sh, Io& io, u32 argc, char** argv) {
    bool keep_going = false;
    if (!opt_bool(io, "keep", &keep_going)) return 2;
    if (argc != 2) { print_error(io, "usage: launch file  keep: yes"); return 2; }
    if (sh.depth >= MAX_SCRIPT_DEPTH) { print_error(io, "scripts are nested too deeply"); return 1; }
    nebula::Blob b;
    nebula::Status st = lens::text::read_file(sh.cursor, argv[1], &b);
    if (st != nebula::Status::Ok) { print_status(io, "cannot read", argv[1], st); return 1; }
    ++sh.depth;
    int status = 0;
    u64 pos = 0;
    u32 line_no = 0;
    char* line = static_cast<char*>(kheap::alloc(parse::MAX_LINE_BYTES + 1));
    if (!line) { nebula::release(b); --sh.depth; print_error(io, "out of memory"); return 1; }
    while (pos < b.size && !sh.quit) {
        u64 start = pos;
        while (pos < b.size && b.data[pos] != '\n') ++pos;
        u64 len = pos - start;
        if (pos < b.size) ++pos;
        ++line_no;
        if (len && b.data[start + len - 1] == '\r') --len;
        if (len > parse::MAX_LINE_BYTES) {
            print_error(io, "script line is too long");
            status = 2;
            if (!keep_going) break;
            continue;
        }
        mem::copy(line, b.data + start, static_cast<usize>(len));
        line[len] = 0;
        bool blank = true;
        for (u64 i = 0; i < len; ++i)
            if (line[i] != ' ' && line[i] != '\t') { blank = false; break; }
        if (blank) continue;
        io.out.flush();
        status = run_line(sh, line);
        if (sh.aborted) break;
        if (status != 0 && !keep_going) {
            io.err.rgb(palette::SAND);
            io.err.str("  stopped at line ");
            io.err.num(line_no);
            io.err.str(" of ");
            io.err.str(argv[1]);
            io.err.str(" (status ");
            io.err.num(static_cast<u64>(status));
            io.err.str(")\n");
            io.err.reset();
            break;
        }
    }
    kheap::free(line);
    nebula::release(b);
    --sh.depth;
    return status;
}

constexpr Command COMMANDS[] = {
    {"help", "help [command|syntax]", "this list, or details on one command", &cmd_help},
    {"say", "say [text...]  newline: no", "print text, Unicode included", &cmd_say, "newline"},
    {"bind", "bind [name value...]", "show the bound names, or bind a name to text; read it back as @name", &cmd_bind, "", CMD_LITERAL},
    {"unbind", "unbind name...", "remove bound names", &cmd_unbind},
    {"launch", "launch file  keep: yes", "run the lines of a script file; keep: yes continues after a failure", &cmd_launch, "keep"},
    {"ok", "ok", "succeed", &cmd_ok},
    {"fail", "fail", "fail", &cmd_fail},
};

constexpr Table TABLE{"shell", COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0])};

}

void register_builtin_commands() { register_table(&TABLE); }

}
