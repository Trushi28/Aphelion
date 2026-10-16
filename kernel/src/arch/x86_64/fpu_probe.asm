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

section .note.GNU-stack noalloc noexec nowrite
