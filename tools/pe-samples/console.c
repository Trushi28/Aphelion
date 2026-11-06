typedef unsigned long long u64;
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
__declspec(dllimport) void* __stdcall GetStdHandle(unsigned);
__declspec(dllimport) int __stdcall WriteFile(void*, const void*, unsigned, unsigned*, void*);
unsigned _tls_index = 0;
__attribute__((section(".tls"))) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"))) char _tls_end = 0;
__declspec(thread) int tls_counter = 5;
__declspec(thread) char tls_buf[100];
typedef struct { u64 start, end, index, callbacks; unsigned zero, chars; } TLSDIR;
__attribute__((section(".rdata$T"))) const TLSDIR _tls_used = {(u64)&_tls_start, (u64)&_tls_end, (u64)&_tls_index, 0, 0, 0};
int table[4] = {1, 2, 3, 4};
int* ptrs[4] = {&table[0], &table[1], &table[2], &table[3]};
const char msg[] = "hello from pe\n";
void mainCRTStartup(void) {
    unsigned n;
    tls_counter++;
    WriteFile(GetStdHandle(-11), msg, sizeof(msg) - 1, &n, 0);
    ExitProcess(*ptrs[2] + tls_counter);
}
