#include "hook_v2.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace Chimera {
    namespace {
        struct HookRegistryEntryV2 {
            const HookV2 *owner = nullptr;
            std::uintptr_t begin = 0;
            std::uintptr_t end = 0;
        };

        std::mutex hook_registry_mutex_v2;
        std::vector<HookRegistryEntryV2> hook_registry_v2;

        bool range_values_v2(const void *address, std::size_t size,
                             std::uintptr_t &begin, std::uintptr_t &end) noexcept {
            if(!address || size == 0) {
                return false;
            }

            begin = reinterpret_cast<std::uintptr_t>(address);
            if(begin > std::numeric_limits<std::uintptr_t>::max() - size) {
                return false;
            }

            end = begin + size;
            return true;
        }

        bool range_available_v2(const HookV2 *owner, const void *address, std::size_t size) noexcept {
            std::uintptr_t begin = 0;
            std::uintptr_t end = 0;
            if(!range_values_v2(address, size, begin, end)) {
                return false;
            }

            try {
                std::lock_guard<std::mutex> lock(hook_registry_mutex_v2);
                for(const auto &entry : hook_registry_v2) {
                    if(entry.owner != owner && begin < entry.end && entry.begin < end) {
                        return false;
                    }
                }
            }
            catch(...) {
                return false;
            }

            return true;
        }

        bool register_range_v2(const HookV2 *owner, const void *address, std::size_t size) noexcept {
            std::uintptr_t begin = 0;
            std::uintptr_t end = 0;
            if(!range_values_v2(address, size, begin, end)) {
                return false;
            }

            try {
                std::lock_guard<std::mutex> lock(hook_registry_mutex_v2);
                for(const auto &entry : hook_registry_v2) {
                    if(entry.owner != owner && begin < entry.end && entry.begin < end) {
                        return false;
                    }
                }
                hook_registry_v2.push_back({owner, begin, end});
            }
            catch(...) {
                return false;
            }

            return true;
        }

        void unregister_range_v2(const HookV2 *owner) noexcept {
            try {
                std::lock_guard<std::mutex> lock(hook_registry_mutex_v2);
                hook_registry_v2.erase(
                    std::remove_if(
                        hook_registry_v2.begin(),
                        hook_registry_v2.end(),
                        [owner](const HookRegistryEntryV2 &entry) {
                            return entry.owner == owner;
                        }
                    ),
                    hook_registry_v2.end()
                );
            }
            catch(...) {
            }
        }

        bool bytes_equal_v2(const void *address, const std::vector<std::byte> &bytes) noexcept {
            if(bytes.empty() || !CodeFinderV2::readable_range(address, bytes.size())) {
                return false;
            }
            return std::memcmp(address, bytes.data(), bytes.size()) == 0;
        }
    }

    HookV2::HookV2(const char *owner) noexcept {
        if(owner) {
            try {
                owner_ = owner;
            }
            catch(...) {
                owner_.clear();
            }
        }
    }

    HookV2::~HookV2() noexcept {
        if(active() && !rollback()) {
            abandon();
        }
    }

    bool HookV2::expected_matches(void *address, std::span<const PatternByteV2> expected) const noexcept {
        if(!address || expected.empty() || !CodeFinderV2::readable_range(address, expected.size())) {
            return false;
        }

        auto *bytes = reinterpret_cast<const std::byte *>(address);
        for(std::size_t i = 0; i < expected.size(); i++) {
            const auto value = expected[i];
            if(value == -1) {
                continue;
            }
            if(value < 0 || value > 0xFF ||
               *reinterpret_cast<const std::uint8_t *>(bytes + i) != static_cast<std::uint8_t>(value)) {
                return false;
            }
        }

        return true;
    }

    bool HookV2::finalize_install() noexcept {
        if(!hook_.address || !hook_.hook || hook_.original_bytes.empty()) {
            reset_state();
            return false;
        }

        try {
            installed_bytes_.assign(
                hook_.address,
                hook_.address + hook_.original_bytes.size()
            );
        }
        catch(...) {
            installed_bytes_.clear();
            if(bytes_equal_v2(hook_.address, hook_.original_bytes)) {
                reset_state();
            }
            else {
                abandon();
            }
            return false;
        }

        if(!register_range_v2(this, hook_.address, installed_bytes_.size())) {
            if(owns_current_bytes()) {
                overwrite(hook_.address, hook_.original_bytes.data(), hook_.original_bytes.size());
            }

            if(bytes_equal_v2(hook_.address, hook_.original_bytes)) {
                reset_state();
            }
            else {
                abandon();
            }
            return false;
        }

        registered_ = true;
        return true;
    }

    bool HookV2::install_inline(void *address, std::span<const PatternByteV2> expected,
                                const void *call_before, const void *call_after,
                                bool pushad_pushfd) noexcept {
        if(active() || !expected_matches(address, expected) ||
           !CodeFinderV2::executable_range(address, 16) ||
           !range_available_v2(this, address, 5)) {
            return false;
        }

        write_jmp_call(address, hook_, call_before, call_after, pushad_pushfd);
        return finalize_install();
    }

    bool HookV2::install_call(void *address, std::span<const PatternByteV2> expected,
                              const void *expected_target,
                              const void *call_before, const void *call_after,
                              bool pushad_pushfd) noexcept {
        if(!expected_target ||
           CodeFinderV2::relative_call_target(address) != expected_target) {
            return false;
        }
        return install_inline(address, expected, call_before, call_after, pushad_pushfd);
    }

    bool HookV2::install_function(void *address, std::span<const PatternByteV2> expected,
                                  const void *new_function, const void **original_function) noexcept {
        if(active() || !original_function || !expected_matches(address, expected) ||
           !CodeFinderV2::executable_range(address, 16) ||
           !CodeFinderV2::executable_range(new_function, 1) ||
           !range_available_v2(this, address, 5)) {
            return false;
        }

        *original_function = nullptr;
        write_function_override(address, hook_, new_function, original_function);
        if(!finalize_install()) {
            *original_function = nullptr;
            return false;
        }

        return true;
    }

    bool HookV2::rollback() noexcept {
        if(!active()) {
            return true;
        }

        if(!owns_current_bytes()) {
            abandon();
            return false;
        }

        overwrite(hook_.address, hook_.original_bytes.data(), hook_.original_bytes.size());
        if(!bytes_equal_v2(hook_.address, hook_.original_bytes)) {
            return false;
        }

        if(registered_) {
            unregister_range_v2(this);
        }

        reset_state();
        return true;
    }

    bool HookV2::active() const noexcept {
        return hook_.address && hook_.hook && !hook_.original_bytes.empty() && !installed_bytes_.empty();
    }

    bool HookV2::owns_current_bytes() const noexcept {
        return active() && bytes_equal_v2(hook_.address, installed_bytes_);
    }

    std::byte *HookV2::address() const noexcept {
        return hook_.address;
    }

    std::size_t HookV2::size() const noexcept {
        return installed_bytes_.size();
    }

    const char *HookV2::owner() const noexcept {
        return owner_.c_str();
    }

    void HookV2::reset_state() noexcept {
        if(registered_) {
            unregister_range_v2(this);
        }
        registered_ = false;
        installed_bytes_.clear();
        hook_.original_bytes.clear();
        hook_.hook.reset();
        hook_.address = nullptr;
    }

    void HookV2::abandon() noexcept {
        if(registered_) {
            unregister_range_v2(this);
        }
        registered_ = false;
        installed_bytes_.clear();
        hook_.original_bytes.clear();
        hook_.hook.release();
        hook_.address = nullptr;
    }
}
