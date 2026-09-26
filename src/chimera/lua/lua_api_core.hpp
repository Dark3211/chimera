

#ifndef CHIMERA_LUA_API_CORE_HPP
#define CHIMERA_LUA_API_CORE_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <lua.hpp>
#include "../chimera.hpp"
#include "../event/tick.hpp"
#include "../event/interface_render.hpp"
#include "../halo_data/game_engine.hpp"
#include "../halo_data/object.hpp"
#include "../halo_data/player.hpp"
#include "../halo_data/resolution.hpp"
#include "../halo_data/tag.hpp"
#include "../halo_data/tag_class.hpp"
#include "../localization/localization.hpp"
#include "lua_script.hpp"

namespace Chimera {
    static int lua_api_feature_present(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "feature_present");
        }
        const auto *feature = luaL_checkstring(state, 1);
        if(std::strcmp(feature, "client_hud_render_event") == 0) {
            lua_pushboolean(state, hud_render_event_supported());
        }
        else if(std::strcmp(feature, "client_ui_render_event") == 0) {
            lua_pushboolean(state, ui_render_event_supported());
        }
        else {
            lua_pushboolean(state, get_chimera().feature_present(feature));
        }
        return 1;
    }

    static int lua_api_get_api_version(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_api_version");
        }
        lua_pushnumber(state, CHIMERA_LUA_VERSION);
        return 1;
    }

    static int lua_api_get_client_player_id(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_client_player_id");
        }
        auto player_id = get_client_player_id();
        if(player_id.is_null()) {
            lua_pushnil(state);
        }
        else {
            lua_pushinteger(state, static_cast<lua_Integer>(player_id.whole_id));
        }
        return 1;
    }

    static int lua_api_get_game_engine(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_game_engine");
        }
        switch(game_engine()) {
            case GAME_ENGINE_CUSTOM_EDITION:
                lua_pushstring(state, "custom_edition");
                break;
            case GAME_ENGINE_RETAIL:
                lua_pushstring(state, "retail");
                break;
            case GAME_ENGINE_DEMO:
                lua_pushstring(state, "demo");
                break;
        }
        return 1;
    }

    static int lua_api_get_object_count(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_object_count");
        }
        lua_pushinteger(state, ObjectTable::get_object_table().count);
        return 1;
    }

    static int lua_api_get_object_id(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_object_id");
        }
        auto index = luaL_checkinteger(state, 1);
        if(index < 0 || index > std::numeric_limits<std::uint16_t>::max()) {
            lua_pushnil(state);
            return 1;
        }
        auto &table = ObjectTable::get_object_table();
        auto *entry = table.get_element(static_cast<std::size_t>(index));
        if(!entry || !entry->object) {
            lua_pushnil(state);
            return 1;
        }
        ObjectID object_id;
        object_id.index.index = static_cast<std::uint16_t>(index);
        object_id.index.id = entry->id;
        lua_pushinteger(state, static_cast<lua_Integer>(object_id.whole_id));
        return 1;
    }

    static int lua_api_get_object_ids(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_object_ids");
        }

        auto &table = ObjectTable::get_object_table();
        lua_createtable(state, table.count, 0);

        int output_index = 1;
        for(std::size_t i = 0; i < table.current_size; i++) {
            auto *entry = table.get_element(i);
            if(!entry || !entry->object) {
                continue;
            }

            ObjectID object_id;
            object_id.index.index = static_cast<std::uint16_t>(i);
            object_id.index.id = entry->id;
            lua_pushinteger(state, static_cast<lua_Integer>(object_id.whole_id));
            lua_rawseti(state, -2, output_index++);
        }
        return 1;
    }

    static int lua_api_get_player_count(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_player_count");
        }
        lua_pushinteger(state, PlayerTable::get_player_table().count);
        return 1;
    }

    static int lua_api_get_player_id(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_player_id");
        }
        auto index = luaL_checkinteger(state, 1);
        if(index < 0) {
            lua_pushnil(state);
            return 1;
        }
        auto *player = PlayerTable::get_player_table().get_player_by_rcon_id(static_cast<std::size_t>(index));
        if(player) {
            lua_pushinteger(state, static_cast<lua_Integer>(player->get_full_id().whole_id));
        }
        else {
            lua_pushnil(state);
        }
        return 1;
    }

    static int lua_api_get_player_ids(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_player_ids");
        }

        auto &table = PlayerTable::get_player_table();
        lua_createtable(state, table.count, 0);

        int output_index = 1;
        for(std::size_t i = 0; i < table.current_size; i++) {
            auto *player = table.get_element(i);
            if(!player || player->player_id == 0xFFFF) {
                continue;
            }

            lua_pushinteger(state, static_cast<lua_Integer>(player->get_full_id().whole_id));
            lua_rawseti(state, -2, output_index++);
        }
        return 1;
    }

    static int lua_api_get_resolution(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_resolution");
        }
        auto &resolution = get_resolution();
        lua_pushinteger(state, resolution.width);
        lua_pushinteger(state, resolution.height);
        return 2;
    }

    static int lua_api_get_tag_id(lua_State *state) noexcept {
        if(lua_gettop(state) != 2) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_tag_id");
        }
        const char *tag_class = luaL_checkstring(state, 1);
        const char *tag_path = luaL_checkstring(state, 2);
        Tag *tag = nullptr;
        auto tag_class_int = tag_class_from_string(tag_class);
        if(tag_class_int != TagClassInt::TAG_CLASS_NULL) {
            tag = get_tag(tag_path, tag_class_int);
        }
        else {
            tag = get_tag(tag_path, tag_class);
        }
        if(tag) {
            lua_pushinteger(state, static_cast<lua_Integer>(tag->id.whole_id));
        }
        else {
            lua_pushnil(state);
        }
        return 1;
    }

    static int lua_api_effective_tick_rate(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "effective_tick_rate");
        }
        lua_pushnumber(state, effective_tick_rate());
        return 1;
    }

    static int lua_api_tick_count(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "tick_count");
        }
        lua_pushinteger(state, get_tick_count());
        return 1;
    }

    static int lua_api_tick_progress(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "tick_progress");
        }
        lua_pushnumber(state, get_tick_progress());
        return 1;
    }
}

#endif
