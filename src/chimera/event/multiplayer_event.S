;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

.globl _on_multiplayer_event_asm
_on_multiplayer_event_asm:
    ;# The outer write_jmp_call trampoline already preserves flags/registers.
    ;# Capture the engine's event registers before Halo pushes its own arguments.
    push edi
    push ecx
    push eax
    push edx
    call _do_multiplayer_event
    add esp, 16
    ret
