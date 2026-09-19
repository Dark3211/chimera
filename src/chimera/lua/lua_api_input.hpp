// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_LUA_API_INPUT_HPP
#define CHIMERA_LUA_API_INPUT_HPP

#include "../chimera.hpp"
#include "../halo_data/controls.hpp"
#include "lua_api_common.hpp"

namespace Chimera {
    template<typename Controls> static void lua_api_push_input_state(lua_State *state, const Controls &controls) noexcept {
        lua_createtable(state, 0, 17);
        lua_api_set_boolean(state, "jump", controls.jump != 0);
        lua_api_set_boolean(state, "switch_grenade", controls.switch_grenade != 0);
        lua_api_set_boolean(state, "action", controls.action != 0);
        lua_api_set_boolean(state, "switch_weapon", controls.switch_weapon != 0);
        lua_api_set_boolean(state, "melee", controls.melee != 0);
        lua_api_set_boolean(state, "flashlight", controls.flashlight != 0);
        lua_api_set_boolean(state, "secondary_fire", controls.secondary_fire != 0);
        lua_api_set_boolean(state, "primary_fire", controls.primary_fire != 0);
        lua_api_set_boolean(state, "crouch", controls.crouch != 0);
        lua_api_set_boolean(state, "zoom", controls.zoom != 0);
        lua_api_set_boolean(state, "scores", controls.scores != 0);
        lua_api_set_boolean(state, "reload", controls.reload != 0);
        lua_api_set_boolean(state, "exchange_weapons", controls.exchange_weapons != 0);
        lua_api_set_number(state, "move_forward", controls.move_forward);
        lua_api_set_number(state, "move_left", controls.move_left);
        lua_api_set_number(state, "aim_left", controls.aim_left);
        lua_api_set_number(state, "aim_up", controls.aim_up);
    }

    static int lua_api_get_input_state(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_input_state");
        }
        if(!get_chimera().feature_present("client")) {
            lua_pushnil(state);
            return 1;
        }

        if(game_engine() == GAME_ENGINE_CUSTOM_EDITION) {
            lua_api_push_input_state(state, get_custom_edition_controls());
        }
        else {
            lua_api_push_input_state(state, get_retail_demo_controls());
        }
        return 1;
    }
}

#endif
