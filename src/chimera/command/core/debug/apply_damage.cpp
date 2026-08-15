// SPDX-License-Identifier: GPL-3.0-only

#include "../../../halo_data/tag.hpp"
#include "../../../output/output.hpp"
#include "../../../halo_data/object.hpp"
#include "../../../halo_data/player.hpp"
#include "../../../halo_data/damage.hpp"

#include <optional>
#include <string>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace Chimera {
    static std::optional<std::uint32_t> read_hex(const char *text) noexcept {
        if(!text || !*text) {
            return std::nullopt;
        }

        try {
            std::size_t parsed = 0;
            auto value = std::stoull(text, &parsed, 16);
            if(text[parsed] != '\0' || value > std::numeric_limits<std::uint32_t>::max()) {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(value);
        }
        catch(...) {
            return std::nullopt;
        }
    }

    static std::optional<long long> read_decimal(const char *text) noexcept {
        if(!text || !*text) {
            return std::nullopt;
        }

        try {
            std::size_t parsed = 0;
            auto value = std::stoll(text, &parsed, 10);
            if(text[parsed] != '\0') {
                return std::nullopt;
            }
            return value;
        }
        catch(...) {
            return std::nullopt;
        }
    }

    static std::optional<float> read_multiplier(const char *text) noexcept {
        if(!text || !*text) {
            return std::nullopt;
        }

        errno = 0;
        char *end = nullptr;
        float value = std::strtof(text, &end);
        if(errno == ERANGE || end == text || !end || *end != '\0' || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    }

    bool apply_damage_command(int argc, const char **argv) {
        // Get the damage effect
        auto *tag = get_tag(argv[0], TagClassInt::TAG_CLASS_DAMAGE_EFFECT);
        if(!tag) {
            console_error("Tag path does not correspond to a valid damage_effect tag");
            return false;
        }

        TagID damage_effect = tag->id;
        PlayerID causer_player_id = PlayerID::null_id();
        ObjectID damaged_object_id;
        ObjectID causer_object_id = ObjectID::null_id();
        auto &object_table = ObjectTable::get_object_table();
        float multiplier = 1.0F;

        // Make sure the object is valid
        auto damaged_object_id_value = read_hex(argv[1]);
        if(!damaged_object_id_value.has_value()) {
            console_error("Damaged Object ID given is invalid (must be hexadecimal)");
            return false;
        }
        damaged_object_id.whole_id = *damaged_object_id_value;
        auto *damaged_object = object_table.get_dynamic_object(damaged_object_id);
        if(!damaged_object) {
            console_error("Damaged Object ID does not correspond to a valid object");
            return false;
        }

        // Get multiplier
        if(argc >= 3) {
            auto multiplier_value = read_multiplier(argv[2]);
            if(!multiplier_value.has_value()) {
                console_error("Multiplier must be a valid finite number");
                return false;
            }
            multiplier = *multiplier_value;
        }

        // Get causer player/object
        if(argc >= 4) {
            auto player_index_value = read_decimal(argv[3]);
            if(!player_index_value.has_value()) {
                console_error("Damager Player Index given is invalid (must be a valid rcon index or -1)");
                return false;
            }

            auto player_index = *player_index_value;
            if(player_index != -1) {
                if(player_index <= 0 || player_index > std::numeric_limits<std::int32_t>::max()) {
                    console_error("Damager Player Index given is invalid (must be a valid rcon index or -1)");
                    return false;
                }
                auto &player_table = PlayerTable::get_player_table();
                auto *player = player_table.get_player_by_rcon_id(static_cast<std::int32_t>(player_index - 1));
                if(!player) {
                    console_error("Damager Player Index given is invalid (must be a valid rcon index or -1)");
                    return false;
                }
                causer_player_id = player->get_full_id();
            }

            if(argc == 5) {
                auto causer_object_id_value = read_hex(argv[4]);
                if(!causer_object_id_value.has_value()) {
                    console_error("Damager Object ID given is invalid (must be hexadecimal)");
                    return false;
                }
                causer_object_id.whole_id = *causer_object_id_value;
                auto *causer_object = object_table.get_dynamic_object(causer_object_id);
                if(!causer_object) {
                    console_error("Damager Object ID does not correspond to a valid object");
                    return false;
                }
            }
        }

        apply_damage(damaged_object_id, damage_effect, multiplier, causer_player_id, causer_object_id);

        return true;
    }
}
