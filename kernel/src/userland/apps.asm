default rel

section .rodata
align 16

%macro embed 2
global %1_start
global %1_end
%1_start:
    incbin %2
%1_end:
%endmacro

embed apx_hello, "userland/hello.apx"
embed apx_pehost, "userland/pehost.apx"
embed pe_console, "console.exe"
embed pe_k32, "k32.exe"
embed pe_teb, "teb.exe"
embed pe_tlscb, "tlscb.exe"

section .note.GNU-stack noalloc noexec nowrite
