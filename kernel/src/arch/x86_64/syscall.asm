bits 64
section .text

extern syscall_dispatch
extern user_bad_return

global syscall_entry
global user_enter

USER_RSP_OFFSET equ 0
KERNEL_RSP_OFFSET equ 8
USER_DATA_SEL equ 0x1B
USER_CODE_SEL equ 0x23
SYSRET_KEEP equ 0xCD5
RFLAGS_INITIAL equ 0x202
FRAME_VECTOR_SYSCALL equ 0x100

syscall_entry:
    swapgs
    mov [gs:USER_RSP_OFFSET], rsp
    mov rsp, [gs:KERNEL_RSP_OFFSET]

    push qword USER_DATA_SEL
    push qword [gs:USER_RSP_OFFSET]
    push r11
    push qword USER_CODE_SEL
    push rcx
    push qword 0
    push qword FRAME_VECTOR_SYSCALL

    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call syscall_dispatch

    cli

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16

    mov rcx, [rsp]
    mov r11, rcx
    shr r11, 47
    jnz .bad_rip

    mov r11, [rsp + 16]
    and r11, SYSRET_KEEP
    or r11, RFLAGS_INITIAL
    mov rsp, [rsp + 24]
    swapgs
    o64 sysret

.bad_rip:
    and rsp, -16
    call user_bad_return
    ud2

user_enter:
    cli
    push qword USER_DATA_SEL
    push rsi
    push qword RFLAGS_INITIAL
    push qword USER_CODE_SEL
    push rdi
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    swapgs
    iretq

section .note.GNU-stack noalloc noexec nowrite
