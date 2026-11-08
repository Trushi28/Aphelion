#include "apxpack.hpp"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: apxpack program.elf program.apx\n");
        return 2;
    }
    FILE* in = fopen(argv[1], "rb");
    if (!in) {
        fprintf(stderr, "apxpack: cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(in, 0, SEEK_END);
    long n = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (n < 0) {
        fclose(in);
        return 1;
    }
    u8* elf = static_cast<u8*>(malloc(static_cast<size_t>(n) + 1));
    size_t got = elf ? fread(elf, 1, static_cast<size_t>(n), in) : 0;
    fclose(in);
    if (!elf || got != static_cast<size_t>(n)) {
        fprintf(stderr, "apxpack: cannot read %s\n", argv[1]);
        return 1;
    }
    size_t cap = static_cast<size_t>(apx::MAX_FILE_BYTES);
    u8* out = static_cast<u8*>(malloc(cap));
    usize written = 0;
    apxpack::Status st = out ? apxpack::convert(elf, static_cast<usize>(n), out, cap, &written) : apxpack::Status::PackFailed;
    if (st != apxpack::Status::Ok) {
        fprintf(stderr, "apxpack: %s: %s\n", argv[1], apxpack::status_name(st));
        return 1;
    }
    apx::Parsed check = apx::parse(out, written);
    if (check.status != apx::Status::Ok) {
        fprintf(stderr, "apxpack: the packed image does not parse: %s\n", apx::status_name(check.status));
        return 1;
    }
    FILE* f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 1, written, f) != written || fclose(f) != 0) {
        fprintf(stderr, "apxpack: cannot write %s\n", argv[2]);
        return 1;
    }
    printf("apxpack: %s -> %s, %zu bytes, %u data page(s)\n", argv[1], argv[2], static_cast<size_t>(written), check.header.data_pages);
    return 0;
}
