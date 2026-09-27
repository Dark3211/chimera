#pragma once

#include "hook.hpp"
#include "hac/codefinder_v2.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace Chimera {
    class HookV2 {
    public:
        using PatternByteV2 = CodeFinderV2::PatternByteV2;

        explicit HookV2(const char *owner = nullptr) noexcept;
        ~HookV2() noexcept;

        HookV2(const HookV2 &) = delete;
        HookV2 &operator=(const HookV2 &) = delete;
        HookV2(HookV2 &&) = delete;
        HookV2 &operator=(HookV2 &&) = delete;

        bool install_inline(void *address, std::span<const PatternByteV2> expected,
                            const void *call_before = nullptr, const void *call_after = nullptr,
                            bool pushad_pushfd = true) noexcept;
        bool install_call(void *address, std::span<const PatternByteV2> expected,
                          const void *expected_target,
                          const void *call_before = nullptr, const void *call_after = nullptr,
                          bool pushad_pushfd = true) noexcept;
        bool install_function(void *address, std::span<const PatternByteV2> expected,
                              const void *new_function, const void **original_function) noexcept;
        bool rollback() noexcept;
        bool active() const noexcept;
        bool owns_current_bytes() const noexcept;
        std::byte *address() const noexcept;
        std::size_t size() const noexcept;
        const char *owner() const noexcept;

    private:
        bool expected_matches(void *address, std::span<const PatternByteV2> expected) const noexcept;
        bool finalize_install() noexcept;
        void reset_state() noexcept;
        void abandon() noexcept;

        Hook hook_;
        std::vector<std::byte> installed_bytes_;
        std::string owner_;
        bool registered_ = false;
    };
}
