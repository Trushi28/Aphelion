typedef unsigned long long u64;
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
extern char __ImageBase;
unsigned _tls_index = 0;
__attribute__((section(".tls"))) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"))) char _tls_end = 0;
__declspec(thread) unsigned tls_value = 0x1234;
__declspec(thread) unsigned tls_zero[64];
unsigned first_attach = 0;
unsigned second_attach = 0;
unsigned module_ok = 0;

void __stdcall first_callback(void* module, unsigned reason, void* reserved) {
    (void)reserved;
    if (reason == 1) {
        first_attach++;
        if (module == (void*)&__ImageBase) module_ok++;
    }
}

void __stdcall second_callback(void* module, unsigned reason, void* reserved) {
    (void)module;
    (void)reserved;
    if (reason == 1) second_attach += 100;
}

const u64 callbacks[3] = { (u64)first_callback, (u64)second_callback, 0 };
typedef struct { u64 start, end, index, callbacks; unsigned zero, chars; } TLSDIR;
__attribute__((section(".rdata$T"))) const TLSDIR _tls_used = {(u64)&_tls_start, (u64)&_tls_end, (u64)&_tls_index, (u64)callbacks, 0, 0};

void mainCRTStartup(void) {
    unsigned code = 0;
    if (first_attach != 1) code |= 1;
    if (second_attach != 100) code |= 2;
    if (module_ok != 1) code |= 4;
    if (tls_value != 0x1234) code |= 8;
    tls_value++;
    if (tls_value != 0x1235) code |= 16;
    for (int i = 0; i < 64; i++) if (tls_zero[i] != 0) code |= 32;
    tls_zero[63] = 7;
    if (tls_zero[63] != 7) code |= 64;
    ExitProcess(code);
}
