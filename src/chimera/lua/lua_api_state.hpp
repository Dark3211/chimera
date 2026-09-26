

#ifndef CHIMERA_LUA_API_STATE_HPP
#define CHIMERA_LUA_API_STATE_HPP

#include "../chimera.hpp"
#include "../halo_data/cutscene.hpp"
#include "../halo_data/pause.hpp"
#include "lua_api_common.hpp"

namespace Chimera {
    static int lua_api_get_game_state(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_game_state");
        }
        if(!get_chimera().feature_present("client")) {
            lua_pushnil(state);
            return 1;
        }

        const auto &cinematic = get_cinematic_globals();

        lua_createtable(state, 0, 5);
        lua_api_set_boolean(state, "paused", game_paused());
        lua_api_set_boolean(state, "cinematic_in_progress", cinematic.cinematic_in_progress);
        lua_api_set_boolean(state, "cinematic_skip_in_progress", cinematic.cinematic_skip_in_progress);
        lua_api_set_boolean(state, "showing_letterbox", cinematic.showing_letter_box);
        lua_api_set_number(state, "letterbox_size", cinematic.letter_box_size);
        return 1;
    }
}

#endif
