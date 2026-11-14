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
embed apx_echo, "userland/echo.apx"
embed apx_pehost, "userland/pehost.apx"
embed pe_console, "console.exe"
embed pe_k32, "k32.exe"
embed pe_teb, "teb.exe"
embed pe_tlscb, "tlscb.exe"
embed pe_stdio, "stdio.exe"
embed pe_crt, "crt.exe"
embed pe_ucrt, "ucrt.exe"
embed pe_dllmath, "dllmath.dll"
embed pe_dllchain, "dllchain.dll"
embed pe_dllplug, "dllplug.dll"
embed pe_dllfail, "dllfail.dll"
embed pe_dyn, "dyn.exe"
embed pe_sysio, "sysio.exe"
embed pe_argv, "argv.exe"
embed pe_files, "files.exe"
embed pe_threads, "threads.exe"
embed pe_thrdll, "thrdll.dll"

section .note.GNU-stack noalloc noexec nowrite
