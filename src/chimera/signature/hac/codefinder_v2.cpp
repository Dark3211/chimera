#include "codefinder_v2.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace Chimera {
    namespace {
        bool memory_range_v2(const void *address, std::size_t length, bool executable) noexcept {
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

                if(executable) {
                    const DWORD protection = information.Protect & 0xFFU;
                    if(protection != PAGE_EXECUTE &&
                       protection != PAGE_EXECUTE_READ &&
                       protection != PAGE_EXECUTE_READWRITE &&
                       protection != PAGE_EXECUTE_WRITECOPY) {
                        return false;
                    }
                }

                const auto region_begin = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
                const auto region_size = static_cast<std::uintptr_t>(information.RegionSize);
                if(region_size == 0 ||
                   region_begin > std::numeric_limits<std::uintptr_t>::max() - region_size) {
                    return false;
                }

                const auto region_end = region_begin + region_size;
                if(region_end <= cursor) {
                    return false;
                }

                cursor = region_end < end ? region_end : end;
            }

            return true;
        }

        std::byte *relative_target_v2(const void *site, std::uint8_t opcode) noexcept {
            if(!CodeFinderV2::executable_range(site, 5)) {
                return nullptr;
            }

            const auto *bytes = reinterpret_cast<const std::byte *>(site);
            if(*reinterpret_cast<const std::uint8_t *>(bytes) != opcode) {
                return nullptr;
            }

            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes + 1, sizeof(displacement));

            const auto next = reinterpret_cast<std::uintptr_t>(bytes) + 5;
            const auto target_signed = static_cast<std::int64_t>(next) + static_cast<std::int64_t>(displacement);
            if(target_signed <= 0 ||
               static_cast<std::uint64_t>(target_signed) > std::numeric_limits<std::uintptr_t>::max()) {
                return nullptr;
            }

            auto *target = reinterpret_cast<std::byte *>(static_cast<std::uintptr_t>(target_signed));
            return CodeFinderV2::executable_range(target, 1) ? target : nullptr;
        }
    }

    CodeFinderV2::CodeFinderV2(HMODULE module, const PatternByteV2 *signature, std::size_t length) noexcept
        : module_(module) {
        if(!module_ || !signature || length == 0) {
            return;
        }

        try {
            signature_.assign(signature, signature + length);
        }
        catch(...) {
            signature_.clear();
            return;
        }

        bool has_fixed_byte = false;
        for(const auto value : signature_) {
            if(value == -1) {
                continue;
            }
            if(value < 0 || value > 0xFF) {
                signature_.clear();
                return;
            }
            has_fixed_byte = true;
        }

        valid_ = has_fixed_byte;
    }

    CodeFinderV2::CodeFinderV2(HMODULE module, std::span<const PatternByteV2> signature) noexcept
        : CodeFinderV2(module, signature.data(), signature.size()) {}

    bool CodeFinderV2::valid() const noexcept {
        return valid_;
    }

    bool CodeFinderV2::readable_range(const void *address, std::size_t length) noexcept {
        return memory_range_v2(address, length, false);
    }

    bool CodeFinderV2::executable_range(const void *address, std::size_t length) noexcept {
        return memory_range_v2(address, length, true);
    }

    bool CodeFinderV2::module_bounds(std::uintptr_t &begin, std::uintptr_t &end) const noexcept {
        begin = 0;
        end = 0;

        if(!module_ || !readable_range(module_, sizeof(IMAGE_DOS_HEADER))) {
            return false;
        }

        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module_);
        if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
            return false;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(module_);
        const auto nt_offset = static_cast<std::uintptr_t>(dos->e_lfanew);
        if(base > std::numeric_limits<std::uintptr_t>::max() - nt_offset) {
            return false;
        }

        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + nt_offset);
        if(!readable_range(nt, sizeof(IMAGE_NT_HEADERS)) ||
           nt->Signature != IMAGE_NT_SIGNATURE ||
           nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
           nt->OptionalHeader.SizeOfImage == 0) {
            return false;
        }

        const auto image_size = static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
        if(base > std::numeric_limits<std::uintptr_t>::max() - image_size) {
            return false;
        }

        begin = base;
        end = base + image_size;
        return true;
    }

    bool CodeFinderV2::matches(const std::byte *address) const noexcept {
        for(std::size_t i = 0; i < signature_.size(); i++) {
            const auto expected = signature_[i];
            if(expected >= 0 &&
               *reinterpret_cast<const std::uint8_t *>(address + i) != static_cast<std::uint8_t>(expected)) {
                return false;
            }
        }
        return true;
    }

    std::vector<std::uintptr_t> CodeFinderV2::find_range(const void *begin, std::size_t length) const noexcept {
        std::vector<std::uintptr_t> matches_found;
        if(!valid_ || !begin || length < signature_.size() || !readable_range(begin, length)) {
            return matches_found;
        }

        try {
            const auto *bytes = reinterpret_cast<const std::byte *>(begin);
            const auto last = length - signature_.size();
            for(std::size_t offset = 0; offset <= last; offset++) {
                if(matches(bytes + offset)) {
                    matches_found.emplace_back(reinterpret_cast<std::uintptr_t>(bytes + offset));
                }
            }
        }
        catch(...) {
            matches_found.clear();
        }

        return matches_found;
    }

    std::vector<std::uintptr_t> CodeFinderV2::find_all() const noexcept {
        std::vector<std::uintptr_t> matches_found;
        if(!valid_) {
            return matches_found;
        }

        std::uintptr_t image_begin = 0;
        std::uintptr_t image_end = 0;
        if(!module_bounds(image_begin, image_end)) {
            return matches_found;
        }

        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image_begin);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(image_begin + static_cast<std::uintptr_t>(dos->e_lfanew));
        const auto *sections = reinterpret_cast<const IMAGE_SECTION_HEADER *>(
            reinterpret_cast<const std::byte *>(&nt->OptionalHeader) +
            static_cast<std::size_t>(nt->FileHeader.SizeOfOptionalHeader)
        );
        const auto section_count = static_cast<std::size_t>(nt->FileHeader.NumberOfSections);

        if(section_count == 0 ||
           !readable_range(sections, section_count * sizeof(IMAGE_SECTION_HEADER))) {
            return matches_found;
        }

        try {
            for(std::size_t i = 0; i < section_count; i++) {
                const auto &section = sections[i];
                if((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
                    continue;
                }

                const auto rva = static_cast<std::uintptr_t>(section.VirtualAddress);
                if(rva >= image_end - image_begin) {
                    continue;
                }

                auto size = static_cast<std::uintptr_t>(section.Misc.VirtualSize);
                if(size == 0) {
                    size = static_cast<std::uintptr_t>(section.SizeOfRawData);
                }

                const auto available = image_end - image_begin - rva;
                size = std::min(size, available);
                if(size < signature_.size()) {
                    continue;
                }

                const auto *section_begin = reinterpret_cast<const void *>(image_begin + rva);
                const auto section_matches = find_range(section_begin, static_cast<std::size_t>(size));
                matches_found.insert(matches_found.end(), section_matches.begin(), section_matches.end());
            }
        }
        catch(...) {
            matches_found.clear();
        }

        return matches_found;
    }

    std::uintptr_t CodeFinderV2::find_unique() const noexcept {
        const auto matches_found = find_all();
        return matches_found.size() == 1 ? matches_found.front() : 0;
    }

    std::byte *CodeFinderV2::relative_call_target(const void *site) noexcept {
        return relative_target_v2(site, 0xE8);
    }

    std::byte *CodeFinderV2::relative_jump_target(const void *site) noexcept {
        return relative_target_v2(site, 0xE9);
    }
}
