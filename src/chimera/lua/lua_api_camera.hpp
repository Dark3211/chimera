

#ifndef CHIMERA_LUA_API_CAMERA_HPP
#define CHIMERA_LUA_API_CAMERA_HPP

#include "../halo_data/camera.hpp"
#include "lua_api_common.hpp"

namespace Chimera {
    static const char *lua_api_camera_type_name(CameraType type) noexcept {
        switch(type) {
            case CAMERA_FIRST_PERSON: return "first_person";
            case CAMERA_VEHICLE: return "vehicle";
            case CAMERA_CINEMATIC: return "cinematic";
            case CAMERA_DEBUG: return "debug";
            default: return "unknown";
        }
    }

    static int lua_api_get_camera_info(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_camera_info");
        }

        const auto type = camera_type();
        const auto &data = camera_data();

        lua_createtable(state, 0, 5);
        lua_api_set_integer(state, "type", static_cast<lua_Integer>(type));
        lua_api_set_string(state, "type_name", lua_api_camera_type_name(type));
        lua_api_set_point(state, "position", data.position);

        lua_createtable(state, 2, 0);
        for(std::size_t i = 0; i < 2; i++) {
            lua_createtable(state, 0, 3);
            lua_api_set_number(state, "x", data.orientation[i].x);
            lua_api_set_number(state, "y", data.orientation[i].y);
            lua_api_set_number(state, "z", data.orientation[i].z);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "orientation");

        lua_api_set_number(state, "fov", data.fov);
        return 1;
    }
}

#endif
