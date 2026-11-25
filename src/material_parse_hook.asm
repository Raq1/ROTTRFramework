; Mid-function hook inside cdc::MaterialSection::HandleResourceReady (retail RVA 0x344DF7):
;   rcx = CommonMaterial*, rdx = material data, r8d = resource id; the displaced code is
;   "mov rax,[rcx]" + "call [rax]" (SetMaterialData).
; The C++ handler is free to use every volatile register, so all of them (and the flags) are
; put back before the displaced code runs. The stack is aligned here, whatever it was on entry.

option casemap:none

EXTERN MaterialParseIdCaptureHandler:PROC
EXTERN g_materialParseIdTrampoline:QWORD
PUBLIC MaterialParseIdHookStub

.code
MaterialParseIdHookStub PROC
    push rbp
    mov  rbp, rsp
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub  rsp, 80h                       ; 20h home space + 60h for xmm0-xmm5
    and  rsp, -16
    movups xmmword ptr [rsp+20h], xmm0
    movups xmmword ptr [rsp+30h], xmm1
    movups xmmword ptr [rsp+40h], xmm2
    movups xmmword ptr [rsp+50h], xmm3
    movups xmmword ptr [rsp+60h], xmm4
    movups xmmword ptr [rsp+70h], xmm5

    mov  ecx, r8d                       ; handler(int materialId, void* data)
    call MaterialParseIdCaptureHandler

    movups xmm0, xmmword ptr [rsp+20h]
    movups xmm1, xmmword ptr [rsp+30h]
    movups xmm2, xmmword ptr [rsp+40h]
    movups xmm3, xmmword ptr [rsp+50h]
    movups xmm4, xmmword ptr [rsp+60h]
    movups xmm5, xmmword ptr [rsp+70h]
    lea  rsp, [rbp-40h]                 ; back to the pushed r11
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rax
    popfq
    pop  rbp
    jmp  qword ptr [g_materialParseIdTrampoline]
MaterialParseIdHookStub ENDP
END
