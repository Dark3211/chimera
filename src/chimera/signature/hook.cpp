// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>
#include <iostream>
#include <cstring>
#include <string>
#include <limits>
#include <new>

#include "signature.hpp"
#include "hook.hpp"

namespace Chimera {
    static bool is_memory_range_valid(const void *address, std::size_t length, bool require_executable) noexcept {
        if(!address || length == 0) {
            return false;
        }
        const auto begin = reinterpret_cast<std::uintptr_t>(address);
        if(begin > std::numeric_limits<std::uintptr_t>::max() - length) {
            return false;
        }
        const auto end = begin + length;
        auto cursor = begin;
        while(cursor < end) {
            MEMORY_BASIC_INFORMATION information {};
            if(VirtualQuery(reinterpret_cast<const void *>(cursor), &information, sizeof(information)) != sizeof(information)) {
                return false;
            }
            if(information.State != MEM_COMMIT || (information.Protect & PAGE_GUARD) != 0 ||
               (information.Protect & PAGE_NOACCESS) != 0) {
                return false;
            }
            if(require_executable) {
                const DWORD protection = information.Protect & 0xFFU;
                const bool executable = protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
                                        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
                if(!executable) {
                    return false;
                }
            }
            const auto region_begin = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
            if(region_begin > std::numeric_limits<std::uintptr_t>::max() - information.RegionSize) {
                return false;
            }
            const auto region_end = region_begin + information.RegionSize;
            if(region_end <= cursor) {
                return false;
            }
            cursor = region_end < end ? region_end : end;
        }
        return true;
    }

    bool is_readable_memory_range(const void *address, std::size_t length) noexcept {
        return is_memory_range_valid(address, length, false);
    }

    bool is_executable_memory_range(const void *address, std::size_t length) noexcept {
        return is_memory_range_valid(address, length, true);
    }

    void Hook::rollback() noexcept {
        if(this->original_bytes.empty()) {
            return;
        }
        if(this->address) {
            overwrite(this->address, this->original_bytes.data(), this->original_bytes.size());
        }
        this->original_bytes.clear();
    }

    static std::size_t relocated_size(const std::vector<std::byte> &bytes, const std::vector<std::uintptr_t> &offsets) noexcept {
        std::size_t size = bytes.size();
        for(std::size_t i = 0; i < offsets.size(); i++) {
            const auto offset = offsets[i];
            const auto opcode = *reinterpret_cast<const std::uint8_t *>(bytes.data() + offset);
            if(opcode == 0xEB || (opcode >= 0x70 && opcode <= 0x7F)) {
                // Short JMP/Jcc are expanded to a 32-bit relative JMP sequence.
                size += opcode == 0xEB ? 3 : 5;
            }
        }
        return size;
    }

