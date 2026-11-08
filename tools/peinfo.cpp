#include <cosmos/pe.hpp>

extern "C" {
struct _IO_FILE;
typedef struct _IO_FILE FILE;
int printf(const char*, ...);
int dprintf(int, const char*, ...);
FILE* fopen(const char*, const char*);
usize fread(void*, usize, usize, FILE*);
int fclose(FILE*);
void* malloc(usize);
void free(void*);
}

namespace {

constexpr u32 MAX_ENTRIES = 8192;
constexpr u32 MAX_SUPPORTED = 16384;
constexpr u32 TEXT = 160;

struct Entry {
    char dll[TEXT];
    char name[TEXT];
    u16 ordinal;
    bool by_ordinal;
};

Entry g_entries[MAX_ENTRIES];
u32 g_entry_count = 0;
u32 g_entry_total = 0;
char g_supported[MAX_SUPPORTED][TEXT];
u32 g_supported_count = 0;

bool equal(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

char fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

void put(char* dst, u32& n, char c) {
    if (n + 1 < TEXT) dst[n++] = c;
}

void label(const Entry& e, char* out) {
    u32 n = 0;
    for (const char* p = e.dll; *p; ++p) put(out, n, fold(*p));
    put(out, n, '!');
    if (e.by_ordinal) {
        put(out, n, '#');
        char digits[8];
        u32 d = 0;
        u32 v = e.ordinal;
        do {
            digits[d++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (d) put(out, n, digits[--d]);
    } else {
        for (const char* p = e.name; *p; ++p) put(out, n, *p);
    }
    out[n] = 0;
}

bool is_supported(const Entry& e) {
    char l[TEXT];
    label(e, l);
    for (u32 i = 0; i < g_supported_count; ++i) {
        if (equal(g_supported[i], l)) return true;
    }
    return false;
}

bool collect(void*, const char* dll, const char* name, u16 ordinal, u64* address) {
    *address = 0x1000;
    Entry e{};
    u32 n = 0;
    for (const char* p = dll; *p; ++p) put(e.dll, n, fold(*p));
    e.dll[n] = 0;
    e.by_ordinal = name == nullptr;
    e.ordinal = ordinal;
    n = 0;
    if (name) {
        for (const char* p = name; *p; ++p) put(e.name, n, *p);
    }
    e.name[n] = 0;
    for (u32 i = 0; i < g_entry_count; ++i) {
        const Entry& o = g_entries[i];
        if (equal(o.dll, e.dll) && o.by_ordinal == e.by_ordinal && o.ordinal == e.ordinal && equal(o.name, e.name)) return true;
    }
    ++g_entry_total;
    if (g_entry_count < MAX_ENTRIES) g_entries[g_entry_count++] = e;
    return true;
}

u8* slurp(const char* path, usize* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    usize cap = 1 << 16, len = 0;
    u8* buf = static_cast<u8*>(malloc(cap));
    for (;;) {
        if (len == cap) {
            u8* bigger = static_cast<u8*>(malloc(cap * 2));
            for (usize i = 0; i < len; ++i) bigger[i] = buf[i];
            free(buf);
            buf = bigger;
            cap *= 2;
        }
        usize n = fread(buf + len, 1, cap - len, f);
        if (n == 0) break;
        len += n;
    }
    fclose(f);
    *size = len;
    return buf;
}

const char* prot_text(u32 p) {
    if (p & pe::PROT_EXEC) return "r-x";
    if (p & pe::PROT_WRITE) return "rw-";
    return "r--";
}

const char* subsystem_text(u16 s) {
    switch (s) {
        case 1: return "native";
        case 2: return "windows gui";
        case 3: return "windows console";
        case 9: return "windows ce";
        case 10: return "efi application";
        default: return "other subsystem";
    }
}

bool load_supported(const char* path) {
    usize size = 0;
    u8* text = slurp(path, &size);
    if (!text) return false;
    char line[TEXT];
    u32 n = 0;
    for (usize i = 0; i <= size; ++i) {
        char c = i < size ? static_cast<char>(text[i]) : '\n';
        if (c == '\n') {
            line[n] = 0;
            if (n > 0 && line[0] != '#' && g_supported_count < MAX_SUPPORTED) {
                u32 k = 0;
                for (u32 j = 0; line[j]; ++j) g_supported[g_supported_count][k++] = fold(line[j]);
                g_supported[g_supported_count][k] = 0;
                ++g_supported_count;
            }
            n = 0;
        } else if (c != '\r' && n + 1 < TEXT) {
            line[n++] = c;
        }
    }
    free(text);
    return true;
}

pe::Image g_image;
pe::Run g_runs[pe::MAX_RUNS];

}

int main(int argc, char** argv) {
    const char* path = nullptr;
    const char* supported_path = nullptr;
    bool verbose = false;
    bool bad = false;
    for (int i = 1; i < argc; ++i) {
        if (equal(argv[i], "-v")) verbose = true;
        else if (equal(argv[i], "--supported") && i + 1 < argc) supported_path = argv[++i];
        else if (!path) path = argv[i];
        else bad = true;
    }
    if (!path || bad) {
        dprintf(2, "usage: peinfo [-v] [--supported list.txt] program.exe\n");
        return 2;
    }
    usize size = 0;
    u8* file = slurp(path, &size);
    if (!file) {
        dprintf(2, "peinfo: cannot read %s\n", path);
        return 2;
    }

    pe::Status st = pe::parse(file, size, &g_image);
    const pe::Image& img = g_image;
    printf("file            %s (%llu bytes)\n", path, static_cast<unsigned long long>(size));
    if (st != pe::Status::Ok) {
        printf("verdict         REFUSED: %s\n", pe::status_name(st));
        free(file);
        return 1;
    }
    printf("kind            %s, %s\n", img.is_dll ? "DLL" : "program", subsystem_text(img.subsystem));
    printf("image base      0x%llx\n", static_cast<unsigned long long>(img.image_base));
    printf("entry           rva 0x%x\n", img.entry_rva);
    printf("image size      0x%x (headers 0x%x, section align 0x%x, file align 0x%x)\n", img.size_of_image, img.size_of_headers, img.section_alignment, img.file_alignment);
    printf("stack           reserve 0x%llx, commit 0x%llx\n", static_cast<unsigned long long>(img.stack_reserve), static_cast<unsigned long long>(img.stack_commit));
    printf("relocations     %s, dynamic base %s, %s\n", img.has_relocations ? "table present" : "no table", (img.dll_characteristics & 0x40) ? "set" : "clear",
           img.relocatable ? "can be moved" : "fixed at its preferred base");

    printf("\nsections\n");
    for (u32 i = 0; i < img.section_count; ++i) {
        const pe::Section& s = img.sections[i];
        printf("  %-8s rva 0x%06x  size 0x%06x  raw 0x%06x  %s\n", s.name, s.rva, s.virtual_size, s.raw_size, prot_text(s.prot));
    }
    printf("\npage runs       %u\n", pe::plan(img, g_runs, pe::MAX_RUNS));

    u8* image = static_cast<u8*>(malloc(img.size_of_image));
    pe::Status ms = pe::map(img, file, size, image, img.size_of_image);
    if (ms != pe::Status::Ok) {
        printf("verdict         REFUSED: %s\n", pe::status_name(ms));
        free(image);
        free(file);
        return 1;
    }
    const u64 test_base = 0x200000000ull;
    u32 applied = 0;
    pe::Status rs = pe::relocate(img, image, test_base, &applied);
    bool relocatable = rs == pe::Status::Ok;
    if (relocatable) printf("relocated       %u entries applied at 0x%llx%s\n", applied, static_cast<unsigned long long>(test_base), img.has_relocations ? "" : " (position independent, nothing to patch)");
    else printf("relocated       REFUSED: %s (Aphelion cannot map this image at its preferred base)\n", pe::status_name(rs));

    pe::Tls tls{};
    pe::Status ts = pe::tls_info(img, image, relocatable ? test_base : img.image_base, &tls);
    if (ts != pe::Status::Ok) printf("tls             REFUSED: %s\n", pe::status_name(ts));
    else if (tls.present) printf("tls             template %u bytes, zero fill %u, %u callback(s)\n", tls.template_bytes, tls.zero_fill, tls.callback_count);
    else printf("tls             none\n");

    pe::ImportReport report{};
    pe::Status bs = pe::bind_imports(img, image, collect, nullptr, &report);
    if (bs != pe::Status::Ok) {
        printf("imports         REFUSED: %s\n", pe::status_name(bs));
        printf("verdict         REFUSED: %s\n", pe::status_name(bs));
        free(image);
        free(file);
        return 1;
    }
    if (supported_path && !load_supported(supported_path)) {
        dprintf(2, "peinfo: cannot read %s\n", supported_path);
        free(image);
        free(file);
        return 2;
    }

    printf("\nimports         %u unique from %u librar%s\n", g_entry_total, report.libraries, report.libraries == 1 ? "y" : "ies");
    if (g_entry_total > g_entry_count) printf("                (only the first %u are listed)\n", g_entry_count);
    u32 missing = 0;
    for (u32 i = 0; i < g_entry_count; ++i) {
        bool first = true;
        for (u32 j = 0; j < i; ++j) {
            if (equal(g_entries[j].dll, g_entries[i].dll)) first = false;
        }
        if (!first) continue;
        u32 total = 0, lacking = 0;
        for (u32 j = i; j < g_entry_count; ++j) {
            if (!equal(g_entries[j].dll, g_entries[i].dll)) continue;
            ++total;
            if (supported_path && !is_supported(g_entries[j])) ++lacking;
        }
        missing += lacking;
        if (supported_path) printf("  %-24s %4u imports, %u not provided\n", g_entries[i].dll, total, lacking);
        else printf("  %-24s %4u imports\n", g_entries[i].dll, total);
        for (u32 j = i; j < g_entry_count; ++j) {
            if (!equal(g_entries[j].dll, g_entries[i].dll)) continue;
            bool lacks = supported_path && !is_supported(g_entries[j]);
            if (!verbose && !lacks) continue;
            if (g_entries[j].by_ordinal) printf("    %s#%u\n", lacks ? "MISSING " : "", g_entries[j].ordinal);
            else printf("    %s%s\n", lacks ? "MISSING " : "", g_entries[j].name);
        }
    }
    printf("\nverdict         ");
    if (!relocatable) printf("parses, but cannot run: it is fixed at a preferred base outside the user window\n");
    else if (supported_path) printf("loader accepts the image; %u import(s) not provided by the executor\n", missing);
    else printf("loader accepts the image\n");
    free(image);
    free(file);
    return relocatable && missing == 0 ? 0 : 1;
}
