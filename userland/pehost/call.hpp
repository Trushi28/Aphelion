#pragma once
#include <cosmos/types.hpp>

asm(R"(
.text
.globl pehost_call3
.type pehost_call3,@function
pehost_call3:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    movq %rsp, %rbx
    movq %r8, %r9
    movq %rcx, %r8
    movq %rsi, %rcx
    movq %rdi, %rax
    testq %r9, %r9
    jz 1f
    movq %r9, %rsp
1:
    andq $-16, %rsp
    subq $32, %rsp
    callq *%rax
    movq %rbx, %rsp
    popq %rbx
    popq %rbp
    retq
.size pehost_call3, .-pehost_call3
)");

extern "C" u64 pehost_call3(void* function, u64 first, u64 second, u64 third, void* stack_top);
