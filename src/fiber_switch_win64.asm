; Cooperative Windows x64 switch. Offsets match the checked FiberContext layout.
; TEB StackBase/StackLimit/FiberData and x64 DeallocationStack accompany RSP.
; Hardware shadow-stack processes use the OS fiber backend instead.
PUBLIC mirabilis_fiber_switch
PUBLIC mirabilis_fiber_start
.code
mirabilis_fiber_switch PROC
    mov [rcx+0], rsp
    lea rax, resume_fiber
    mov [rcx+8], rax
    mov [rcx+16], rbx
    mov [rcx+24], rbp
    mov [rcx+32], rsi
    mov [rcx+40], rdi
    mov [rcx+48], r12
    mov [rcx+56], r13
    mov [rcx+64], r14
    mov [rcx+72], r15
    mov rax, gs:[30h]
    mov r8, [rax+08h]
    mov [rcx+80], r8
    mov r8, [rax+10h]
    mov [rcx+88], r8
    mov r8, [rax+1478h]
    mov [rcx+96], r8
    mov r8, [rax+20h]
    mov [rcx+104], r8
    stmxcsr [rcx+112]
    fnstcw [rcx+116]
    movaps [rcx+128], xmm6
    movaps [rcx+144], xmm7
    movaps [rcx+160], xmm8
    movaps [rcx+176], xmm9
    movaps [rcx+192], xmm10
    movaps [rcx+208], xmm11
    movaps [rcx+224], xmm12
    movaps [rcx+240], xmm13
    movaps [rcx+256], xmm14
    movaps [rcx+272], xmm15
    mov r8, [rdx+80]
    mov [rax+08h], r8
    mov r8, [rdx+88]
    mov [rax+10h], r8
    mov r8, [rdx+96]
    mov [rax+1478h], r8
    mov r8, [rdx+104]
    mov [rax+20h], r8
    ldmxcsr [rdx+112]
    fldcw [rdx+116]
    movaps xmm6, [rdx+128]
    movaps xmm7, [rdx+144]
    movaps xmm8, [rdx+160]
    movaps xmm9, [rdx+176]
    movaps xmm10, [rdx+192]
    movaps xmm11, [rdx+208]
    movaps xmm12, [rdx+224]
    movaps xmm13, [rdx+240]
    movaps xmm14, [rdx+256]
    movaps xmm15, [rdx+272]
    mov rbx, [rdx+16]
    mov rbp, [rdx+24]
    mov rsi, [rdx+32]
    mov rdi, [rdx+40]
    mov r12, [rdx+48]
    mov r13, [rdx+56]
    mov r14, [rdx+64]
    mov r15, [rdx+72]
    mov rsp, [rdx+0]
    jmp qword ptr [rdx+8]
resume_fiber:
    ret
mirabilis_fiber_switch ENDP

mirabilis_fiber_start PROC FRAME
    sub rsp, 40
    .allocstack 40
    .endprolog
    mov rcx, r13
    call r12
    int 3
mirabilis_fiber_start ENDP
END
