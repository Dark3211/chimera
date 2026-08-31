// SPDX-License-Identifier: GPL-3.0-only

#include "../signature/signature.hpp"
#include "../chimera.hpp"
#include "../output/output.hpp"

#include "multiplayer.hpp"

namespace Chimera {
    static auto *current_gametype_data() noexcept {
        static auto *data = *reinterpret_cast<std::uint8_t **>(get_chimera().get_signature("current_gametype_sig").data() + 2);
        return data;
    }

    ServerType server_type() {
        static auto *server_type = *reinterpret_cast<ServerType **>(get_chimera().get_signature("server_type_sig").data() + 3);
        return *server_type;
    }

    Gametype gametype() {
        return *reinterpret_cast<Gametype *>(current_gametype_data());
    }

    bool is_team() {
        return *(current_gametype_data() + 4);
    }
}
