

#ifndef CHIMERA_LUA_API_OBJECT_HPP
#define CHIMERA_LUA_API_OBJECT_HPP

#include "lua_api_common.hpp"

namespace Chimera {
    static bool lua_api_get_object_node_count_value(BaseDynamicObject *object, std::size_t &count) noexcept {
        if(!object || !object->nodes()) {
            return false;
        }

        const auto block_size = static_cast<std::size_t>(object->object.node_matrices.size);
        if(block_size == 0 || block_size % sizeof(ModelNode) != 0) {
            return false;
        }

        count = block_size / sizeof(ModelNode);
        return count > 0 && count <= MAX_NODES;
    }

    static int lua_api_get_object_node_count(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_object_node_count");
        }

        auto *object = lua_api_resolve_object(state, 1);
        std::size_t count = 0;
        if(!lua_api_get_object_node_count_value(object, count)) {
            lua_pushnil(state);
            return 1;
        }

        lua_pushinteger(state, static_cast<lua_Integer>(count));
        return 1;
    }

    static bool lua_api_model_node_is_finite(const ModelNode &node) noexcept {
        if(!std::isfinite(node.scale)
        || !std::isfinite(node.position.x)
        || !std::isfinite(node.position.y)
        || !std::isfinite(node.position.z)) {
            return false;
        }

        for(std::size_t i = 0; i < 3; i++) {
            if(!std::isfinite(node.rotation.v[i].x)
            || !std::isfinite(node.rotation.v[i].y)
            || !std::isfinite(node.rotation.v[i].z)) {
                return false;
            }
        }
        return true;
    }

    static int lua_api_get_object_node_info(lua_State *state) noexcept {
        if(lua_gettop(state) != 2) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_object_node_info");
        }

        auto *object = lua_api_resolve_object(state, 1);
        if(!object || !lua_isnumber(state, 2)) {
            lua_pushnil(state);
            return 1;
        }

        const auto index_number = lua_tonumber(state, 2);
        if(!std::isfinite(index_number) || index_number < 0.0 || std::floor(index_number) != index_number) {
            lua_pushnil(state);
            return 1;
        }

        std::size_t count = 0;
        if(!lua_api_get_object_node_count_value(object, count)
        || index_number >= static_cast<lua_Number>(count)) {
            lua_pushnil(state);
            return 1;
        }

        const auto index = static_cast<std::size_t>(index_number);
        const auto *nodes = object->nodes();
        const auto &node = nodes[index];
        if(!lua_api_model_node_is_finite(node)) {
            lua_pushnil(state);
            return 1;
        }

        lua_createtable(state, 0, 4);
        lua_api_set_integer(state, "index", static_cast<lua_Integer>(index));
        lua_api_set_number(state, "scale", node.scale);
        lua_api_set_point(state, "position", node.position);

        lua_createtable(state, 3, 0);
        for(std::size_t i = 0; i < 3; i++) {
            lua_createtable(state, 0, 3);
            lua_api_set_number(state, "x", node.rotation.v[i].x);
            lua_api_set_number(state, "y", node.rotation.v[i].y);
            lua_api_set_number(state, "z", node.rotation.v[i].z);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        lua_setfield(state, -2, "rotation");

        return 1;
    }

    struct LuaApiObjectTagBase {
        PAD(0x28);
        TagReference model;
    };
    static_assert(sizeof(LuaApiObjectTagBase) == 0x38);

    struct LuaApiGbxModelBase {
        PAD(0xB8);
        TagBlock nodes;
    };
    static_assert(sizeof(LuaApiGbxModelBase) == 0xC4);

    struct LuaApiModelNodeDefinition {
        char name[32];
        std::int16_t next_sibling_node_index;
        std::int16_t first_child_node_index;
        std::int16_t parent_node_index;
        std::uint16_t pad;
        Point3D default_translation;
        float default_rotation[4];
        float node_distance_from_parent;
        PAD(0x54);
    };
    static_assert(sizeof(LuaApiModelNodeDefinition) == 0x9C);

    static bool lua_api_tag_data_range_is_valid(const void *address, std::size_t size) noexcept {
        static constexpr std::uintptr_t TAG_DATA_SAFE_REGION_SIZE = 0x1700000;
        if(!address) {
            return false;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(get_tag_data_address());
        if(base > std::numeric_limits<std::uintptr_t>::max() - TAG_DATA_SAFE_REGION_SIZE) {
            return false;
        }
        const auto end = base + TAG_DATA_SAFE_REGION_SIZE;
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        if(start < base || start >= end) {
            return false;
        }
        return size <= end - start;
    }

    static bool lua_api_get_object_node_definition_value(BaseDynamicObject *object,
                                                         std::size_t index,
                                                         const LuaApiModelNodeDefinition *&node,
                                                         TagID &model_tag_id,
                                                         std::size_t &definition_count) noexcept {
        std::size_t runtime_count = 0;
        if(!lua_api_get_object_node_count_value(object, runtime_count) || index >= runtime_count) {
            return false;
        }

        auto *object_definition = get_tag(object->definition_index);
        if(!object_definition
        || object_definition->id.index.index != object->definition_index.index.index
        || !lua_api_tag_data_range_is_valid(object_definition->data, sizeof(LuaApiObjectTagBase))) {
            return false;
        }

        const auto *object_data = reinterpret_cast<const LuaApiObjectTagBase *>(object_definition->data);
        model_tag_id = object_data->model.tag_id;
        auto *model_definition = get_tag(model_tag_id);
        if(!model_definition
        || model_definition->id.whole_id != model_tag_id.whole_id
        || (model_definition->primary_class != TagClassInt::TAG_CLASS_GBXMODEL
            && model_definition->primary_class != TagClassInt::TAG_CLASS_MODEL)
        || !lua_api_tag_data_range_is_valid(model_definition->data, sizeof(LuaApiGbxModelBase))) {
            return false;
        }

        const auto *model_data = reinterpret_cast<const LuaApiGbxModelBase *>(model_definition->data);
        const auto node_count = static_cast<std::size_t>(model_data->nodes.count);
        if(node_count == 0 || node_count > 255 || node_count < runtime_count || index >= node_count) {
            return false;
        }
        if(node_count > std::numeric_limits<std::size_t>::max() / sizeof(LuaApiModelNodeDefinition)) {
            return false;
        }

        const auto bytes = node_count * sizeof(LuaApiModelNodeDefinition);
        if(!lua_api_tag_data_range_is_valid(model_data->nodes.address, bytes)) {
            return false;
        }

        const auto *nodes = reinterpret_cast<const LuaApiModelNodeDefinition *>(model_data->nodes.address);
        const auto &candidate = nodes[index];
        auto valid_relation = [node_count](std::int16_t value) noexcept {
            return value == -1 || (value >= 0 && static_cast<std::size_t>(value) < node_count);
        };
        if(!valid_relation(candidate.next_sibling_node_index)
        || !valid_relation(candidate.first_child_node_index)
        || !valid_relation(candidate.parent_node_index)
        || !std::isfinite(candidate.default_translation.x)
        || !std::isfinite(candidate.default_translation.y)
        || !std::isfinite(candidate.default_translation.z)
        || !std::isfinite(candidate.default_rotation[0])
        || !std::isfinite(candidate.default_rotation[1])
        || !std::isfinite(candidate.default_rotation[2])
        || !std::isfinite(candidate.default_rotation[3])
        || !std::isfinite(candidate.node_distance_from_parent)) {
            return false;
        }

        node = &candidate;
        definition_count = node_count;
        return true;
    }

    static int lua_api_get_object_node_definition(lua_State *state) noexcept {
        if(lua_gettop(state) != 2) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "get_object_node_definition");
        }

        auto *object = lua_api_resolve_object(state, 1);
        if(!object || !lua_isnumber(state, 2)) {
            lua_pushnil(state);
            return 1;
        }

        const auto index_number = lua_tonumber(state, 2);
        if(!std::isfinite(index_number) || index_number < 0.0 || std::floor(index_number) != index_number) {
            lua_pushnil(state);
            return 1;
        }

        const auto index = static_cast<std::size_t>(index_number);
        const LuaApiModelNodeDefinition *node = nullptr;
        TagID model_tag_id = HaloID::null_id();
        std::size_t definition_count = 0;
        if(!lua_api_get_object_node_definition_value(object, index, node, model_tag_id, definition_count)) {
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

        lua_createtable(state, 0, 9);
        lua_api_set_integer(state, "index", static_cast<lua_Integer>(index));
        lua_api_set_id(state, "model_tag_id", model_tag_id);
        auto name = lua_api_bounded_string(node->name, sizeof(node->name));
        lua_api_set_string(state, "name", name.c_str());
        set_optional_index("next_sibling_index", node->next_sibling_node_index);
        set_optional_index("first_child_index", node->first_child_node_index);
        set_optional_index("parent_index", node->parent_node_index);
        lua_api_set_point(state, "default_translation", node->default_translation);

        lua_createtable(state, 0, 4);
        lua_api_set_number(state, "i", node->default_rotation[0]);
        lua_api_set_number(state, "j", node->default_rotation[1]);
        lua_api_set_number(state, "k", node->default_rotation[2]);
        lua_api_set_number(state, "w", node->default_rotation[3]);
        lua_setfield(state, -2, "default_rotation");

        lua_api_set_number(state, "node_distance_from_parent", node->node_distance_from_parent);
        return 1;
    }

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
        lua_createtable(state, 0, 31);
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

        ObjectID owner_object_id;
        owner_object_id.whole_id = static_cast<std::uint32_t>(datum.owner_object_index);
        if(!owner_object_id.is_null() && ObjectTable::get_object_table().get_dynamic_object(owner_object_id)) {
            lua_api_set_id(state, "owner_object_id", owner_object_id);
        }
        else {
            lua_pushnil(state);
            lua_setfield(state, -2, "owner_object_id");
        }

        lua_api_set_id(state, "parent_object_id", datum.parent_object_index);
        if(datum.parent_node_index >= 0 && !datum.parent_object_index.is_null()) {
            auto *parent = ObjectTable::get_object_table().get_dynamic_object(datum.parent_object_index);
            std::size_t parent_node_count = 0;
            const auto parent_node_index = static_cast<std::size_t>(datum.parent_node_index);
            if(parent
            && lua_api_get_object_node_count_value(parent, parent_node_count)
            && parent_node_index < parent_node_count) {
                lua_pushinteger(state, static_cast<lua_Integer>(parent_node_index));
            }
            else {
                lua_pushnil(state);
            }
        }
        else {
            lua_pushnil(state);
        }
        lua_setfield(state, -2, "parent_node_index");

        lua_api_set_id(state, "child_object_id", datum.first_child_object_index);

        const auto &animation = datum.animation;
        lua_createtable(state, 0, 5);
        auto *animation_tag = get_tag(animation.animation_tag_id);
        if(animation_tag && animation_tag->id.whole_id == animation.animation_tag_id.whole_id) {
            lua_api_set_id(state, "tag_id", animation.animation_tag_id);
        }
        else {
            lua_pushnil(state);
            lua_setfield(state, -2, "tag_id");
        }
        auto set_optional_animation_value = [state](const char *key, std::int16_t value) noexcept {
            if(value < 0) {
                lua_pushnil(state);
            }
            else {
                lua_pushinteger(state, static_cast<lua_Integer>(value));
            }
            lua_setfield(state, -2, key);
        };
        set_optional_animation_value("index", animation.state.index);
        set_optional_animation_value("frame_index", animation.state.frame_index);
        set_optional_animation_value("interpolation_frame_index", animation.interpolation_frame_index);
        set_optional_animation_value("interpolation_frame_count", animation.interpolation_frame_count);
        lua_setfield(state, -2, "animation");

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
        return 1;
    }
}

#endif
