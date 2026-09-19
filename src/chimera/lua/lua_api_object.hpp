// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_LUA_API_OBJECT_HPP
#define CHIMERA_LUA_API_OBJECT_HPP

#include "lua_api_common.hpp"

namespace Chimera {
    static int lua_api_get_object_info(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_object_info");
        }
        auto *object = lua_api_resolve_object(state, 1);
        if(!object) {
            lua_pushnil(state);
            return 1;
        }

        auto id = object->full_object_id();
        const auto &datum = object->object;
        lua_createtable(state, 0, 28);
        lua_api_set_id(state, "id", id);
        auto *definition = get_tag(object->definition_index);
        if(definition) {
            lua_api_set_id(state, "tag_id", definition->id);
        }
        else {
            lua_pushnil(state);
            lua_setfield(state, -2, "tag_id");
        }
        lua_api_set_integer(state, "type", datum.type);
        lua_api_set_string(state, "type_name", lua_api_object_type_name(datum.type));
        lua_api_set_point(state, "position", datum.position);
        lua_api_set_point(state, "velocity", datum.translational_velocity);
        lua_api_set_point(state, "forward", datum.forward);
        lua_api_set_point(state, "up", datum.up);
        lua_api_set_point(state, "angular_velocity", datum.angular_velocity);
        lua_api_set_point(state, "bounding_center", datum.bounding_sphere_center);
        lua_api_set_number(state, "radius", datum.bounding_sphere_radius);
        lua_api_set_number(state, "scale", datum.scale);
        lua_api_set_integer(state, "owner_team_index", datum.owner_team_index);
        if(datum.owner_player_index < 0) {
            lua_pushnil(state);
        }
        else {
            lua_pushinteger(state, datum.owner_player_index);
        }
        lua_setfield(state, -2, "owner_player_index");
        lua_api_set_id(state, "parent_object_id", datum.parent_object_index);
        lua_api_set_id(state, "child_object_id", datum.first_child_object_index);
        lua_api_set_number(state, "maximum_health", datum.maximum_body_vitality);
        lua_api_set_number(state, "maximum_shields", datum.maximum_shield_vitality);
        lua_api_set_number(state, "health", datum.body_vitality);
        lua_api_set_number(state, "shields", datum.shield_vitality);
        lua_api_set_integer(state, "shield_stun_ticks", datum.shield_stun_ticks);
        lua_api_set_boolean(state, "on_ground", (datum.flags & (1u << OBJECT_DATA_FLAGS_ON_GROUND_BIT)) != 0);
        lua_api_set_boolean(state, "outside_map", (datum.flags & (1u << OBJECT_DATA_FLAGS_OUTSIDE_OF_MAP_BIT)) != 0);
        lua_api_set_boolean(state, "no_collisions", (datum.flags & (1u << OBJECT_DATA_FLAGS_NO_COLLISIONS_BIT)) != 0);
        lua_api_set_boolean(state, "cannot_take_damage", (datum.damage_flags & (1u << OBJECT_DAMAGE_FLAGS_CANNOT_TAKE_DAMAGE_BIT)) != 0);
        lua_api_set_boolean(state, "dead", (datum.damage_flags & (1u << OBJECT_DAMAGE_FLAGS_DEAD_BIT)) != 0);
        lua_api_set_boolean(state, "shield_depleted", (datum.damage_flags & (1u << OBJECT_DAMAGE_FLAGS_SHIELD_DEPLETED_BIT)) != 0);
        lua_api_set_boolean(state, "shield_charging", (datum.damage_flags & (1u << OBJECT_DAMAGE_FLAGS_SHIELD_CHARGING_BIT)) != 0);

        lua_api_set_integer(state, "datum_role", datum.datum_role);
        lua_api_set_string(state, "datum_role_name", lua_api_network_role_name(datum.datum_role));
        lua_api_set_boolean(state, "network_at_rest", datum.network_at_rest);
        lua_api_set_boolean(state, "was_network_at_rest", datum.was_network_at_rest);
        lua_api_set_integer(state, "last_incremental_send_time", datum.last_incremental_send_time);
        lua_api_set_boolean(state, "has_been_updated_from_network", (datum.flags & (1u << OBJECT_DATA_FLAGS_HAS_BEEN_UPDATED_FROM_NETWORK_BIT)) != 0);
        lua_api_set_integer(state, "render_flags", datum.render_flags);
        lua_api_set_integer(state, "idle_ticks", datum.idle_ticks);
        lua_api_set_integer(state, "variant_number", datum.variant_number);
        lua_api_set_integer(state, "parent_node_index", datum.parent_node_index);

        lua_createtable(state, 0, 10);
        lua_api_set_boolean(state, "server_position_valid", datum.is_server_position_valid);
        lua_api_set_point(state, "last_server_position", datum.last_server_position);
        lua_api_set_boolean(state, "server_orientation_valid", datum.is_server_orientation_valid);
        lua_api_set_point(state, "last_server_forward", datum.last_server_forward);
        lua_api_set_point(state, "last_server_up", datum.last_server_up);
        lua_api_set_boolean(state, "server_velocity_valid", datum.is_server_translational_velocity_valid);
        lua_api_set_point(state, "last_server_velocity", datum.last_server_translational_velocity);
        lua_api_set_boolean(state, "update_timestamp_valid", datum.is_update_timestamp_valid);
        lua_api_set_integer(state, "last_update_timestamp", datum.last_update_timestamp);
        lua_setfield(state, -2, "network");

