#include "signature_v2.hpp"

#include <limits>

namespace Chimera {
    SignatureV2::SignatureV2(const char *name, const char *feature, HMODULE module,
                             const SigByteV2 *signature, std::size_t length) noexcept
        : module_(module) {
        if(!module_ || !signature || length == 0) {
            return;
        }

        try {
            if(name) {
                name_ = name;
            }
            if(feature) {
                feature_ = feature;
            }
            pattern_.assign(signature, signature + length);
        }
        catch(...) {
            pattern_.clear();
            return;
        }

        refresh();
    }

    bool SignatureV2::refresh() noexcept {
        matches_.clear();
        valid_ = false;

        if(!module_ || pattern_.empty()) {
            return false;
        }

        CodeFinderV2 finder(module_, pattern_.data(), pattern_.size());
        if(!finder.valid()) {
            return false;
        }

        matches_ = finder.find_all();
        valid_ = true;
        return true;
    }

    bool SignatureV2::valid() const noexcept {
        return valid_;
    }

    const char *SignatureV2::name() const noexcept {
        return name_.c_str();
    }

    const char *SignatureV2::feature() const noexcept {
        return feature_.c_str();
    }

    std::size_t SignatureV2::count() const noexcept {
        return matches_.size();
    }

    const std::vector<std::uintptr_t> &SignatureV2::matches() const noexcept {
        return matches_;
    }

    std::byte *SignatureV2::match(std::size_t index) const noexcept {
        return index < matches_.size() ? reinterpret_cast<std::byte *>(matches_[index]) : nullptr;
    }

    std::byte *SignatureV2::unique() const noexcept {
        return matches_.size() == 1 ? reinterpret_cast<std::byte *>(matches_.front()) : nullptr;
    }

    std::byte *SignatureV2::unique_address(std::ptrdiff_t offset) const noexcept {
        return matches_.size() == 1 ? address(0, offset) : nullptr;
    }

    std::byte *SignatureV2::unique_call_site(std::ptrdiff_t offset) const noexcept {
        return matches_.size() == 1 ? call_site(0, offset) : nullptr;
    }

    std::byte *SignatureV2::unique_call_target(std::ptrdiff_t offset) const noexcept {
        return matches_.size() == 1 ? call_target(0, offset) : nullptr;
    }

    std::byte *SignatureV2::unique_jump_site(std::ptrdiff_t offset) const noexcept {
        return matches_.size() == 1 ? jump_site(0, offset) : nullptr;
    }

    std::byte *SignatureV2::unique_jump_target(std::ptrdiff_t offset) const noexcept {
        return matches_.size() == 1 ? jump_target(0, offset) : nullptr;
    }

    std::byte *SignatureV2::address(std::size_t index, std::ptrdiff_t offset) const noexcept {
        const auto *base_pointer = match(index);
        if(!base_pointer) {
            return nullptr;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(base_pointer);
        const auto result_signed = static_cast<std::int64_t>(base) + static_cast<std::int64_t>(offset);
        if(result_signed <= 0 ||
           static_cast<std::uint64_t>(result_signed) > std::numeric_limits<std::uintptr_t>::max()) {
            return nullptr;
        }

        auto *result = reinterpret_cast<std::byte *>(static_cast<std::uintptr_t>(result_signed));
        return CodeFinderV2::readable_range(result, 1) ? result : nullptr;
    }

    std::byte *SignatureV2::call_site(std::size_t index, std::ptrdiff_t offset) const noexcept {
        auto *site = address(index, offset);
        if(!CodeFinderV2::executable_range(site, 5) ||
           *reinterpret_cast<const std::uint8_t *>(site) != 0xE8) {
            return nullptr;
        }
        return site;
    }

    std::byte *SignatureV2::call_target(std::size_t index, std::ptrdiff_t offset) const noexcept {
        return CodeFinderV2::relative_call_target(call_site(index, offset));
    }

    std::byte *SignatureV2::jump_site(std::size_t index, std::ptrdiff_t offset) const noexcept {
        auto *site = address(index, offset);
        if(!CodeFinderV2::executable_range(site, 5) ||
           *reinterpret_cast<const std::uint8_t *>(site) != 0xE9) {
            return nullptr;
        }
        return site;
    }

    std::byte *SignatureV2::jump_target(std::size_t index, std::ptrdiff_t offset) const noexcept {
        return CodeFinderV2::relative_jump_target(jump_site(index, offset));
    }

    bool SignatureV2::matches_at(std::size_t index, std::ptrdiff_t offset,
                                 std::span<const SigByteV2> pattern) const noexcept {
        auto *site = address(index, offset);
        if(!site || pattern.empty() || !CodeFinderV2::readable_range(site, pattern.size())) {
            return false;
        }

        for(std::size_t i = 0; i < pattern.size(); i++) {
            const auto expected = pattern[i];
            if(expected == -1) {
                continue;
            }
            if(expected < 0 || expected > 0xFF ||
               *reinterpret_cast<const std::uint8_t *>(site + i) != static_cast<std::uint8_t>(expected)) {
                return false;
            }
        }

        return true;
    }
}
