

#ifndef CHIMERA_LUA_API_TAG_HPP
#define CHIMERA_LUA_API_TAG_HPP

#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include "../halo_data/tag.hpp"
#include "lua_api_common.hpp"

namespace Chimera {
    static bool lua_api_tag_path_is_valid(const char *path) noexcept {
        std::size_t remaining = 0;
        return lua_api_tag_data_remaining_size(path, remaining)
            && std::memchr(path, 0, remaining) != nullptr;
    }

    static bool lua_api_tag_count_is_safe(std::uint32_t count) noexcept {
        return static_cast<std::size_t>(count) <= LUA_API_TAG_DATA_SAFE_REGION_SIZE / sizeof(Tag);
    }

    static int lua_api_get_tag_count(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_tag_count");
        }

        const auto count = get_tag_data_header().tag_count;
        if(!lua_api_tag_count_is_safe(count)) {
            lua_pushnil(state);
            return 1;
        }

        lua_pushinteger(state, static_cast<lua_Integer>(count));
        return 1;
    }

    static int lua_api_get_tag_id_by_index(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_tag_id_by_index");
        }
        if(!lua_isnumber(state, 1)) {
            lua_pushnil(state);
            return 1;
        }

        const auto value = lua_tonumber(state, 1);
        if(!std::isfinite(value) || value < 0.0 || std::floor(value) != value) {
            lua_pushnil(state);
            return 1;
        }

        const auto count = get_tag_data_header().tag_count;
        if(!lua_api_tag_count_is_safe(count) || value >= static_cast<lua_Number>(count)) {
            lua_pushnil(state);
            return 1;
        }

        auto *tag = get_tag(static_cast<std::size_t>(value));
        if(!tag || tag->id.is_null()) {
            lua_pushnil(state);
            return 1;
        }

        lua_pushinteger(state, static_cast<lua_Integer>(tag->id.whole_id));
        return 1;
    }

    static int lua_api_get_tag_info(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_tag_info");
        }
        if(!lua_isnumber(state, 1)) {
            lua_pushnil(state);
            return 1;
        }

        auto number = lua_tonumber(state, 1);
        if(!std::isfinite(number) || number < 0.0 || number > static_cast<lua_Number>(std::numeric_limits<std::uint32_t>::max()) || std::floor(number) != number) {
            lua_pushnil(state);
            return 1;
        }

        TagID requested_id;
        requested_id.whole_id = static_cast<std::uint32_t>(number);
        auto *tag = get_tag(requested_id);
        if(!tag || tag->id.whole_id != requested_id.whole_id) {
            lua_pushnil(state);
            return 1;
        }

        lua_createtable(state, 0, 3);
        lua_api_set_id(state, "id", tag->id);

        const auto tag_class = static_cast<std::uint32_t>(tag->primary_class);
        const char class_name[4] = {
            static_cast<char>((tag_class >> 24) & 0xFF),
            static_cast<char>((tag_class >> 16) & 0xFF),
            static_cast<char>((tag_class >> 8) & 0xFF),
            static_cast<char>(tag_class & 0xFF)
        };
        lua_pushlstring(state, class_name, sizeof(class_name));
        lua_setfield(state, -2, "class");

        if(lua_api_tag_path_is_valid(tag->path)) {
            lua_pushstring(state, tag->path);
        }
        else {
            lua_pushnil(state);
        }
        lua_setfield(state, -2, "path");
        return 1;
    }
}

#endif
