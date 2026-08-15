// SPDX-License-Identifier: GPL-3.0-only

#include "../../../localization/localization.hpp"
#include "../../../halo_data/player.hpp"
#include "../../../halo_data/object.hpp"
#include "../../../halo_data/multiplayer.hpp"
#include "../../../output/output.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace Chimera {
    static bool parse_finite_float(const char *text, float &value) noexcept {
        if(!text || !*text) {
            return false;
        }
        errno = 0;
        char *end = nullptr;
        value = std::strtof(text, &end);
        return errno != ERANGE && end != text && end && *end == '\0' && std::isfinite(value);
    }

    static bool parse_player_number(const char *text, int &index) noexcept {
        if(!text || !*text) {
            return false;
        }
        errno = 0;
        char *end = nullptr;
        unsigned long value = std::strtoul(text, &end, 10);
        if(errno == ERANGE || end == text || !end || *end != '\0' || value == 0 || value > static_cast<unsigned long>(std::numeric_limits<int>::max())) {
            return false;
        }
        index = static_cast<int>(value - 1);
        return true;
    }

    bool teleport_command(int argc, const char **argv) {
        // Prevent desyncs if needed
        if(server_type() == ServerType::SERVER_DEDICATED) {
            console_error(localize("chimera_error_must_be_host"));
            return false;
        }

        float x, y, z;
        auto &player_table = PlayerTable::get_player_table();
        auto &object_table = ObjectTable::get_object_table();

        // Teleport to specific coordinates
        if(argc == 3 || argc == 4) {
            if(!parse_finite_float(argv[argc - 3], x) || !parse_finite_float(argv[argc - 2], y) || !parse_finite_float(argv[argc - 1], z)) {
                console_error(localize("chimera_teleport_invalid_arguments"));
                return false;
            }
        }

        // Teleport to a player
        else if(argc == 1 || argc == 2) {
            // Get the player
            Player *player;
            int index;
            if(!parse_player_number(argv[argc - 1], index)) {
                console_error(localize("chimera_error_takes_player_number"));
                return false;
            }
            player = player_table.get_player_by_rcon_id(index);

            // If the player does not exist show an error
            if(!player) {
                console_error(localize("chimera_error_player_not_found"), argc ? *argv : nullptr);
                return false;
            }

            // Is the player alive?
            auto *object = object_table.get_dynamic_object(player->object_id);
            if(!object) {
                console_error(localize("chimera_teleport_dead"));
                return false;
            }

            x = object->object.bounding_sphere_center.x;
            y = object->object.bounding_sphere_center.y;
            z = object->object.bounding_sphere_center.z;
        }

        else {
            console_error(localize("chimera_teleport_invalid_arguments"));
            return false;
        }

        // Get the player
        Player *local_player;

        // What player are we teleporting?
        if(argc == 1 || argc == 3) {
            local_player = player_table.get_client_player();
        }
        else {
            int index;
            if(!parse_player_number(argv[0], index)) {
                console_error(localize("chimera_error_takes_player_number"));
                return false;
            }
            local_player = player_table.get_player_by_rcon_id(index);
        }

        if(!local_player) {
            console_error(localize("chimera_teleport_dead_self"));
            return false;
        }

        // Is the player alive?
        auto *player_object = object_table.get_dynamic_object(local_player->object_id);
        if(!player_object) {
            console_error(localize("chimera_teleport_dead_self"));
            return false;
        }

        // Get the parent
        while(true) {
            auto *parent_object = object_table.get_dynamic_object(player_object->object.parent_object_index);
            if(parent_object) {
                player_object = parent_object;
            }
            else {
                break;
            }
        }

        player_object->object.position.x = x;
        player_object->object.position.y = y;
        player_object->object.position.z = z;

        if(server_type() == ServerType::SERVER_NONE) {
            console_output(localize("chimera_teleport_success_sp"), x, y, z);
        }
        else {
            console_output(localize("chimera_teleport_success_mp"), local_player->name, x, y, z);
        }

        return true;
    }
}
