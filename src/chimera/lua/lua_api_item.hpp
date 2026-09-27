

#ifndef CHIMERA_LUA_API_ITEM_HPP
#define CHIMERA_LUA_API_ITEM_HPP

#include "lua_api_common.hpp"

namespace Chimera {
    static bool lua_api_item_value_is_finite(const ItemDatum &item) noexcept {
        return std::isfinite(item.item_rest_object_offset.x)
            && std::isfinite(item.item_rest_object_offset.y)
            && std::isfinite(item.item_rest_object_offset.z)
            && std::isfinite(item.rotation_axis.x)
            && std::isfinite(item.rotation_axis.y)
            && std::isfinite(item.rotation_axis.z)
            && std::isfinite(item.rotation_sine)
            && std::isfinite(item.rotation_cosine);
    }

    static int lua_api_get_item_info(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_item_info");
        }

        auto *object = lua_api_resolve_object(state, 1);
        if(!object
        || (object->object.type != OBJECT_TYPE_WEAPON
            && object->object.type != OBJECT_TYPE_EQUIPMENT
            && object->object.type != OBJECT_TYPE_GARBAGE)) {
            lua_pushnil(state);
            return 1;
        }

        auto *item_object = reinterpret_cast<ItemDynamicObject *>(object);
        const auto &item = item_object->item;
        if(!lua_api_item_value_is_finite(item)) {
            lua_pushnil(state);
            return 1;
        }

        auto set_optional_index = [state](const char *key, std::int16_t value) noexcept {
            if(value < 0) {
                lua_pushnil(state);
            }
            else {
                lua_pushinteger(state, static_cast<lua_Integer>(value));
            }
            lua_setfield(state, -2, key);
        };

        auto set_object_id = [state](const char *key, std::int32_t value) noexcept {
            ObjectID id;
            id.whole_id = static_cast<std::uint32_t>(value);
            lua_api_set_id(state, key, id);
        };

        lua_createtable(state, 0, 18);
        lua_api_set_id(state, "object_id", object->full_object_id());
        lua_api_set_integer(state, "type", object->object.type);
        lua_api_set_string(state, "type_name", lua_api_object_type_name(object->object.type));

        lua_api_set_boolean(state, "in_unit_inventory",
            (item.flags & (1u << ITEM_DATUM_FLAGS_IN_UNIT_INVENTORY_BIT)) != 0);
        lua_api_set_boolean(state, "belongs_to_player",
            (item.flags & (1u << ITEM_DATUM_FLAGS_BELONGS_TO_PLAYER_BIT)) != 0);
        lua_api_set_boolean(state, "has_nonzero_angular_velocity",
            (item.flags & (1u << ITEM_DATUM_FLAGS_HAS_NONZERO_ANGULAR_VELOCITY_BIT)) != 0);
        lua_api_set_boolean(state, "on_structure",
            (item.flags & (1u << ITEM_DATUM_FLAGS_ON_STRUCTURE_BIT)) != 0);
        lua_api_set_boolean(state, "on_object",
            (item.flags & (1u << ITEM_DATUM_FLAGS_ON_OBJECT_BIT)) != 0);
        lua_api_set_boolean(state, "does_not_accelerate",
            (item.flags & (1u << ITEM_DATUM_FLAGS_DOES_NOT_ACCELERATE_BIT)) != 0);

        lua_api_set_integer(state, "detonation_ticks", item.detonation_ticks);
        set_optional_index("rested_surface_index", item.rested_surface_index);
        set_optional_index("bsp_index", item.bsp_index);
        set_object_id("ignore_object_id", item.ignore_object_index);
        lua_api_set_integer(state, "last_owned_time", item.last_owned_time);
        set_object_id("rest_object_id", item.item_on_rest_object_index);

        lua_api_set_point(state, "rest_offset", item.item_rest_object_offset);
        lua_api_set_point(state, "rotation_axis", item.rotation_axis);
        lua_api_set_number(state, "rotation_sine", item.rotation_sine);
        lua_api_set_number(state, "rotation_cosine", item.rotation_cosine);

        return 1;
    }
}

#endif
