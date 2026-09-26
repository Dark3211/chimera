#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Chimera {
    class CodeFinderV2 {
    public:
        using PatternByteV2 = short;

        CodeFinderV2(HMODULE module, const PatternByteV2 *signature, std::size_t length) noexcept;
        CodeFinderV2(HMODULE module, std::span<const PatternByteV2> signature) noexcept;

        bool valid() const noexcept;
        std::vector<std::uintptr_t> find_all() const noexcept;
        std::vector<std::uintptr_t> find_range(const void *begin, std::size_t length) const noexcept;
        std::uintptr_t find_unique() const noexcept;

        static bool readable_range(const void *address, std::size_t length = 1) noexcept;
        static bool executable_range(const void *address, std::size_t length = 1) noexcept;
        static std::byte *relative_call_target(const void *site) noexcept;
        static std::byte *relative_jump_target(const void *site) noexcept;

    private:
        bool matches(const std::byte *address) const noexcept;
        bool module_bounds(std::uintptr_t &begin, std::uintptr_t &end) const noexcept;

        HMODULE module_ = nullptr;
        std::vector<PatternByteV2> signature_;
        bool valid_ = false;
    };
}
