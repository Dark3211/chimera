#pragma once

#include "hac/codefinder_v2.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Chimera {
    class SignatureV2 {
    public:
        using SigByteV2 = CodeFinderV2::PatternByteV2;

        SignatureV2(const char *name, const char *feature, HMODULE module,
                    const SigByteV2 *signature, std::size_t length) noexcept;

        bool refresh() noexcept;
        bool valid() const noexcept;
        const char *name() const noexcept;
        const char *feature() const noexcept;
        std::size_t count() const noexcept;
        const std::vector<std::uintptr_t> &matches() const noexcept;
        std::byte *match(std::size_t index) const noexcept;
        std::byte *unique() const noexcept;
        std::byte *unique_address(std::ptrdiff_t offset = 0) const noexcept;
        std::byte *unique_call_site(std::ptrdiff_t offset = 0) const noexcept;
        std::byte *unique_call_target(std::ptrdiff_t offset = 0) const noexcept;
        std::byte *unique_jump_site(std::ptrdiff_t offset = 0) const noexcept;
        std::byte *unique_jump_target(std::ptrdiff_t offset = 0) const noexcept;
        std::byte *address(std::size_t index, std::ptrdiff_t offset = 0) const noexcept;
        std::byte *call_site(std::size_t index, std::ptrdiff_t offset = 0) const noexcept;
        std::byte *call_target(std::size_t index, std::ptrdiff_t offset = 0) const noexcept;
        std::byte *jump_site(std::size_t index, std::ptrdiff_t offset = 0) const noexcept;
        std::byte *jump_target(std::size_t index, std::ptrdiff_t offset = 0) const noexcept;
        bool matches_at(std::size_t index, std::ptrdiff_t offset,
                        std::span<const SigByteV2> pattern) const noexcept;

    private:
        HMODULE module_ = nullptr;
        std::string name_;
        std::string feature_;
        std::vector<SigByteV2> pattern_;
        std::vector<std::uintptr_t> matches_;
        bool valid_ = false;
    };
}
