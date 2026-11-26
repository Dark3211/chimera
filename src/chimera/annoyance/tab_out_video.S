;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

.globl _tab_out_video_reacquire_device_subroutine_asm
_tab_out_video_reacquire_device_subroutine_asm:
    mov eax, dword ptr [_tab_out_video_disable_video_ptr]
    mov al, byte ptr [eax]
    test al, al
    jz _tab_out_video_reacquire_device_subroutine_original_asm

    ;# If D3D9 is ready for Reset, let Halo's original recovery routine run even
    ;# if the background-playback flag has not yet been cleared.
    push eax
    push ecx
    push edx
    call _tab_out_video_device_requires_reset
    test eax, eax
    pop edx
    pop ecx
    pop eax
    jz _tab_out_video_reacquire_device_subroutine_skip_reset
    test al, al
    jmp _tab_out_video_reacquire_device_subroutine_original_asm

.globl _tab_out_video_reacquire_device_subroutine_skip_reset
_tab_out_video_reacquire_device_subroutine_skip_reset:
    ;# Device is still lost or video remains intentionally disabled. Keep the
    ;# original fast path and avoid calling ResetVideoDevice prematurely.
    xor al, al
    ;# Since the hook is partially into the function, we need to properly clean up after ourselves.
    pop ebx
    add esp, 0x38
    ret

.globl _tab_out_video_reacquire_device_subroutine_original_asm
_tab_out_video_reacquire_device_subroutine_original_asm:
    ;# Video is not disabled, undo the hook and proceed as normal
    jmp dword ptr [_tab_out_video_device_subroutine_original]
