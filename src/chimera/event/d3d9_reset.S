;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

.globl _on_d3d9_reset_asm
_on_d3d9_reset_asm:
    pushfd
    pushad

    push dword ptr [esp+0x2C]
    push dword ptr [esp+0x2C]
    call _do_d3d9_reset_event
    add esp, 8

    popad
    popfd
    ret
