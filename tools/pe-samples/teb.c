typedef unsigned long long u64;
typedef unsigned short u16;
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
extern char __ImageBase;
unsigned entry_alignment = 99;

__asm__(".text\n"
        ".globl mainCRTStartup\n"
        "mainCRTStartup:\n"
        "    movq %rsp, %rax\n"
        "    andl $15, %eax\n"
        "    movl %eax, entry_alignment(%rip)\n"
        "    subq $40, %rsp\n"
        "    call checks\n"
        "    ud2\n");

static u64 gs64(unsigned off) {
    u64 v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"((u64)off));
    return v;
}

void checks(void) {
    unsigned code = 0;
    u64 teb = gs64(0x30);
    u64 peb = gs64(0x60);
    u64 sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    if (teb == 0) code |= 1;
    if (*(u64*)(peb + 0x10) != (u64)&__ImageBase) code |= 2;
    if (*(u16*)&__ImageBase != 0x5A4D) code |= 4;
    if (!(sp < gs64(0x08) && sp > gs64(0x10))) code |= 8;
    if (gs64(0x58) == 0) code |= 16;
    if (gs64(0x40) == 0) code |= 32;
    if (entry_alignment != 8) code |= 64;
    if (gs64(0x08) - gs64(0x10) < 0x10000) code |= 128;
    ExitProcess(code);
}
