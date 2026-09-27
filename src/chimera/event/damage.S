;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text

.globl _on_damage_asm
_on_damage_asm:
    pushfd
    pushad

    ;# Stack at this point is the original stack minus pushfd/pushad.
    ;# Original apply-damage arguments:
    ;# +0x04 damage data
    ;# +0x08 target object ID
    ;# +0x0C node index
    ;# +0x10 region index
    ;# +0x14 material index
    mov eax, dword ptr [esp+0x30]
    mov ecx, dword ptr [esp+0x34]
    mov edx, dword ptr [esp+0x38]
    mov ebx, dword ptr [esp+0x24]

    ;# do_damage_event(damage, &object, node, region, material, return_address)
    push ebx
    push edx
    push ecx
    push eax
    lea eax, dword ptr [esp+0x3C]
    push eax
    push dword ptr [esp+0x3C]
    call _do_damage_event
    add esp, 24

    test al, al
    jz damage_denied

    ;# 1 = post-damage result requested; 2 = direct original execution.
    cmp al, 1
    jne damage_direct

    popad
    popfd

    ;# The trampoline must enter with the exact stack layout of the original
    ;# function. Replace its return address without changing registers/ESP.
    push eax
    lea eax, dword ptr damage_result_return
    mov dword ptr [esp+4], eax
    pop eax
    jmp dword ptr [_do_continue_damage_effect]

damage_direct:
    popad
    popfd
    jmp dword ptr [_do_continue_damage_effect]

damage_denied:
    popad
    popfd
    ret

damage_result_return:
    ;# Halo's original routine has completed. ESP now points at its first
    ;# argument because the synthetic return address was consumed.
    ;# Reserve a slot for the real return address and preserve all registers.
    sub esp, 4
    push eax
    pushfd
    pushad

    call _do_damage_result_event

    ;# Save the real return address in the reserved slot (40 bytes above ESP).
    mov dword ptr [esp+0x28], eax

    popad
    popfd
    pop eax
    ret
