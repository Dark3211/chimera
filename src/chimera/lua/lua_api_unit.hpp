// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_LUA_API_UNIT_HPP
#define CHIMERA_LUA_API_UNIT_HPP

#include "lua_api_common.hpp"

namespace Chimera {
    static int lua_api_get_unit_info(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_unit_info");
        }
        auto *object = lua_api_resolve_object(state, 1);
        if(!object || (object->object.type != OBJECT_TYPE_BIPED && object->object.type != OBJECT_TYPE_VEHICLE)) {
            lua_pushnil(state);
            return 1;
        }

        auto *unit = reinterpret_cast<UnitDynamicObject *>(object);
        const auto &datum = unit->unit;
        lua_createtable(state, 0, 34);
        lua_api_set_id(state, "object_id", object->full_object_id());
        lua_api_set_id(state, "player_id", datum.player_index);
        lua_api_set_boolean(state, "active_camouflaged", (datum.flags & (1u << UNIT_DATUM_FLAGS_ACTIVE_CAMOUFLAGED_BIT)) != 0);
        lua_api_set_boolean(state, "controllable", (datum.flags & (1u << UNIT_DATUM_FLAGS_CONTROLLABLE_BIT)) != 0);
        lua_api_set_boolean(state, "flashlight_on", (datum.flags & (1u << UNIT_DATUM_FLAGS_INTEGRATED_LIGHT_ON_BIT)) != 0);
        lua_api_set_boolean(state, "crouch", (datum.control_flags & (1u << UNIT_CONTROL_FLAGS_CROUCH_MODIFIER_BIT)) != 0);
        lua_api_set_boolean(state, "jump", (datum.control_flags & (1u << UNIT_CONTROL_FLAGS_JUMP_BIT)) != 0);
        lua_api_set_boolean(state, "action", (datum.control_flags & (1u << UNIT_CONTROL_FLAGS_ACTION_BIT)) != 0);
        lua_api_set_boolean(state, "reload", (datum.control_flags & (1u << UNIT_CONTROL_FLAGS_WEAPON_RELOAD_BIT)) != 0);
        lua_api_set_boolean(state, "primary_fire", (datum.control_flags & (1u << UNIT_CONTROL_FLAGS_WEAPON_PRIMARY_TRIGGER_BIT)) != 0);
        lua_api_set_boolean(state, "secondary_fire", (datum.control_flags & (1u << UNIT_CONTROL_FLAGS_WEAPON_SECONDARY_TRIGGER_BIT)) != 0);
        lua_api_set_boolean(state, "grenade", (datum.control_flags & (1u << UNIT_CONTROL_FLAGS_THROW_GRENADE_BIT)) != 0);
        lua_api_set_boolean(state, "swap_weapons", (datum.control_flags & (1u << UNIT_CONTROL_FLAGS_SWAP_WEAPONS_BIT)) != 0);
        lua_api_set_point(state, "desired_facing", datum.desired_facing_vector);
        lua_api_set_point(state, "desired_aiming", datum.desired_aiming_vector);
        lua_api_set_point(state, "aiming", datum.aiming_vector);
        lua_api_set_point(state, "aiming_velocity", datum.aiming_velocity);
        lua_api_set_point(state, "desired_looking", datum.desired_looking_vector);
        lua_api_set_point(state, "looking", datum.looking_vector);
        lua_api_set_point(state, "looking_velocity", datum.looking_velocity);
        lua_api_set_point(state, "throttle", datum.throttle);
        lua_api_set_number(state, "primary_trigger", datum.primary_trigger);
        lua_api_set_integer(state, "aiming_speed", datum.aiming_speed);
        lua_api_set_integer(state, "melee_attack_state", datum.melee_attack_state);
        lua_api_set_integer(state, "last_unit_effect_type", datum.last_unit_effect_type);
        lua_api_set_integer(state, "game_time_at_last_unit_effect", datum.game_time_at_last_unit_effect);
        lua_api_set_integer(state, "grenade_throw_state", datum.grenade_throw_state);
        lua_api_set_integer(state, "grenade_throw_ticks", datum.grenade_throw_ticks);
        lua_api_set_integer(state, "grenade_throw_full_power_ticks", datum.grenade_throw_full_power_ticks);
        lua_api_set_number(state, "ambient_illumination", datum.ambient_illumination);
        lua_api_set_number(state, "self_illumination", datum.self_illumination);
        lua_api_set_number(state, "mouth_aperture", datum.mouth_aperture);

        if(datum.parent_seat_index == 0xFFFF) {
            lua_pushnil(state);
        }
        else {
            lua_pushinteger(state, datum.parent_seat_index);
        }
        lua_setfield(state, -2, "seat_index");

        if(datum.current_weapon_index >= MAXIMUM_WEAPONS_PER_UNIT) {
            lua_pushnil(state);
        }
        else {
            lua_pushinteger(state, datum.current_weapon_index);
        }
        lua_setfield(state, -2, "current_weapon_index");

        if(datum.desired_weapon_index >= MAXIMUM_WEAPONS_PER_UNIT) {
            lua_pushnil(state);
        }
        else {
            lua_pushinteger(state, datum.desired_weapon_index);
        }
        lua_setfield(state, -2, "desired_weapon_index");

