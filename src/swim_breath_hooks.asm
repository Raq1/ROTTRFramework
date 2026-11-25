option casemap:none

EXTERN g_swimOverrideActive:BYTE
EXTERN g_swimSinPitch:REAL4
EXTERN g_swimCosMinusOne:REAL4
EXTERN g_swimCamFwdX:REAL4
EXTERN g_swimCamFwdY:REAL4
EXTERN g_swimVzExtra:REAL4
EXTERN g_swimVzMin:REAL4
EXTERN g_swimVzMax:REAL4
EXTERN g_swimVerticalTrampoline:QWORD
EXTERN g_swimVerticalResume:QWORD

PUBLIC SwimVerticalStub

.code

; MinHook detour at PSCAF_P_Move_AdjustWadeSpeed+0x84E (retail 0x6A046E), the start of the deep-water
; target-plane block, entered by jmp with the function's own frame (rsp unchanged):
;   [rsp+50h] desired velocity x,y,z (after the forward-probe speed curve)
;   [rsp+60h] identical copy here (water current is added to [rsp+50h] only after 0x6A074C).
; Override active: the part of the desired velocity along the camera's horizontal forward (f) is tilted
; by the camera pitch: xmm7 = clamp(f * sin(pitch) + keys, min, max), xy -= camFwd * f * (1 - cos(pitch)).
; The block (target plane float/sink, snap, ceiling-ahead follow) is skipped by resuming at 0x6A0745,
; which stores xmm7 as desired.z. Clobbers only xmm0-xmm3 and xmm7, all dead at the resume point.
SwimVerticalStub PROC
    cmp     byte ptr [g_swimOverrideActive], 0
    je      swim_passthrough

    movss   xmm0, dword ptr [rsp+50h]
    movss   xmm1, dword ptr [rsp+54h]
    movaps  xmm2, xmm0
    mulss   xmm2, dword ptr [g_swimCamFwdX]
    movaps  xmm3, xmm1
    mulss   xmm3, dword ptr [g_swimCamFwdY]
    addss   xmm2, xmm3
    movaps  xmm7, xmm2
    mulss   xmm7, dword ptr [g_swimSinPitch]
    addss   xmm7, dword ptr [g_swimVzExtra]
    minss   xmm7, dword ptr [g_swimVzMax]
    maxss   xmm7, dword ptr [g_swimVzMin]

    mulss   xmm2, dword ptr [g_swimCosMinusOne]
    movaps  xmm3, xmm2
    mulss   xmm3, dword ptr [g_swimCamFwdX]
    addss   xmm0, xmm3
    mulss   xmm2, dword ptr [g_swimCamFwdY]
    addss   xmm1, xmm2
    movss   dword ptr [rsp+50h], xmm0
    movss   dword ptr [rsp+54h], xmm1
    movss   dword ptr [rsp+60h], xmm0
    movss   dword ptr [rsp+64h], xmm1
    jmp     qword ptr [g_swimVerticalResume]

swim_passthrough:
    jmp     qword ptr [g_swimVerticalTrampoline]
SwimVerticalStub ENDP

END
