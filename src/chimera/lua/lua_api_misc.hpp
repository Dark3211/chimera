// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_LUA_API_MISC_HPP
#define CHIMERA_LUA_API_MISC_HPP

#include "lua_api_common.hpp"

namespace Chimera {
    static const char *lua_api_weapon_state_name(WeaponState state) noexcept {
        switch(state) {
            case WEAPON_STATE_IDLE: return "idle";
            case WEAPON_STATE_PRIMARY_RECOIL: return "primary_recoil";
            case WEAPON_STATE_SECONDARY_RECOIL: return "secondary_recoil";
            case WEAPON_STATE_PRIMARY_CHAMBER: return "primary_chamber";
            case WEAPON_STATE_SECONDARY_CHAMBER: return "secondary_chamber";
            case WEAPON_STATE_PRIMARY_RELOAD: return "primary_reload";
            case WEAPON_STATE_SECONDARY_RELOAD: return "secondary_reload";
            case WEAPON_STATE_PRIMARY_CHARGED: return "primary_charged";
            case WEAPON_STATE_SECONDARY_CHARGED: return "secondary_charged";
            case WEAPON_STATE_READY: return "ready";
            case WEAPON_STATE_PUT_AWAY: return "put_away";
            default: return "unknown";
        }
    }

    static const char *lua_api_weapon_trigger_state_name(WeaponTriggerState state) noexcept {
        switch(state) {
            case WEAPON_TRIGGER_STATE_IDLE: return "idle";
            case WEAPON_TRIGGER_STATE_OVERLOADING: return "overloading";
            case WEAPON_TRIGGER_STATE_CHARGING: return "charging";
            case WEAPON_TRIGGER_STATE_CHARGED: return "charged";
            case WEAPON_TRIGGER_STATE_RECOVERING: return "recovering";
            case WEAPON_TRIGGER_STATE_TRACKING: return "tracking";
            case WEAPON_TRIGGER_STATE_SPEWING: return "spewing";
            case WEAPON_TRIGGER_STATE_LOCKED: return "locked";
            case WEAPON_TRIGGER_STATE_UNINITIALIZED: return "uninitialized";
            default: return "unknown";
        }
    }

    static const char *lua_api_weapon_magazine_state_name(WeaponMagazineState state) noexcept {
        switch(state) {
            case WEAPON_MAGAZINE_STATE_IDLE: return "idle";
            case WEAPON_MAGAZINE_STATE_RELOADING: return "reloading";
            case WEAPON_MAGAZINE_STATE_UNCHAMBERED: return "unchambered";
            case WEAPON_MAGAZINE_STATE_CHAMBERING: return "chambering";
            default: return "unknown";
        }
    }

    static int lua_api_get_weapon_info(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_weapon_info");
        }
        auto *object = lua_api_resolve_object(state, 1);
        if(!object || object->object.type != OBJECT_TYPE_WEAPON) {
            lua_pushnil(state);
            return 1;
        }

        auto *weapon = reinterpret_cast<WeaponDynamicObject *>(object);
        const auto &item = weapon->item;
        const auto &datum = weapon->weapon;

        lua_createtable(state, 0, 25);
        lua_api_set_id(state, "object_id", object->full_object_id());

        lua_api_set_integer(state, "state", datum.state);
        lua_api_set_string(state, "state_name", lua_api_weapon_state_name(datum.state));
        lua_api_set_integer(state, "last_reported_state", datum.last_reported_state);
        lua_api_set_string(state, "last_reported_state_name", lua_api_weapon_state_name(datum.last_reported_state));
        lua_api_set_integer(state, "state_timer", datum.state_timer);

        lua_api_set_number(state, "primary_trigger", datum.primary_trigger);
        lua_api_set_number(state, "heat", datum.heat);
        lua_api_set_number(state, "age", datum.age);
        lua_api_set_number(state, "overcharged", datum.overcharged);
        lua_api_set_number(state, "integrated_light_power", datum.integrated_light_power);
        lua_api_set_integer(state, "integrated_light_delay_ticks", datum.integrated_light_delay_ticks);
        lua_api_set_number(state, "recoil_angular_velocity", datum.recoil_angular_velocity);
        lua_api_set_integer(state, "recoil_recovery_time", datum.recoil_recovery_time);
        lua_api_set_integer(state, "shots_until_demotion", datum.shots_until_demotion);
        lua_api_set_integer(state, "alternate_shots_loaded", datum.alternate_shots_loaded);
        lua_api_set_integer(state, "game_time_last_fired", datum.game_time_last_fired);

