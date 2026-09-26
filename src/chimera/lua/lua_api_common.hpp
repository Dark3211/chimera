

#ifndef CHIMERA_LUA_API_COMMON_HPP
#define CHIMERA_LUA_API_COMMON_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <windows.h>
#include <lua.hpp>
#include "../halo_data/game_engine.hpp"
#include "../halo_data/map.hpp"
#include "../halo_data/multiplayer.hpp"
#include "../halo_data/object.hpp"
#include "../halo_data/player.hpp"
#include "../halo_data/server.hpp"
#include "../localization/localization.hpp"

namespace Chimera {
    static void lua_api_set_integer(lua_State *state, const char *key, lua_Integer value) noexcept {
        lua_pushinteger(state, value);
        lua_setfield(state, -2, key);
    }

    static void lua_api_set_number(lua_State *state, const char *key, lua_Number value) noexcept {
        lua_pushnumber(state, value);
        lua_setfield(state, -2, key);
    }

    static void lua_api_set_boolean(lua_State *state, const char *key, bool value) noexcept {
        lua_pushboolean(state, value);
        lua_setfield(state, -2, key);
    }

    static void lua_api_set_string(lua_State *state, const char *key, const char *value) noexcept {
        if(value) {
            lua_pushstring(state, value);
        }
        else {
            lua_pushnil(state);
        }
        lua_setfield(state, -2, key);
    }

    static void lua_api_set_id(lua_State *state, const char *key, HaloID value) noexcept {
        if(value.is_null()) {
            lua_pushnil(state);
        }
        else {
            lua_pushinteger(state, static_cast<lua_Integer>(value.whole_id));
        }
        lua_setfield(state, -2, key);
    }

    static std::string lua_api_bounded_string(const char *value, std::size_t maximum_length) {
        if(!value || maximum_length == 0) {
            return {};
        }
        std::size_t length = 0;
        while(length < maximum_length && value[length] != 0) {
            length++;
        }
        return std::string(value, length);
    }

    static std::string lua_api_wide_to_utf8(const wchar_t *value, std::size_t maximum_length) {
        if(!value || maximum_length == 0) {
            return {};
        }
        std::size_t length = 0;
        while(length < maximum_length && value[length] != 0) {
            length++;
        }
        if(length == 0 || length > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return {};
        }
        int required = WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if(required <= 0) {
            return {};
        }
        std::string result(static_cast<std::size_t>(required), '\0');
        if(WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), &result[0], required, nullptr, nullptr) != required) {
            return {};
        }
        return result;
    }

    static void lua_api_set_wide_string(lua_State *state, const char *key, const wchar_t *value, std::size_t maximum_length) {
        auto converted = lua_api_wide_to_utf8(value, maximum_length);
        lua_pushlstring(state, converted.data(), converted.size());
        lua_setfield(state, -2, key);
    }

    static void lua_api_set_point(lua_State *state, const char *key, const Point3D &point) noexcept {
        lua_createtable(state, 0, 3);
        lua_api_set_number(state, "x", point.x);
        lua_api_set_number(state, "y", point.y);
        lua_api_set_number(state, "z", point.z);
        lua_setfield(state, -2, key);
    }

    static BaseDynamicObject *lua_api_resolve_object(lua_State *state, int argument) noexcept {
        if(!lua_isnumber(state, argument)) {
            return nullptr;
        }
        auto number = lua_tonumber(state, argument);
        if(!std::isfinite(number) || number < 0.0 || number > static_cast<lua_Number>(std::numeric_limits<std::uint32_t>::max()) || std::floor(number) != number) {
            return nullptr;
        }
        auto value = static_cast<std::uint32_t>(number);
        auto &table = ObjectTable::get_object_table();
        if(value <= std::numeric_limits<std::uint16_t>::max()) {
            return table.get_dynamic_object(value);
        }
        ObjectID id;
        id.whole_id = value;
        return table.get_dynamic_object(id);
    }

    static Player *lua_api_resolve_player(lua_State *state) noexcept {
        auto args = lua_gettop(state);
        auto &table = PlayerTable::get_player_table();
        if(args == 0) {
            return table.get_client_player();
        }
        if(args != 1 || !lua_isnumber(state, 1)) {
            return nullptr;
        }
        auto number = lua_tonumber(state, 1);
        if(!std::isfinite(number) || number < 0.0 || number > static_cast<lua_Number>(std::numeric_limits<std::uint32_t>::max()) || std::floor(number) != number) {
            return nullptr;
        }
        auto value = static_cast<std::uint32_t>(number);
        if(value < 16) {
            return table.get_player_by_rcon_id(value);
        }
        PlayerID id;
        id.whole_id = value;
        return table.get_player(id);
    }

    static const char *lua_api_object_type_name(ObjectType type) noexcept {
        switch(type) {
            case OBJECT_TYPE_BIPED: return "biped";
            case OBJECT_TYPE_VEHICLE: return "vehicle";
            case OBJECT_TYPE_WEAPON: return "weapon";
            case OBJECT_TYPE_EQUIPMENT: return "equipment";
            case OBJECT_TYPE_GARBAGE: return "garbage";
            case OBJECT_TYPE_PROJECTILE: return "projectile";
            case OBJECT_TYPE_SCENERY: return "scenery";
            case OBJECT_TYPE_DEVICE_MACHINE: return "device_machine";
            case OBJECT_TYPE_DEVICE_CONTROL: return "device_control";
            case OBJECT_TYPE_DEVICE_LIGHT_FIXTURE: return "device_light_fixture";
            case OBJECT_TYPE_PLACEHOLDER: return "placeholder";
            case OBJECT_TYPE_SOUND_SCENERY: return "sound_scenery";
            default: return "unknown";
        }
    }

    static const char *lua_api_server_type_name(ServerType type) noexcept {
        switch(type) {
            case SERVER_DEDICATED: return "dedicated";
            case SERVER_LOCAL: return "local";
            default: return "none";
        }
    }

}

#endif
