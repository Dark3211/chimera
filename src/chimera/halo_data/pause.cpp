// SPDX-License-Identifier: GPL-3.0-only

#include "../signature/signature.hpp"
#include "../chimera.hpp"

namespace Chimera {
    bool game_paused() noexcept {
        static std::byte **paused_addr = *reinterpret_cast<std::byte ***>(get_chimera().get_signature("game_paused_sig").data() + 2);
        return *reinterpret_cast<bool *>(*paused_addr + 2);
    }
}
