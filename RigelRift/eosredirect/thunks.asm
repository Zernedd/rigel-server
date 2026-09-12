; thunks.asm - ordinal-forwarding thunks for the dsound.dll disguise.
;
; The game's A2-Win64-Shipping.exe binds dsound.dll by ORDINAL (1,3,6,8,11,12), never by name. So the
; proxy only has to expose those ordinals and forward each to the SAME ordinal in the real system
; dsound.dll. These thunks are signature-agnostic: they tail-jump through a resolved pointer, so any
; argument shape and calling convention passes through untouched. DllMain fills g_realN before the game
; can call anything (dsound is a static import, resolved at load; our DllMain runs during that load).
;
; Ordinals 1..12 are all exported (exports.def) even though only six are imported, so the proxy still
; satisfies the loader if the build's import set ever changes.

.data
PUBLIC g_real1, g_real2, g_real3, g_real4, g_real5, g_real6
PUBLIC g_real7, g_real8, g_real9, g_real10, g_real11, g_real12
g_real1  dq 0
g_real2  dq 0
g_real3  dq 0
g_real4  dq 0
g_real5  dq 0
g_real6  dq 0
g_real7  dq 0
g_real8  dq 0
g_real9  dq 0
g_real10 dq 0
g_real11 dq 0
g_real12 dq 0

.code
Thunk1  PROC
    jmp qword ptr [g_real1]
Thunk1  ENDP
Thunk2  PROC
    jmp qword ptr [g_real2]
Thunk2  ENDP
Thunk3  PROC
    jmp qword ptr [g_real3]
Thunk3  ENDP
Thunk4  PROC
    jmp qword ptr [g_real4]
Thunk4  ENDP
Thunk5  PROC
    jmp qword ptr [g_real5]
Thunk5  ENDP
Thunk6  PROC
    jmp qword ptr [g_real6]
Thunk6  ENDP
Thunk7  PROC
    jmp qword ptr [g_real7]
Thunk7  ENDP
Thunk8  PROC
    jmp qword ptr [g_real8]
Thunk8  ENDP
Thunk9  PROC
    jmp qword ptr [g_real9]
Thunk9  ENDP
Thunk10 PROC
    jmp qword ptr [g_real10]
Thunk10 ENDP
Thunk11 PROC
    jmp qword ptr [g_real11]
Thunk11 ENDP
Thunk12 PROC
    jmp qword ptr [g_real12]
Thunk12 ENDP

END
