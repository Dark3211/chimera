// SPDX-License-Identifier: GPL-3.0-only

#include <cstddef>
#include "../signature/signature.hpp"
#include "../chimera.hpp"
#include "../output/output.hpp"

#include "multiplayer.hpp"

namespace Chimera {
    struct CurrentGametypeData {
        Gametype type;
        std::uint16_t pad_02;
        std::uint8_t team_game;
        std::uint8_t pad_05[0x23];
        std::uint8_t score_limit;
    };
    static_assert(offsetof(CurrentGametypeData, team_game) == 0x4);
    static_assert(offsetof(CurrentGametypeData, score_limit) == 0x28);

    static CurrentGametypeData *current_gametype_data() noexcept {
        static auto *data = *reinterpret_cast<std::uint8_t **>(get_chimera().get_signature("current_gametype_sig").data() + 2);
        return reinterpret_cast<CurrentGametypeData *>(data);
    }

    ServerType server_type() {
        static auto *server_type = *reinterpret_cast<ServerType **>(get_chimera().get_signature("server_type_sig").data() + 3);
        return *server_type;
    }

    Gametype gametype() {
        return current_gametype_data()->type;
    }

    bool is_team() {
        return current_gametype_data()->team_game != 0;
    }

    std::uint8_t score_limit() noexcept {
        return current_gametype_data()->score_limit;
    }
}
