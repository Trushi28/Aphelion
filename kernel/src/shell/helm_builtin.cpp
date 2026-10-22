#include <cosmos/helm.hpp>
#include <cosmos/kheap.hpp>

namespace helm {

namespace {

bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

u32 length(const char* s) { return static_cast<u32>(__builtin_strlen(s)); }

int cmd_echo(Shell&, Io& io, u32 argc, char** argv) {
    u32 first = 1;
    bool newline = true;
    if (argc > 1 && same(argv[1], "-n")) { newline = false; first = 2; }
    for (u32 i = first; i < argc; ++i) {
        if (i > first) io.out.put(' ');
        io.out.str(argv[i]);
    }
    if (newline) io.out.put('\n');
    return 0;
}

int cmd_true(Shell&, Io&, u32, char**) { return 0; }
int cmd_false(Shell&, Io&, u32, char**) { return 1; }

int cmd_set(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc == 1) {
        for (u32 i = 0; i < MAX_VARS; ++i) {
            if (!sh.vars[i].used) continue;
            io.out.rgb(palette::AMBER);
            io.out.str(sh.vars[i].name);
            io.out.reset();
            io.out.put('=');
            io.out.str(sh.vars[i].value);
            io.out.put('\n');
        }
        return 0;
    }
    char name[VAR_NAME + 8];
    char value[VAR_VALUE + 8];
    u32 vn = 0;
    u32 first_value = 2;
    const char* a = argv[1];
    u32 eq = 0;
    while (a[eq] && a[eq] != '=') ++eq;
    if (a[eq] == '=') {
        if (eq >= sizeof(name)) { print_error(io, "variable name is too long"); return 1; }
        __builtin_memcpy(name, a, eq);
        name[eq] = 0;
        const char* tail = a + eq + 1;
        u32 tl = length(tail);
        if (tl >= sizeof(value)) { print_error(io, "value is too long"); return 1; }
        __builtin_memcpy(value, tail, tl);
        vn = tl;
    } else {
        if (length(a) >= sizeof(name)) { print_error(io, "variable name is too long"); return 1; }
        __builtin_memcpy(name, a, length(a) + 1);
    }
    bool have = vn > 0;
    for (u32 i = first_value; i < argc; ++i) {
        u32 al = length(argv[i]);
        u32 extra = have ? 1 : 0;
        if (vn + extra + al + 1 >= sizeof(value)) { print_error(io, "value is too long"); return 1; }
        if (have) value[vn++] = ' ';
        __builtin_memcpy(value + vn, argv[i], al);
        vn += al;
        have = true;
    }
    value[vn] = 0;
    if (!var_set(sh, name, value)) {
        print_error(io, "cannot set that variable (names are letters, digits and _; at most 23 characters; 24 variables; values under 160 bytes)");
        return 1;
    }
    return 0;
}

int cmd_unset(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc < 2) { print_error(io, "usage: unset name..."); return 2; }
    for (u32 i = 1; i < argc; ++i) var_unset(sh, argv[i]);
    return 0;
}

void help_syntax(Io& io) {
    Writer& w = io.out;
    auto head = [&](const char* t) { w.rgb(palette::AMBER); w.str(t); w.reset(); w.put('\n'); };
    auto row = [&](const char* k, const char* v) {
        w.str("  ");
        w.rgb(palette::ICE);
        w.pad_to(k, 22);
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
    head("quoting");
    row("'text'", "literal, nothing inside is special");
    row("\"text $x\"", "variables and escapes: \\n \\t \\e \\\" \\\\ \\$ \\u{20B9}");
    row("a\\ b", "backslash escapes one character");
    row("# comment", "ignored when it starts a word");
    head("combining");
    row("a | b", "send the output of a into b");
    row("a > f, a >> f", "write or append output to a file");
    row("a < f", "read input from a file");
    row("a ; b", "run both");
    row("a && b, a || b", "run b if a succeeded, or if a failed");
    head("expansion");
    row("$name, ${name}", "variable (set name value); $? is the last status; $cwd the location");
    row("* ?", "wildcards in the last path component; ? is one character, not one byte");
    head("keys");
    row("Tab", "complete commands, realms, files; press twice to list");
    row("Up, Down", "history");
    row("Ctrl+A/E/U/K/W", "start, end, delete to start, delete to end, delete word");
    row("Ctrl+Left/Right", "move by word");
    row("Ctrl+C", "abandon the line or interrupt a running command");
    row("Ctrl+L", "clear the screen");
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
        return 0;
    }
    u32 usage_width = 0;
    for (u32 t = 0; t < table_count(); ++t) {
        const Table* tb = table_at(t);
        for (u32 i = 0; i < tb->count; ++i) {
            u32 c = utf8::cells(tb->commands[i].usage, length(tb->commands[i].usage));
            if (c > usage_width) usage_width = c;
        }
    }
    if (usage_width > 30) usage_width = 30;
    for (u32 t = 0; t < table_count(); ++t) {
        const Table* tb = table_at(t);
        w.rgb(palette::AMBER);
        w.str(tb->title);
        w.reset();
        w.put('\n');
        for (u32 i = 0; i < tb->count; ++i) {
            w.str("  ");
            w.rgb(palette::ICE);
            w.pad_to(tb->commands[i].usage, usage_width + 2);
            w.rgb(palette::MIST);
            w.str(tb->commands[i].summary);
            w.reset();
            w.put('\n');
        }
    }
    w.rgb(palette::MIST);
    w.str("help <command> for one command, help syntax for quoting, pipes, wildcards and keys\n");
    w.reset();
    return 0;
}

int cmd_run(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    bool keep_going = false;
    int c;
    while ((c = next_option(o, "k", "")) >= 0) {
        if (c == 'k') keep_going = true;
        else { print_error(io, "usage: run [-k] file"); return 2; }
    }
    if (argc - o.index != 1) { print_error(io, "usage: run [-k] file"); return 2; }
    if (sh.depth >= MAX_SCRIPT_DEPTH) { print_error(io, "scripts are nested too deeply"); return 1; }
    nebula::Blob b;
    nebula::Status st = nebula::read_file(sh.session, argv[o.index], &b);
    if (st != nebula::Status::Ok) { print_status(io, "cannot read", argv[o.index], st); return 1; }
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
        __builtin_memcpy(line, b.data + start, static_cast<usize>(len));
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
            io.err.str(argv[o.index]);
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
    {"echo", "echo [-n] [text...]", "print text, Unicode included", &cmd_echo},
    {"set", "set [name value...]", "show or set shell variables", &cmd_set},
    {"unset", "unset name...", "remove shell variables", &cmd_unset},
    {"run", "run [-k] file", "run the lines of a script file", &cmd_run},
    {"true", "true", "succeed", &cmd_true},
    {"false", "false", "fail", &cmd_false},
};

constexpr Table TABLE{"shell", COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0])};

}

void register_builtin_commands() { register_table(&TABLE); }

}
