// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_LUA_API_PLAYER_HPP
#define CHIMERA_LUA_API_PLAYER_HPP

#include "lua_api_common.hpp"

namespace Chimera {
    static int lua_api_get_player_info(lua_State *state) noexcept {
        if(lua_gettop(state) > 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_player_info");
        }
        auto *player = lua_api_resolve_player(state);
        if(!player) {
            lua_pushnil(state);
            return 1;
        }

        auto full_id = player->get_full_id();
        lua_createtable(state, 0, 36);
        lua_api_set_id(state, "id", full_id);
        lua_api_set_integer(state, "table_index", full_id.index.index);
        lua_api_set_wide_string(state, "name", player->name, 12);
        lua_api_set_integer(state, "team", player->team);
        lua_api_set_id(state, "interaction_object_id", player->interaction_object_id);
        lua_api_set_integer(state, "interaction_object_type", player->interaction_object_type);
        lua_api_set_integer(state, "interaction_object_seat", player->interaction_object_seat);
        lua_api_set_integer(state, "respawn_time", player->respawn_time);
        lua_api_set_integer(state, "respawn_time_growth", player->respawn_time_growth);
        lua_api_set_id(state, "object_id", player->object_id);
        lua_api_set_id(state, "last_object_id", player->last_object_id);
        lua_api_set_integer(state, "color", player->color);
        lua_api_set_integer(state, "machine_index", player->machine_index);
        lua_api_set_integer(state, "index", player->index);
        lua_api_set_integer(state, "invisibility_time", player->invisibility_time);
        lua_api_set_number(state, "speed", player->speed);
        lua_api_set_integer(state, "last_fire_time", player->last_fire_time);
        lua_api_set_integer(state, "last_death_time", player->last_death_time);
        lua_api_set_id(state, "slayer_target", player->slayer_target);
        lua_api_set_boolean(state, "odd_man_out", player->odd_man_out != 0);
        lua_api_set_integer(state, "kill_streak", player->kill_streak);
        lua_api_set_integer(state, "last_kill_time", player->last_kill_time);
        lua_api_set_integer(state, "kills", player->kills);
        lua_api_set_integer(state, "assists", player->assists);
        lua_api_set_integer(state, "betrays_and_suicides", player->betrays);
        lua_api_set_integer(state, "deaths", player->deaths);
        lua_api_set_integer(state, "suicides", player->suicides);
        auto betrayals = player->betrays >= player->suicides ? player->betrays - player->suicides : 0;
        lua_api_set_integer(state, "betrayals", betrayals);
        lua_api_set_integer(state, "ping", player->ping);
        lua_api_set_integer(state, "team_kill_count", player->team_kill_count);
        lua_api_set_integer(state, "team_kill_timer", player->team_kill_timer);
        lua_api_set_boolean(state, "melee", player->melee != 0);
        lua_api_set_boolean(state, "action", player->action != 0);
        lua_api_set_boolean(state, "flashlight", player->flashlight != 0);
        lua_api_set_boolean(state, "reload", player->reload != 0);
        lua_api_set_point(state, "position", player->position);

        ObjectID vehicle = HaloID::null_id();
        ObjectID current_weapon = HaloID::null_id();
        lua_Integer vehicle_seat = -1;
        auto *object = ObjectTable::get_object_table().get_dynamic_object(player->object_id);
        if(object) {
            if(!object->object.parent_object_index.is_null()) {
                auto *parent = ObjectTable::get_object_table().get_dynamic_object(object->object.parent_object_index);
                if(parent && parent->object.type == OBJECT_TYPE_VEHICLE) {
                    vehicle = object->object.parent_object_index;
                }
            }
            if(object->object.type == OBJECT_TYPE_BIPED || object->object.type == OBJECT_TYPE_VEHICLE) {
                auto *unit = reinterpret_cast<UnitDynamicObject *>(object);
                if(unit->unit.current_weapon_index < MAXIMUM_WEAPONS_PER_UNIT) {
                    current_weapon = unit->unit.weapon_object_indices[unit->unit.current_weapon_index];
                }
                if(unit->unit.parent_seat_index != 0xFFFF) {
                    vehicle_seat = unit->unit.parent_seat_index;
                }
            }
        }
        lua_api_set_id(state, "vehicle_id", vehicle);
        lua_api_set_boolean(state, "in_vehicle", !vehicle.is_null());
        if(vehicle_seat < 0) {
            lua_pushnil(state);
        }
        else {
            lua_pushinteger(state, vehicle_seat);
        }
        lua_setfield(state, -2, "vehicle_seat");
        lua_api_set_id(state, "current_weapon_id", current_weapon);
        return 1;
    }
}

#endif