        lua_api_set_boolean(state, "in_unit_inventory", (item.flags & (1u << ITEM_DATUM_FLAGS_IN_UNIT_INVENTORY_BIT)) != 0);
        lua_api_set_boolean(state, "belongs_to_player", (item.flags & (1u << ITEM_DATUM_FLAGS_BELONGS_TO_PLAYER_BIT)) != 0);
        lua_api_set_boolean(state, "on_structure", (item.flags & (1u << ITEM_DATUM_FLAGS_ON_STRUCTURE_BIT)) != 0);
        lua_api_set_boolean(state, "on_object", (item.flags & (1u << ITEM_DATUM_FLAGS_ON_OBJECT_BIT)) != 0);
        lua_api_set_boolean(state, "does_not_accelerate", (item.flags & (1u << ITEM_DATUM_FLAGS_DOES_NOT_ACCELERATE_BIT)) != 0);
        lua_api_set_integer(state, "detonation_ticks", item.detonation_ticks);

        lua_createtable(state, 2, 0);
        for(std::size_t i = 0; i < 2; i++) {
            const auto &trigger = datum.triggers[i];
            lua_createtable(state, 0, 18);
            lua_api_set_integer(state, "state", trigger.state);
            lua_api_set_string(state, "state_name", lua_api_weapon_trigger_state_name(trigger.state));
            lua_api_set_integer(state, "state_timer", trigger.state_timer);
            lua_api_set_boolean(state, "released_since_last_shot", (trigger.flags & (1u << WEAPON_TRIGGER_DATUM_FLAGS_RELEASED_SINCE_LAST_SHOT_BIT)) != 0);
            lua_api_set_boolean(state, "was_down", (trigger.flags & (1u << WEAPON_TRIGGER_DATUM_FLAGS_WAS_DOWN_BIT)) != 0);
            lua_api_set_boolean(state, "toggled", (trigger.flags & (1u << WEAPON_TRIGGER_DATUM_FLAGS_TOGGLED_BIT)) != 0);
            lua_api_set_boolean(state, "blurred", (trigger.flags & (1u << WEAPON_TRIGGER_DATUM_FLAGS_BLURRED_BIT)) != 0);
            lua_api_set_boolean(state, "fired_before_charging", (trigger.flags & (1u << WEAPON_TRIGGER_DATUM_FLAGS_FIRED_BEFORE_CHARGING_BIT)) != 0);
            lua_api_set_integer(state, "firing_effects_used_flags", trigger.firing_effects_used_flags);
            lua_api_set_integer(state, "firing_effect_index", trigger.firing_effect_index);
            lua_api_set_integer(state, "firing_effect_shots_remaining", trigger.firing_effect_shots_remaining);
            lua_api_set_integer(state, "sequential_non_tracer_rounds", trigger.sequential_non_tracer_rounds);
            lua_api_set_number(state, "rate_of_fire", trigger.rate_of_fire);
            lua_api_set_number(state, "ejection_port_position", trigger.ejection_port_position);
            lua_api_set_number(state, "illumination", trigger.illumination);
            lua_api_set_number(state, "error", trigger.error);
            lua_api_set_integer(state, "charging_effect_index", trigger.charging_effect_index);
            lua_api_set_integer(state, "delay_ticks_before_empty_clip_auto_reload", trigger.delay_ticks_before_empty_clip_auto_reload);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "triggers");

        lua_createtable(state, 2, 0);
        for(std::size_t i = 0; i < 2; i++) {
            const auto &magazine = datum.magazines[i];
            lua_createtable(state, 0, 7);
            lua_api_set_integer(state, "state", magazine.state);
            lua_api_set_string(state, "state_name", lua_api_weapon_magazine_state_name(magazine.state));
            lua_api_set_integer(state, "state_timer", magazine.state_timer);
            lua_api_set_integer(state, "original_time", magazine.original_time);
            lua_api_set_integer(state, "rounds_total", magazine.rounds_total);
            lua_api_set_integer(state, "rounds_loaded", magazine.rounds_loaded);
            lua_api_set_integer(state, "rounds_fractional_recharged", magazine.rounds_fractional_recharged);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "magazines");

