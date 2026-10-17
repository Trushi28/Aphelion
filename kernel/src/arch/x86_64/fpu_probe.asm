bits 64
section .text

global fpu_probe
fpu_probe:
    push rbp
    mov rbp, rsp
    sub rsp, 96
    xor eax, eax

    fninit
    fld1
    fld1
    faddp st1, st0
    fistp dword [rsp]
    cmp dword [rsp], 2
    jne .after_x87
    or eax, 1
.after_x87:

    mov dword [rsp], 0x1F80
    ldmxcsr [rsp]
    mov rcx, 0x0000000200000001
    movq xmm0, rcx
    mov rdx, 0x0000000400000003
    movq xmm1, rdx
    paddd xmm0, xmm1
    movq rcx, xmm0
    mov rdx, 0x0000000600000004
    cmp rcx, rdx
    jne .after_sse
    or eax, 2
.after_sse:

    test rdi, rdi
    jz .done
    xor edx, edx
.fill:
    lea ecx, [rdx + 1]
    mov dword [rsp + rdx * 4], ecx
    inc edx
    cmp edx, 8
    jb .fill
    vmovups ymm0, [rsp]
    vmovups [rsp + 32], ymm0
    vzeroupper
    xor edx, edx
.check:
    mov ecx, [rsp + rdx * 4]
    cmp ecx, [rsp + 32 + rdx * 4]
    jne .done
    inc edx
    cmp edx, 8
    jb .check
    or eax, 4

.done:
    leave
    ret

%macro REGSET 3
    %2 %1 %+ 0, [rdi + 0 * %3]
    %2 %1 %+ 1, [rdi + 1 * %3]
    %2 %1 %+ 2, [rdi + 2 * %3]
    %2 %1 %+ 3, [rdi + 3 * %3]
    %2 %1 %+ 4, [rdi + 4 * %3]
    %2 %1 %+ 5, [rdi + 5 * %3]
    %2 %1 %+ 6, [rdi + 6 * %3]
    %2 %1 %+ 7, [rdi + 7 * %3]
    %2 %1 %+ 8, [rdi + 8 * %3]
    %2 %1 %+ 9, [rdi + 9 * %3]
    %2 %1 %+ 10, [rdi + 10 * %3]
    %2 %1 %+ 11, [rdi + 11 * %3]
    %2 %1 %+ 12, [rdi + 12 * %3]
    %2 %1 %+ 13, [rdi + 13 * %3]
    %2 %1 %+ 14, [rdi + 14 * %3]
    %2 %1 %+ 15, [rdi + 15 * %3]
%endmacro

%macro REGGET 3
    %2 [rdi + 0 * %3], %1 %+ 0
    %2 [rdi + 1 * %3], %1 %+ 1
    %2 [rdi + 2 * %3], %1 %+ 2
    %2 [rdi + 3 * %3], %1 %+ 3
    %2 [rdi + 4 * %3], %1 %+ 4
    %2 [rdi + 5 * %3], %1 %+ 5
    %2 [rdi + 6 * %3], %1 %+ 6
    %2 [rdi + 7 * %3], %1 %+ 7
    %2 [rdi + 8 * %3], %1 %+ 8
    %2 [rdi + 9 * %3], %1 %+ 9
    %2 [rdi + 10 * %3], %1 %+ 10
    %2 [rdi + 11 * %3], %1 %+ 11
    %2 [rdi + 12 * %3], %1 %+ 12
    %2 [rdi + 13 * %3], %1 %+ 13
    %2 [rdi + 14 * %3], %1 %+ 14
    %2 [rdi + 15 * %3], %1 %+ 15
%endmacro

global fpu_load_regs
fpu_load_regs:
    test rsi, rsi
    jz .sse
    REGSET ymm, vmovdqu, 32
    ret
.sse:
    REGSET xmm, movdqu, 16
    ret

global fpu_store_regs
fpu_store_regs:
    test rsi, rsi
    jz .sse
    REGGET ymm, vmovdqu, 32
    ret
.sse:
    REGGET xmm, movdqu, 16
    ret

global fpu_set_modes
fpu_set_modes:
    sub rsp, 8
    mov [rsp], di
    fldcw [rsp]
    mov [rsp], esi
    ldmxcsr [rsp]
    add rsp, 8
    ret

global fpu_get_modes
fpu_get_modes:
    sub rsp, 8
    fnstcw [rdi]
    stmxcsr [rsp]
    mov eax, [rsp]
    mov [rsi], eax
    add rsp, 8
    ret

section .note.GNU-stack noalloc noexec nowrite
