#include <begin.hpp>
#include <cosmos/fileabi.hpp>

namespace {

constexpr u64 CHUNK = 256;

void emit(const char* data, u64 length) {
    while (length) {
        u64 n = length < CHUNK ? length : CHUNK;
        sys::debug_write(data, n);
        data += n;
        length -= n;
    }
}

}

extern "C" i64 aphelion_main(u64 channel, u64) {
    begin::Start start;
    if (channel == 0 || !begin::receive(channel, &start)) return 100;
    char line[startup::MAX_MESSAGE + 2];
    u64 used = 0;
    for (u32 i = 1; i < start.parsed.argc; ++i) {
        if (i > 1) line[used++] = ' ';
        for (const char* c = start.parsed.argv[i]; *c; ++c) line[used++] = *c;
    }
    line[used++] = '\n';
    emit(line, used);
    u64 input = start.handle[startup::STDIN];
    if (input != 0) {
        char buffer[fileabi::MAX_IO];
        for (;;) {
            i64 got = sys::file_read(input, 0, buffer, sizeof(buffer));
            if (got <= 0) break;
            emit(buffer, static_cast<u64>(got));
        }
        sys::handle_close(input);
    }
    return 0;
}
