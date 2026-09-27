

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
        lua_createtable(state, 0, 35);
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
        lua_api_set_point(state, "looking", datum.looking_vector);
        lua_api_set_point(state, "throttle", datum.throttle);
        lua_api_set_number(state, "primary_trigger", datum.primary_trigger);

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
        lua_api_set_number(state, "active_camouflage", datum.active_camouflage);

        const auto &animation = datum.animation;
        lua_createtable(state, 0, 9);
        auto set_optional_byte = [state](const char *key, std::int8_t value) noexcept {
            if(value < 0) {
                lua_pushnil(state);
            }
            else {
                lua_pushinteger(state, static_cast<lua_Integer>(value));
            }
            lua_setfield(state, -2, key);
        };
        auto set_animation_state = [state](const char *key, const AnimationState &animation_state) noexcept {
            lua_createtable(state, 0, 2);
            if(animation_state.index < 0) {
                lua_pushnil(state);
            }
            else {
                lua_pushinteger(state, static_cast<lua_Integer>(animation_state.index));
            }
            lua_setfield(state, -2, "index");
            if(animation_state.frame_index < 0) {
                lua_pushnil(state);
            }
            else {
                lua_pushinteger(state, static_cast<lua_Integer>(animation_state.frame_index));
            }
            lua_setfield(state, -2, "frame_index");
            lua_setfield(state, -2, key);
        };

        set_optional_byte("unit_index", animation.seat_index);
        set_optional_byte("weapon_index", animation.weapon_index);
        set_optional_byte("weapon_type_index", animation.weapon_type_index);
        set_optional_byte("state", animation.state);
        set_optional_byte("action", animation.action);
        set_optional_byte("overlay_action", animation.overlay_action);
        set_optional_byte("desired_state", animation.desired_state);
        set_animation_state("replacement_animation", animation.action_animation);
        set_animation_state("overlay_animation", animation.overlay_action_animation);
        lua_setfield(state, -2, "animation");

        return 1;
    }
}

#endif
