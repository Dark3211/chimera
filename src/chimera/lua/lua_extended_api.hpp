// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_LUA_EXTENDED_API_HPP
#define CHIMERA_LUA_EXTENDED_API_HPP

#include "lua_api_core.hpp"
#include "lua_api_player.hpp"
#include "lua_api_object.hpp"
#include "lua_api_unit.hpp"
#include "lua_api_input.hpp"
#include "lua_api_tag.hpp"
#include "lua_api_camera.hpp"
#include "lua_api_state.hpp"
#include "lua_api_misc.hpp"

namespace Chimera {
    inline void set_extended_api_functions(lua_State *state) noexcept {
        lua_register(state, "feature_present", lua_api_feature_present);
        lua_register(state, "get_api_version", lua_api_get_api_version);
        lua_register(state, "get_client_player_id", lua_api_get_client_player_id);
        lua_register(state, "get_game_engine", lua_api_get_game_engine);
        lua_register(state, "get_object_count", lua_api_get_object_count);
        lua_register(state, "get_object_id", lua_api_get_object_id);
        lua_register(state, "get_player_count", lua_api_get_player_count);
        lua_register(state, "get_player_id", lua_api_get_player_id);
        lua_register(state, "get_resolution", lua_api_get_resolution);
        lua_register(state, "get_tag_id", lua_api_get_tag_id);
        lua_register(state, "get_tag_info", lua_api_get_tag_info);
        lua_register(state, "get_tag_count", lua_api_get_tag_count);
        lua_register(state, "get_tag_id_by_index", lua_api_get_tag_id_by_index);
        lua_register(state, "get_scenario_tag_id", lua_api_get_scenario_tag_id);
        lua_register(state, "get_camera_info", lua_api_get_camera_info);
        lua_register(state, "get_game_state", lua_api_get_game_state);
        lua_register(state, "effective_tick_rate", lua_api_effective_tick_rate);
        lua_register(state, "tick_count", lua_api_tick_count);
        lua_register(state, "tick_progress", lua_api_tick_progress);
        lua_register(state, "get_player_info", lua_api_get_player_info);
        lua_register(state, "get_object_info", lua_api_get_object_info);
        lua_register(state, "get_unit_info", lua_api_get_unit_info);
        lua_register(state, "get_input_state", lua_api_get_input_state);
        lua_register(state, "get_weapon_info", lua_api_get_weapon_info);
        lua_register(state, "get_map_info", lua_api_get_map_info);
        lua_register(state, "get_multiplayer_info", lua_api_get_multiplayer_info);
        lua_register(state, "get_server_players", lua_api_get_server_players);
    }
}

#endif
