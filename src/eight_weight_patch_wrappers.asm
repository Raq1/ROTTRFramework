option casemap:none

EXTERN VertexElemGetSizeHookCore:PROC
EXTERN RendererTypeToFormatHookCore:PROC

PUBLIC VertexElemGetSizeHook
PUBLIC RendererTypeToFormatHook

.code




VertexElemGetSizeHook PROC
    sub     rsp, 0A8h
    mov     [rsp+20h], r8
    mov     [rsp+28h], r9
    mov     [rsp+30h], r10
    mov     [rsp+38h], r11
    movdqu  xmmword ptr [rsp+40h], xmm0
    movdqu  xmmword ptr [rsp+50h], xmm1
    movdqu  xmmword ptr [rsp+60h], xmm2
    movdqu  xmmword ptr [rsp+70h], xmm3
    movdqu  xmmword ptr [rsp+80h], xmm4
    movdqu  xmmword ptr [rsp+90h], xmm5
    call    VertexElemGetSizeHookCore
    movdqu  xmm0, xmmword ptr [rsp+40h]
    movdqu  xmm1, xmmword ptr [rsp+50h]
    movdqu  xmm2, xmmword ptr [rsp+60h]
    movdqu  xmm3, xmmword ptr [rsp+70h]
    movdqu  xmm4, xmmword ptr [rsp+80h]
    movdqu  xmm5, xmmword ptr [rsp+90h]
    mov     r8,  [rsp+20h]
    mov     r9,  [rsp+28h]
    mov     r10, [rsp+30h]
    mov     r11, [rsp+38h]
    add     rsp, 0A8h
    ret
VertexElemGetSizeHook ENDP

RendererTypeToFormatHook PROC
    sub     rsp, 0A8h
    mov     [rsp+20h], r8
    mov     [rsp+28h], r9
    mov     [rsp+30h], r10
    mov     [rsp+38h], r11
    movdqu  xmmword ptr [rsp+40h], xmm0
    movdqu  xmmword ptr [rsp+50h], xmm1
    movdqu  xmmword ptr [rsp+60h], xmm2
    movdqu  xmmword ptr [rsp+70h], xmm3
    movdqu  xmmword ptr [rsp+80h], xmm4
    movdqu  xmmword ptr [rsp+90h], xmm5
    call    RendererTypeToFormatHookCore
    movdqu  xmm0, xmmword ptr [rsp+40h]
    movdqu  xmm1, xmmword ptr [rsp+50h]
    movdqu  xmm2, xmmword ptr [rsp+60h]
    movdqu  xmm3, xmmword ptr [rsp+70h]
    movdqu  xmm4, xmmword ptr [rsp+80h]
    movdqu  xmm5, xmmword ptr [rsp+90h]
    mov     r8,  [rsp+20h]
    mov     r9,  [rsp+28h]
    mov     r10, [rsp+30h]
    mov     r11, [rsp+38h]
    add     rsp, 0A8h
    ret
RendererTypeToFormatHook ENDP

END
