// SPDX-License-Identifier: GPL-3.0-only

#include <exception>
#include "../signature/signature.hpp"
#include "../chimera.hpp"

#include "game_engine.hpp"

namespace Chimera {
    GameEngine game_engine() noexcept {
        static const GameEngine game_engine_used = []() noexcept -> GameEngine {
            const auto *game_engine_name = *reinterpret_cast<const char **>(get_chimera().get_signature("game_engine_sig").data() + 4);
            if(std::strcmp(game_engine_name, "halom") == 0) {
                return GameEngine::GAME_ENGINE_CUSTOM_EDITION;
            }
            if(std::strcmp(game_engine_name, "halor") == 0) {
                return GameEngine::GAME_ENGINE_RETAIL;
            }
            if(std::strcmp(game_engine_name, "halod") == 0) {
                return GameEngine::GAME_ENGINE_DEMO;
            }

            // Match the previous noexcept + optional::value() failure semantics for an unknown engine.
            std::terminate();
        }();
        return game_engine_used;
    }
}