        lua_createtable(state, MAXIMUM_WEAPONS_PER_UNIT, 0);
        for(std::size_t i = 0; i < MAXIMUM_WEAPONS_PER_UNIT; i++) {
            if(datum.weapon_object_indices[i].is_null()) {
                lua_pushnil(state);
            }
            else {
                lua_pushinteger(state, static_cast<lua_Integer>(datum.weapon_object_indices[i].whole_id));
            }
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "weapon_ids");

        lua_createtable(state, MAXIMUM_WEAPONS_PER_UNIT, 0);
        for(std::size_t i = 0; i < MAXIMUM_WEAPONS_PER_UNIT; i++) {
            lua_pushinteger(state, datum.weapon_last_used_at_game_time[i]);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "weapon_last_used_at");

        lua_api_set_id(state, "equipment_object_id", datum.equipment_object_index);
        lua_api_set_id(state, "grenade_object_id", datum.grenade_object_index);
        lua_api_set_integer(state, "current_grenade_type", datum.current_grenade_index);
        lua_api_set_integer(state, "desired_grenade_type", datum.desired_grenade_index);
        lua_api_set_integer(state, "frag_grenades", datum.grenade_counts[UNIT_GRENADE_TYPE_HUMAN_FRAGMENTATION]);
        lua_api_set_integer(state, "plasma_grenades", datum.grenade_counts[UNIT_GRENADE_TYPE_COVENANT_PLASMA]);

        if(datum.current_zoom_level == 0xFF) {
            lua_pushnil(state);
        }
        else {
            lua_pushinteger(state, datum.current_zoom_level);
        }
        lua_setfield(state, -2, "zoom_level");

        if(datum.desired_zoom_level == 0xFF) {
            lua_pushnil(state);
        }
        else {
            lua_pushinteger(state, datum.desired_zoom_level);
        }
        lua_setfield(state, -2, "desired_zoom_level");

        if(object->object.type == OBJECT_TYPE_VEHICLE) {
            lua_api_set_id(state, "driver_object_id", datum.driver_object_index);
            lua_api_set_id(state, "gunner_object_id", datum.gunner_object_index);
        }
        lua_api_set_id(state, "last_vehicle_id", datum.last_vehicle_index);
        lua_api_set_number(state, "integrated_light_power", datum.integrated_light_power);
        lua_api_set_number(state, "integrated_light_battery", datum.integrated_light_battery);
        lua_api_set_number(state, "integrated_night_vision_power", datum.integrated_night_vision_power);
        lua_api_set_number(state, "active_camouflage", datum.active_camouflage);
        lua_api_set_number(state, "active_camouflage_super_amount", datum.active_camouflage_super_amount);
        lua_api_set_integer(state, "body_stun_ticks", datum.body_stun_ticks);
        lua_api_set_number(state, "body_stun", datum.body_stun);
        lua_api_set_integer(state, "time_of_death", datum.time_of_death);

        lua_createtable(state, NUMBER_OF_UNIT_POWERED_SEATS, 0);
        for(std::size_t i = 0; i < NUMBER_OF_UNIT_POWERED_SEATS; i++) {
            lua_pushnumber(state, datum.seat_power[i]);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "seat_power");

        lua_createtable(state, 0, 4);
        lua_api_set_point(state, "last_position", datum.seat_last_position);
        lua_api_set_point(state, "last_velocity", datum.seat_last_velocity);
        lua_api_set_point(state, "acceleration", datum.seat_acceleration);
        lua_api_set_point(state, "desired_acceleration", datum.seat_desired_acceleration);
        lua_setfield(state, -2, "seat_motion");

        lua_createtable(state, 0, 16);
        lua_api_set_integer(state, "flags", datum.animation.flags);
        lua_api_set_integer(state, "aiming_screen_index", datum.animation.aiming_screen_index);
        lua_api_set_integer(state, "looking_screen_index", datum.animation.looking_screen_index);
        lua_api_set_integer(state, "last_ping_animation_index", datum.animation.last_ping_animation_index);
        lua_api_set_integer(state, "seat_index", datum.animation.seat_index);
        lua_api_set_integer(state, "weapon_index", datum.animation.weapon_index);
        lua_api_set_integer(state, "weapon_type_index", datum.animation.weapon_type_index);
        lua_api_set_integer(state, "state", datum.animation.state);
        lua_api_set_integer(state, "action", datum.animation.action);
        lua_api_set_integer(state, "overlay_action", datum.animation.overlay_action);
        lua_api_set_integer(state, "desired_state", datum.animation.desired_state);
        lua_api_set_integer(state, "base_seat_index", datum.animation.base_seat_index);
        lua_api_set_integer(state, "emotion_index", datum.animation.emotion_index);
        lua_api_set_boolean(state, "aiming_with_euler_screen", datum.animation.aiming_with_euler_screen);
        lua_api_set_boolean(state, "looking_with_euler_screen", datum.animation.looking_with_euler_screen);
        lua_api_set_integer(state, "external_animation_graph_index", datum.animation.external_animation_graph_index);
        lua_setfield(state, -2, "animation");

        lua_createtable(state, 0, 5);
        lua_api_set_boolean(state, "force_local_update", datum.force_local_update);
        lua_api_set_boolean(state, "from_network_data_valid", datum.is_from_network_data_valid);
        lua_api_set_boolean(state, "did_just_complete_client_update", datum.did_just_complete_client_update);
        lua_api_set_integer(state, "last_completed_client_update_id", datum.last_completed_client_update_id);
        lua_api_set_point(state, "position_after_last_client_update", datum.position_after_completing_last_client_update);
        lua_setfield(state, -2, "network");

        return 1;
    }
}

#endif