    static std::vector<std::byte> relocate_instructions(const std::byte *source, const std::vector<std::byte> &bytes,
                                                          const std::vector<std::uintptr_t> &offsets,
                                                          std::byte *destination) {
        std::vector<std::size_t> source_to_destination(bytes.size(), std::numeric_limits<std::size_t>::max());
        std::size_t cursor = 0;
        for(std::size_t i = 0; i < offsets.size(); i++) {
            const std::size_t source_offset = offsets[i];
            const std::size_t next_offset = i + 1 < offsets.size() ? offsets[i + 1] : bytes.size();
            for(std::size_t j = source_offset; j < next_offset; j++) {
                source_to_destination[j] = cursor + (j - source_offset);
            }
            const auto opcode = *reinterpret_cast<const std::uint8_t *>(bytes.data() + source_offset);
            cursor += next_offset - source_offset;
            if(opcode == 0xEB || (opcode >= 0x70 && opcode <= 0x7F)) {
                cursor += opcode == 0xEB ? 3 : 5;
            }
        }

        const auto target_address = [&](const std::byte *target) -> const std::byte * {
            if(target >= source && target < source + bytes.size()) {
                const auto source_offset = static_cast<std::size_t>(target - source);
                if(source_to_destination[source_offset] != std::numeric_limits<std::size_t>::max()) {
                    return destination + source_to_destination[source_offset];
                }
            }
            if(target == source + bytes.size()) {
                return destination + cursor;
            }
            return target;
        };

        std::vector<std::byte> relocated;
        relocated.reserve(cursor);

        for(std::size_t i = 0; i < offsets.size(); i++) {
            const std::size_t source_offset = offsets[i];
            const std::size_t next_offset = i + 1 < offsets.size() ? offsets[i + 1] : bytes.size();
            const std::size_t instruction_size = next_offset - source_offset;
            const auto opcode = *reinterpret_cast<const std::uint8_t *>(bytes.data() + source_offset);
            const std::size_t destination_offset = relocated.size();
            relocated.insert(relocated.end(), bytes.begin() + source_offset, bytes.begin() + next_offset);

            if(opcode == 0xE8 && instruction_size == 5) {
                std::int32_t displacement;
                std::memcpy(&displacement, bytes.data() + source_offset + 1, sizeof(displacement));
                const auto *target = target_address(source + source_offset + 5 + displacement);
                const auto new_displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target)
                    - reinterpret_cast<std::intptr_t>(destination + destination_offset + 5));
                std::memcpy(relocated.data() + destination_offset + 1, &new_displacement, sizeof(new_displacement));
            }
            else if(opcode == 0xE9 && instruction_size == 5) {
                std::int32_t displacement;
                std::memcpy(&displacement, bytes.data() + source_offset + 1, sizeof(displacement));
                const auto *target = target_address(source + source_offset + 5 + displacement);
                const auto new_displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target)
                    - reinterpret_cast<std::intptr_t>(destination + destination_offset + 5));
                std::memcpy(relocated.data() + destination_offset + 1, &new_displacement, sizeof(new_displacement));
            }
            else if(opcode == 0x0F && instruction_size == 6) {
                const auto secondary = *reinterpret_cast<const std::uint8_t *>(bytes.data() + source_offset + 1);
                if(secondary >= 0x80 && secondary <= 0x8F) {
                    std::int32_t displacement;
                    std::memcpy(&displacement, bytes.data() + source_offset + 2, sizeof(displacement));
                    const auto *target = target_address(source + source_offset + 6 + displacement);
                    const auto new_displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target)
                        - reinterpret_cast<std::intptr_t>(destination + destination_offset + 6));
                    std::memcpy(relocated.data() + destination_offset + 2, &new_displacement, sizeof(new_displacement));
                }
            }
            else if(opcode == 0xEB && instruction_size == 2) {
                std::int8_t displacement;
                std::memcpy(&displacement, bytes.data() + source_offset + 1, sizeof(displacement));
                const auto *target = target_address(source + source_offset + 2 + displacement);
                relocated.resize(relocated.size() - 2);
                relocated.push_back(std::byte{0xE9});
                const auto new_displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target)
                    - reinterpret_cast<std::intptr_t>(destination + destination_offset + 5));
                const auto *disp = reinterpret_cast<const std::byte *>(&new_displacement);
                relocated.insert(relocated.end(), disp, disp + sizeof(new_displacement));
            }
            else if(opcode >= 0x70 && opcode <= 0x7F && instruction_size == 2) {
                std::int8_t displacement;
                std::memcpy(&displacement, bytes.data() + source_offset + 1, sizeof(displacement));
                const auto *target = target_address(source + source_offset + 2 + displacement);
                const auto inverse = static_cast<std::uint8_t>(opcode ^ 0x01);
                relocated.resize(relocated.size() - 2);
                relocated.push_back(static_cast<std::byte>(inverse));
                relocated.push_back(std::byte{0x05});
                relocated.push_back(std::byte{0xE9});
                const auto new_displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target)
                    - reinterpret_cast<std::intptr_t>(destination + destination_offset + 7));
                const auto *disp = reinterpret_cast<const std::byte *>(&new_displacement);
                relocated.insert(relocated.end(), disp, disp + sizeof(new_displacement));
            }
        }

        return relocated;
    }

    // Get the bytes to the instruction(s) at the given address. I'll modify this as more types of instructions are needed.
    static bool get_instructions(const std::byte *at_start, std::vector<std::byte> &bytes, std::vector<std::uintptr_t> &offsets, std::size_t minimum_size = 1) {
        bytes.clear();
        offsets.clear();
        if(!at_start) {
            return false;
        }
        const auto *at = at_start;

        // Keep adding instructions until we have what we need.
        while(bytes.size() < minimum_size) {
            const auto *instruction_start = at;
            switch(*reinterpret_cast<const std::uint8_t *>(at)) {
                // add eax, <val>
                case 0x05:
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 5);
                    at += 5;
                    break;

                // jmp <relative offset> or movsx
                case 0x0F: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    auto op2 = *reinterpret_cast<const std::uint8_t *>(at + 2);
                    if(op1 >= 0x80 && op1 <= 0x8F) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                        break;
                    }
                    else if(op1 == 0xBF || op1 == 0xB6 || op1 == 0xB7) {
                        if(op2 == 0x6E || op2 == 0x4E || op2 == 0x4B || op2 == 0x43) {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 4);
                            at += 4;
                        }
                        else if(op2 == 0x15) {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 7);
                            at += 7;
                        }
                        else if (op2 == 0x54 || op2 == 0x44) {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 5);
                            at += 5;
                        }
                        else {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 3);
                            at += 3;
                        }
                        break;
                    }
                    else {
                        return false;
                    }
                }

                // and <value>
                case 0x25: {
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 5);
                    at += 5;
                    break;
                }

                case 0x2B: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(op1 == 0x0D) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                        break;
                    }
                    return false;
                }

                // oxr <value>
                case 0x33: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(op1 == 0xDB || op1 == 0xF6 || op1 == 0xC9) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 2);
                        at += 2;
                        break;
                    }
                    return false;
                }

                // cmp ecx, something
                case 0x3B: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(op1 == 0xCD) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 2);
                        at += 2;
                        break;
                    }
                    return false;
                }

                // cmp eax, <value>
                case 0x3D: {
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 5);
                    at += 5;
                    break;
                }

                // push/pop <register>
                case 0x50: case 0x54: case 0x58: case 0x5C: case 0x60:
                case 0x51: case 0x55: case 0x59: case 0x5D: case 0x61:
                case 0x52: case 0x56: case 0x5A: case 0x5E:
                case 0x53: case 0x57: case 0x5B: case 0x5F:
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 1);
                    at += 1;
                    break;

                case 0x66: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    auto op2 = *reinterpret_cast<const std::uint8_t *>(at + 2);
                    // mov [reg]
                    if(op1 == 0x89) {
                        if(op2 == 0x45 || op2 == 0x4A) {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 4);
                            at += 4;
                        }
                        else {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 3);
                            at += 3;
                        }
                        break;
                    }
                    // mov word ptr [reg+op4]
                    else if(op1 == 0xC7) {
                        if(op2 == 0x45) {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 6);
                            at += 6;
                        }
                        else if(op2 == 0x44) {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 7);
                            at += 7;
                        }
                        else if(op2 == 0x05) {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 9);
                            at += 9;
                        }
                        break;
                    }
                    // sub dword ptr [reg+op4], reg
                    else if(op1 == 0x29) {
                        if(op2 == 0x8B) {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 7);
                            at += 7;
                        }
                        break;
                    }
                    // mov [addr], ax
                    else if(op1 == 0xA3) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                        break;
                    }
                    // cmp reg, [reg+op3] or mov reg, [reg+op3]
                    else if(op1 == 0x3B || op1 == 0x3D || op1 == 0x8B) {
                        if(op2 == 0xCE) {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 3);
                            at += 3;
                        }
                        else {
                            offsets.push_back(at - at_start);
                            bytes.insert(bytes.end(), at, at + 4);
                            at += 4;
                        }
                        break;
                    }
                    return false;
                }

                // push 0x00000000-0xFFFFFFFF
                case 0x68: {
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 5);
                    at += 5;
                    break;
                }

                // push 0x00000000-0xFFFFFFFF
                case 0x69: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(op1 == 0xFF) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                        break;
                    }
                    return false;
                }

                // push 0x00-0xFF
                case 0x6A: {
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 2);
                    at += 2;
                    break;
                }

                // short conditional jump
                case 0x70: case 0x71: case 0x72: case 0x73:
                case 0x74: case 0x75: case 0x76: case 0x77:
                case 0x78: case 0x79: case 0x7A: case 0x7B:
                case 0x7C: case 0x7D: case 0x7E: case 0x7F:
                // short unconditional jump
                case 0xEB: {
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 2);
                    at += 2;
                    break;
                }

                // add/or/adc/sbb/and/sub/xor/cmp <something> 0x00000000-0x7FFFFFFF
                case 0x81: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    // add/or/adc/sbb/and/sub/xor/cmp dword ptr <something> 0x00000000-0x7FFFFFFF
                    if(op1 == 0x3D) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 10);
                        at += 10;
                        break;
                    }
                    // add/or/adc/sbb/and/sub/xor/cmp <register> 0x00000000-0x7FFFFFFF
                    if(op1 >= 0xC0 || op1 == 0x0D) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                        break;
                    }
                    return false;
                }

                // add/or/adc/sbb/and/sub/xor/cmp <something> 0x00-0x7F
                case 0x83: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    // add/or/adc/sbb/and/sub/xor/cmp <register> 0x00 - 0x7F
                    if(op1 >= 0xC0) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 3);
                        at += 3;
                    }
                    else {
                        return false;
                    }
                    break;
                }

                // test <something>, <something>
                case 0x84:
                case 0x85: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    // add/or/adc/sbb/and/sub/xor/cmp <register> 0x00 - 0x7F
                    if(op1 >= 0xC0) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 2);
                        at += 2;
                        break;
                    }
                    else {
                        return false;
                    }
                }

                // idk
                case 0x89: {
                    auto a = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(a == 0x06) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 2);
                        at += 2;
                        break;
                    }

                    if(a == 0x7D) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 3);
                        at += 3;
                        break;
                    }

                    if(a == 0x15 || a == 0x3D) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                        break;
                    }

                    if(a == 0x6C || a == 0x4C || a == 0x44 || a == 0x54) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 4);
                        at += 4;
                        break;
                    }

                    auto b = *reinterpret_cast<const std::uint8_t *>(at + 2);
                    if(a == 0xC && b == 0x85) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 7);
                        at += 7;
                        break;
                    }

                    if((a == 0x94 || a == 0x84) && b == 0x24) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 7);
                        at += 7;
                        break;
                    }

                    return false;
                }

                // mov bl, [eax+esi]
                case 0x8A: {
                    auto a = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(a == 0x1C || a == 0x46 || a == 0x48 || a == 0x14) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 3);
                        at += 3;
                        break;
                    }

                    if(a == 0x54) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 4);
                        at += 4;
                        break;
                    }

                    return false;
                }

                // moving stuff
                case 0x8B: {
                    auto a = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    auto b = *reinterpret_cast<const std::uint8_t *>(at + 2);
                    if((a == 0x6C || a == 0x4C || a == 0x44 || a == 0x54) && b == 0x24) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 4);
                        at += 4;
                        break;
                    }
                    else if(a == 0xE5 || a == 0xF8 || a == 0xC3 || a == 0xC2 || a == 0xEC || a == 0x12 || a == 0xF0) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 2);
                        at += 2;
                        break;
                    }
                    else if(a == 0x50 || a == 0x40 || a == 0x79) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 3);
                        at += 3;
                        break;
                    }
                    else if(a == 0x93 || a == 0x0D || a == 0x2D || a == 0x1D || a == 0x83 || a == 0x89 || a == 0x92 || a == 0x15) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                        break;
                    }
                    return false;
                }

                // lea
                case 0x8D: {
                    auto a = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    auto b = *reinterpret_cast<const std::uint8_t *>(at + 2);
                    if(a == 0x44 && (b == 0x0C || b == 0x24)) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 4);
                        at += 4;
                        break;
                    }
                    else if(a == 0x7E) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 3);
                        at += 3;
                        break;
                    }

                    return false;
                }

                // shl
                case 0xC1: {
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 3);
                    at += 3;
                    break;
                }

                case 0xD3: {
                    auto a = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(a == 0xE3) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 2);
                        at += 2;
                        break;
                    }

                    return false;
                }

                // nop
                case 0x90: {
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 1);
                    at ++;
                    break;
                }

                // mov al, byte [something]
                case 0xA0:
                // mov eax, dword [something]
                case 0xA1:
                // move byte [something], al
                case 0xA2:
                // move dword [something], eax
                case 0xA3: {
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 5);
                    at += 5;
                    break;
                }

                // mov something
                case 0xB8:
                case 0xBA:
                case 0xBB:
                case 0xBE:
                case 0xBF: {
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 5);
                    at += 5;
                    break;
                }

                // mov something
                case 0xC7: {
                    auto a = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(a == 0x05) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                        break;
                    }
                    else if(a == 0x44) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 8);
                        at += 8;
                        break;
                    }
                    else if(a == 0x45) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 7);
                        at += 7;
                        break;
                    }
                    return false;
                }

                // fmul
                case 0xD8: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(op1 == 0x4F) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 3);
                        at += 3;
                        break;
                    }
                    else if(op1 == 0x4C) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 4);
                        at += 4;
                        break;
                    }
                    else if(op1 == 0x0D) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                        break;
                    }
                    return false;
                }

                // fld / fst
                case 0xD9: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(op1 == 0x47 || op1 == 0x55 || op1 == 0x42 || op1 == 0x45 || op1 == 0x46) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 3);
                        at += 3;
                        break;
                    }
                    else if(op1 == 0xC0) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 2);
                        at += 2;
                        break;
                    }
                    else if(op1 == 0x1D || op1 == 0x05) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                        break;
                    }
                    else if(op1 == 0x1C || op1 == 0x9C) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 7);
                        at += 7;
                        break;
                    }
                    return false;
                }


                case 0xE8:
                case 0xE9:
                    offsets.push_back(at - at_start);
                    bytes.insert(bytes.end(), at, at + 5);
                    at += 5;
                    break;

                // test something
                case 0xF7: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    //test dword ptr[x], 0x00000000-0x7FFFFFFF
                    if(op1 == 0x05) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 10);
                        at += 10;
                        break;
                    }
                    return false;
                }

                // call dword ptr[x]
                case 0xFF: {
                    auto op1 = *reinterpret_cast<const std::uint8_t *>(at + 1);
                    if(op1 == 0x51 || op1 == 0x52 || op1 == 0x56 || op1 == 0x57) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 3);
                        at += 3;
                    }
                    else if(op1 == 0xD3) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 2);
                        at += 2;
                    }
                    else if(op1 == 0x54) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 4);
                        at += 4;
                    }
                    else if(op1 == 0x15 || op1 == 0x92 || op1 == 0x91) {
                        offsets.push_back(at - at_start);
                        bytes.insert(bytes.end(), at, at + 6);
                        at += 6;
                    }
                    else {
                        return false;
                    }
                    break;
                }

                // Terminate. We don't know what to do.
                default:
                    std::cout << "Cannot figure out what's at " << std::to_string(reinterpret_cast<std::uintptr_t>(at)) << std::endl;
                    return false;
            }
            if(at == instruction_start) {
                return false;
            }
        }
        return true;
    }

    void write_jmp_call(void *jmp_at, Hook &hook, const void *call_before, const void *call_after, bool pushad_pushfd) {
        if(!is_executable_memory_range(jmp_at, 16) ||
           (call_before && !is_executable_memory_range(call_before, 1)) ||
           (call_after && !is_executable_memory_range(call_after, 1))) {
            return;
        }

        // Rollback the hook if not already done so
        hook.rollback();

        // Get the instructions. Unsupported instructions now fail this hook
        // cleanly instead of terminating the entire Halo process.
        std::vector<std::uintptr_t> offsets;
        std::vector<std::byte> bytes;
        std::byte *jmp_at_byte = reinterpret_cast<std::byte *>(jmp_at);
        try {
            if(!get_instructions(jmp_at_byte, bytes, offsets, 5)) {
                return;
            }
        }
        catch(...) {
            return;
        }
        hook.address = jmp_at_byte;

        // Calculate how much data we'll need. (size of bytes plus 9 bytes per call [5 for the call and 4 for pushad/popad and pushfd/popfd])
        std::size_t added_pushad_bytes = pushad_pushfd ? 4 : 0;
        const std::size_t relocated_original_size = relocated_size(bytes, offsets);
        std::size_t size = relocated_original_size + (call_before ? 5 + added_pushad_bytes : 0) + (call_after ? 5 + added_pushad_bytes : 0) + 5;

        // Back up the original bytes
        try {
            hook.original_bytes.insert(hook.original_bytes.end(), jmp_at_byte, jmp_at_byte + bytes.size());
        }
        catch(...) {
            hook.address = nullptr;
            return;
        }

        // Now make the hook. Avoid throwing std::bad_alloc through callers that
        // install hooks from noexcept initialization paths.
        hook.hook.reset(new(std::nothrow) std::byte[size]);
        if(!hook.hook) {
            hook.original_bytes.clear();
            hook.address = nullptr;
            return;
        }
        auto *hook_data = hook.hook.get();
        DWORD old_protection = 0;

        // Give it PAGE_EXECUTE_READWRITE so the generated trampoline can execute safely.
        if(!VirtualProtect(hook_data, size, PAGE_EXECUTE_READWRITE, &old_protection)) {
            hook.hook.reset();
            hook.original_bytes.clear();
            hook.address = nullptr;
            return;
        }

        std::vector<std::byte> relocated;
        try {
            relocated = relocate_instructions(jmp_at_byte, bytes, offsets,
                                              hook_data + (call_before ? 5 + added_pushad_bytes : 0));
        }
        catch(...) {
            hook.hook.reset();
            hook.original_bytes.clear();
            hook.address = nullptr;
            return;
        }

        // Overwrite the original bytes with NOPs and a jmp instruction
        DWORD new_protection = PAGE_EXECUTE_READWRITE;
        if(!VirtualProtect(jmp_at_byte, bytes.size(), new_protection, &old_protection)) {
            hook.hook.reset();
            hook.original_bytes.clear();
            hook.address = nullptr;
            return;
        }
        *reinterpret_cast<std::uint8_t *>(jmp_at_byte) = 0xE9;
        *reinterpret_cast<std::uintptr_t *>(jmp_at_byte + 1) = hook_data - (jmp_at_byte + 5);
        std::memset(jmp_at_byte + 5, 0x90, bytes.size() - 5);
        if(old_protection != new_protection) {
            VirtualProtect(jmp_at, bytes.size(), old_protection, &new_protection);
        }
        FlushInstructionCache(GetCurrentProcess(), jmp_at_byte, bytes.size());

        // Let's do dis
        auto add_call = [&pushad_pushfd](const void *where, std::byte *data) {
            std::size_t call_offset = pushad_pushfd ? 2 : 0;

            if(pushad_pushfd) {
                // pushfd
                *reinterpret_cast<std::uint8_t *>(data + 0) = 0x9C;
                // pushad
                *reinterpret_cast<std::uint8_t *>(data + 1) = 0x60;

                // popad
                *reinterpret_cast<std::uint8_t *>(data + 7) = 0x61;
                // popfd
                *reinterpret_cast<std::uint8_t *>(data + 8) = 0x9D;
            }

            // call
            *reinterpret_cast<std::uint8_t *>(data + call_offset) = 0xE8;
            *reinterpret_cast<std::uintptr_t *>(data + call_offset + 1) = reinterpret_cast<const std::byte *>(where) - (data + call_offset + 5);
        };

        // Add the first call
        if(call_before) {
            add_call(reinterpret_cast<const std::uint8_t *>(call_before), hook_data);
            hook_data += 5 + added_pushad_bytes;
        }

        // Copy the already-relocated original instructions.
        std::copy(relocated.begin(), relocated.end(), hook_data);
        hook_data += relocated.size();

        // Add the other call
        if(call_after) {
            add_call(reinterpret_cast<const std::uint8_t *>(call_after), hook_data);
            hook_data += 5 + added_pushad_bytes;
        }

        // Add the jmp instruction to exit this hook
        *reinterpret_cast<std::uint8_t *>(hook_data) = 0xE9;
        *reinterpret_cast<std::uintptr_t *>(hook_data + 1) = (jmp_at_byte + bytes.size()) - (hook_data + 5);
        FlushInstructionCache(GetCurrentProcess(), hook.hook.get(), size);
    }

    void write_function_override(void *jmp_at, Hook &hook, const void *new_function, const void **original_function) {
        if(!original_function || !is_executable_memory_range(jmp_at, 16) ||
           !is_executable_memory_range(new_function, 1)) {
            return;
        }

        // Rollback the hook if not already done so
        hook.rollback();

        // Get the instructions. Unsupported instructions fail safely.
        std::vector<std::uintptr_t> offsets;
        std::vector<std::byte> bytes;
        std::byte *jmp_at_byte = reinterpret_cast<std::byte *>(jmp_at);
        try {
            if(!get_instructions(jmp_at_byte, bytes, offsets, 5)) {
                return;
            }
        }
        catch(...) {
            return;
        }
        hook.address = jmp_at_byte;

        // Calculate how much data we'll need. Short relative branches may expand in the trampoline.
        std::size_t size = 5 + relocated_size(bytes, offsets) + 5;

        // Back up the original bytes
        try {
            hook.original_bytes.insert(hook.original_bytes.end(), jmp_at_byte, jmp_at_byte + bytes.size());
        }
        catch(...) {
            hook.address = nullptr;
            return;
        }

        // Now make the hook.
        hook.hook.reset(new(std::nothrow) std::byte[size]);
        if(!hook.hook) {
            hook.original_bytes.clear();
            hook.address = nullptr;
            return;
        }
        auto *hook_data = hook.hook.get();
        DWORD old_protection = 0;

        // Give it PAGE_EXECUTE_READWRITE so the generated trampoline can execute safely.
        if(!VirtualProtect(hook_data, size, PAGE_EXECUTE_READWRITE, &old_protection)) {
            hook.hook.reset();
            hook.original_bytes.clear();
            hook.address = nullptr;
            return;
        }

        std::vector<std::byte> relocated;
        try {
            relocated = relocate_instructions(jmp_at_byte, bytes, offsets, hook_data + 5);
        }
        catch(...) {
            hook.hook.reset();
            hook.original_bytes.clear();
            hook.address = nullptr;
            return;
        }

        // Overwrite the original bytes with NOPs and a jmp instruction
        DWORD new_protection = PAGE_EXECUTE_READWRITE;
        if(!VirtualProtect(jmp_at_byte, bytes.size(), new_protection, &old_protection)) {
            hook.hook.reset();
            hook.original_bytes.clear();
            hook.address = nullptr;
            return;
        }
        *reinterpret_cast<std::uint8_t *>(jmp_at_byte) = 0xE9;
        *reinterpret_cast<std::uintptr_t *>(jmp_at_byte + 1) = hook_data - (jmp_at_byte + 5);
        std::memset(jmp_at_byte + 5, 0x90, bytes.size() - 5);
        if(old_protection != new_protection) {
            VirtualProtect(jmp_at, bytes.size(), old_protection, &new_protection);
        }
        FlushInstructionCache(GetCurrentProcess(), jmp_at_byte, bytes.size());

        // Write a jmp to the new function
        *reinterpret_cast<std::uint8_t *>(hook_data) = 0xE9;
        *reinterpret_cast<std::uintptr_t *>(hook_data + 1) = reinterpret_cast<const std::byte *>(new_function) - (hook_data + 5);
        hook_data += 5;

        // Copy the already-relocated original instructions.
        std::copy(relocated.begin(), relocated.end(), hook_data);
        *original_function = hook_data;

        // Write a jmp to the original function after all is said and done
        hook_data += relocated.size();
        *reinterpret_cast<std::uint8_t *>(hook_data) = 0xE9;
        *reinterpret_cast<std::uintptr_t *>(hook_data + 1) = reinterpret_cast<const std::byte *>(jmp_at) + bytes.size() - (hook_data + 5);
        FlushInstructionCache(GetCurrentProcess(), hook.hook.get(), size);
    }

    void write_code(void *pointer, const SigByte *data, std::size_t length) noexcept {
        // Instantiate our new_protection and old_protection variables.
        DWORD new_protection = PAGE_EXECUTE_READWRITE, old_protection;

        // Apply read/write/execute protection
        if(!VirtualProtect(pointer, length, new_protection, &old_protection)) {
            return;
        }

        // Copy
        for(std::size_t i = 0; i < length; i++) {
            if(data[i] != -1) {
                *(reinterpret_cast<std::uint8_t *>(pointer) + i) = static_cast<std::uint8_t>(data[i]);
            }
        }
        FlushInstructionCache(GetCurrentProcess(), pointer, length);

        // Restore the older protection unless it's the same
        if(new_protection != old_protection) {
            VirtualProtect(pointer, length, old_protection, &new_protection);
        }
    }
}