        lua_createtable(state, 0, 5);
        lua_api_set_id(state, "tag_id", datum.animation.animation_tag_id);
        lua_api_set_integer(state, "index", datum.animation.state.index);
        lua_api_set_integer(state, "frame_index", datum.animation.state.frame_index);
        lua_api_set_integer(state, "interpolation_frame_index", datum.animation.interpolation_frame_index);
        lua_api_set_integer(state, "interpolation_frame_count", datum.animation.interpolation_frame_count);
        lua_setfield(state, -2, "animation");

        lua_createtable(state, 0, 14);
        lua_api_set_boolean(state, "invisible", (datum.flags & (1u << OBJECT_DATA_FLAGS_INVISIBLE_BIT)) != 0);
        lua_api_set_boolean(state, "on_media", (datum.flags & (1u << OBJECT_DATA_FLAGS_ON_MEDIA_BIT)) != 0);
        lua_api_set_boolean(state, "partially_under_media", (datum.flags & (1u << OBJECT_DATA_FLAGS_PARTIALLY_UNDER_MEDIA_BIT)) != 0);
        lua_api_set_boolean(state, "wholly_under_media", (datum.flags & (1u << OBJECT_DATA_FLAGS_WHOLLY_UNDER_MEDIA_BIT)) != 0);
        lua_api_set_boolean(state, "at_rest", (datum.flags & (1u << OBJECT_DATA_FLAGS_AT_REST_BIT)) != 0);
        lua_api_set_boolean(state, "animates_automatically", (datum.flags & (1u << OBJECT_DATA_FLAGS_ANIMATES_AUTOMATICALLY_BIT)) != 0);
        lua_api_set_boolean(state, "has_attached_lights", (datum.flags & (1u << OBJECT_DATA_FLAGS_HAS_ATTACHED_LIGHTS_BIT)) != 0);
        lua_api_set_boolean(state, "has_attached_shader", (datum.flags & (1u << OBJECT_DATA_FLAGS_HAS_ATTACHED_SHADER_BIT)) != 0);
        lua_api_set_boolean(state, "has_attached_looping_sounds", (datum.flags & (1u << OBJECT_DATA_FLAGS_HAS_ATTACHED_LOOPING_SOUNDS_BIT)) != 0);
        lua_api_set_boolean(state, "connected_to_map", (datum.flags & (1u << OBJECT_DATA_FLAGS_CONNECTED_TO_MAP_BIT)) != 0);
        lua_api_set_boolean(state, "mirrored", (datum.flags & (1u << OBJECT_DATA_FLAGS_MIRRORED_BIT)) != 0);
        lua_api_set_boolean(state, "garbage", (datum.flags & (1u << OBJECT_DATA_FLAGS_GARBAGE_BIT)) != 0);
        lua_api_set_boolean(state, "cannot_be_garbage", (datum.flags & (1u << OBJECT_DATA_FLAGS_CANNOT_BE_GARBAGE_BIT)) != 0);
        lua_api_set_boolean(state, "shadowless", (datum.flags & (1u << OBJECT_DATA_FLAGS_SHADOWLESS_BIT)) != 0);
        lua_setfield(state, -2, "flags");

        lua_createtable(state, 0, 3);
        lua_api_set_integer(state, "leaf_index", datum.location.leaf_index);
        lua_api_set_integer(state, "cluster_index", datum.location.cluster_index);
        lua_api_set_integer(state, "bonus", datum.location.bonus);
        lua_setfield(state, -2, "location");

        lua_createtable(state, NUMBER_OF_INCOMING_OBJECT_FUNCTIONS, 0);
        for(std::size_t i = 0; i < NUMBER_OF_INCOMING_OBJECT_FUNCTIONS; i++) {
            lua_pushnumber(state, datum.incoming_function_values[i]);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "incoming_functions");

        lua_createtable(state, NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS, 0);
        for(std::size_t i = 0; i < NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS; i++) {
            lua_pushnumber(state, datum.outgoing_function_values[i]);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "outgoing_functions");
        lua_api_set_integer(state, "functions_active_flags", datum.functions_active_flags);

        lua_createtable(state, MAXIMUM_REGIONS_PER_OBJECT, 0);
        for(std::size_t i = 0; i < MAXIMUM_REGIONS_PER_OBJECT; i++) {
            lua_createtable(state, 0, 3);
            lua_api_set_boolean(state, "destroyed", (datum.regions_destroyed_flags & (1u << i)) != 0);
            lua_api_set_integer(state, "damage", datum.region_damage[i]);
            lua_api_set_integer(state, "permutation", datum.region_permutations[i]);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "regions");

        lua_createtable(state, NUMBER_OF_OBJECT_CHANGE_COLORS, 0);
        for(std::size_t i = 0; i < NUMBER_OF_OBJECT_CHANGE_COLORS; i++) {
            lua_api_push_color(state, datum.base_change_colors[i]);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "base_change_colors");

        lua_createtable(state, NUMBER_OF_OBJECT_CHANGE_COLORS, 0);
        for(std::size_t i = 0; i < NUMBER_OF_OBJECT_CHANGE_COLORS; i++) {
            lua_api_push_color(state, datum.outgoing_change_colors[i]);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "outgoing_change_colors");

        return 1;
    }
}

#endif