        return 1;
    }

    static int lua_api_get_map_info(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_map_info");
        }

        lua_createtable(state, 0, 10);

        if(game_engine() == GAME_ENGINE_DEMO) {
            auto &header = get_demo_map_header();
            auto valid = header.is_valid();
            auto name = lua_api_bounded_string(header.name, sizeof(header.name));
            lua_api_set_string(state, "name", name.c_str());
            lua_api_set_boolean(state, "valid", valid);
            lua_api_set_boolean(state, "protected", false);
            if(valid) {
                lua_api_set_integer(state, "game_type", header.game_type);
                lua_api_set_integer(state, "engine_type", header.engine_type);
                lua_api_set_integer(state, "file_size", header.file_size);
                lua_api_set_integer(state, "tag_data_size", header.tag_data_size);
                lua_api_set_integer(state, "crc32", header.crc32);
                auto build = lua_api_bounded_string(header.build, sizeof(header.build));
                lua_api_set_string(state, "build", build.c_str());
            }
        }
        else {
            auto &header = get_map_header();
            auto valid = header.is_valid();
            auto name = lua_api_bounded_string(header.name, sizeof(header.name));
            lua_api_set_string(state, "name", name.c_str());
            lua_api_set_boolean(state, "valid", valid);
            lua_api_set_boolean(state, "protected", valid && map_is_protected());
            if(valid) {
                lua_api_set_integer(state, "game_type", header.game_type);
                lua_api_set_integer(state, "engine_type", header.engine_type);
                lua_api_set_integer(state, "file_size", header.file_size);
                lua_api_set_integer(state, "tag_data_size", header.tag_data_size);
                lua_api_set_integer(state, "crc32", header.crc32);
                auto build = lua_api_bounded_string(header.build, sizeof(header.build));
                lua_api_set_string(state, "build", build.c_str());
            }
        }
        return 1;
    }

    static int lua_api_get_multiplayer_info(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_multiplayer_info");
        }

        auto type = server_type();
        lua_createtable(state, 0, 9);
        lua_api_set_string(state, "server_type", lua_api_server_type_name(type));
        if(type == SERVER_NONE) {
            lua_pushnil(state);
            lua_setfield(state, -2, "gametype");
            lua_pushnil(state);
            lua_setfield(state, -2, "team_game");
            return 1;
        }

        lua_api_set_integer(state, "gametype", gametype());
        lua_api_set_boolean(state, "team_game", is_team());
        auto *info = ServerInfo::get_server_info();
        if(info) {
            lua_api_set_wide_string(state, "server_name", info->server_name, 0x42);
            auto map_name = lua_api_bounded_string(info->map_name, sizeof(info->map_name));
            lua_api_set_string(state, "map_name", map_name.c_str());
            lua_api_set_wide_string(state, "gametype_name", info->gametype, 0x18);
        }
        auto *players = ServerInfoPlayerList::get_server_info_player_list();
        if(players) {
            lua_api_set_integer(state, "max_players", players->max_players);
            lua_api_set_integer(state, "player_count", players->player_count);
        }
        return 1;
    }

    static int lua_api_get_server_players(lua_State *state) noexcept {
        if(lua_gettop(state) != 0) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_server_players");
        }

        auto *players = ServerInfoPlayerList::get_server_info_player_list();
        if(!players) {
            lua_pushnil(state);
            return 1;
        }

        lua_createtable(state, 16, 0);
        int output_index = 1;
        for(std::size_t i = 0; i < 16; i++) {
            const auto &entry = players->players[i];
            if(entry.player_id == 0xFF || entry.machine_index == 0xFF) {
                continue;
            }
            lua_createtable(state, 0, 8);
            lua_api_set_wide_string(state, "name", entry.name, 0xC);
            lua_api_set_integer(state, "armor_color", entry.armor_color);
            lua_api_set_integer(state, "machine_index", entry.machine_index);
            lua_api_set_integer(state, "status", entry.status);
            lua_api_set_integer(state, "team", entry.team);
            lua_api_set_integer(state, "player_index", entry.player_id);
            auto *player = entry.get_player_table_player();
            if(player && player->player_id != 0xFFFF) {
                lua_api_set_id(state, "player_id", player->get_full_id());
            }
            else {
                lua_pushnil(state);
                lua_setfield(state, -2, "player_id");
            }
            lua_rawseti(state, -2, output_index++);
        }
        return 1;
    }
}

#endif
