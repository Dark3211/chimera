// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include "../event/camera.hpp"
#include "../event/command.hpp"
#include "../event/connect.hpp"
#include "../event/damage.hpp"
#include "../event/frame.hpp"
#include "../event/interface_render.hpp"
#include "../event/map_load.hpp"
#include "../event/multiplayer_event.hpp"
#include "../event/rcon_message.hpp"
#include "../event/revert.hpp"
#include "../event/tick.hpp"
#include "../halo_data/camera.hpp"
#include "../halo_data/object.hpp"
#include "../halo_data/player.hpp"
#include "../halo_data/tag.hpp"
#include "../localization/localization.hpp"
#include "../math_trig/math_trig.hpp"
#include "../output/output.hpp"
#include "../chimera.hpp"
#include "lua_variables.hpp"
#include "lua_callback.hpp"

namespace Chimera {
    static LARGE_INTEGER last_time;

    extern std::vector<std::unique_ptr<LuaScript>> scripts;
    extern void load_map_script() noexcept;

    #define call_all_priorities(function) \
        function(EVENT_PRIORITY_BEFORE); \
        function(EVENT_PRIORITY_DEFAULT); \
        function(EVENT_PRIORITY_AFTER); \
        function(EVENT_PRIORITY_FINAL) \

    static int pcall(lua_State *state, int args, int result_count) noexcept {
        auto result = lua_pcall(state, args, result_count, 0);
        if(result != LUA_OK) {
            print_error(state);
        }
        return result;
    }

    template<typename Function>
    static void for_each_lua_callback(LuaScriptCallback &callback,
                                      EventPriority priority,
                                      Function &&function) noexcept {
        if(callback.callback_function != "" && callback.priority == priority) {
            function(callback.callback_function.c_str());
        }

        const auto listener_count = callback.listeners.size();
        for(std::size_t i = 0; i < listener_count; i++) {
            if(i >= callback.listeners.size()) {
                break;
            }
            const auto listener_priority = callback.listeners[i].priority;
            const auto listener_function = callback.listeners[i].callback_function;
            if(listener_function != "" && listener_priority == priority) {
                function(listener_function.c_str());
            }
        }
    }

    #define basic_callback(callback) [](EventPriority priority) noexcept { \
        for(std::size_t i = 0; i < scripts.size(); i++) { \
            auto &script = *scripts[i].get(); \
            auto &script_callback = script.callback; \
            auto *&state = script.state; \
            for_each_lua_callback(script_callback, priority, [&](const char *function_name) noexcept { \
                lua_getglobal(state, function_name); \
                pcall(state, 0, 0); \
            }); \
        } \
    };

    static bool lua_to_uint32(lua_State *state, int index, std::uint32_t &value) noexcept {
        if(!lua_isnumber(state, index)) {
            return false;
        }
        auto number = lua_tonumber(state, index);
        if(!std::isfinite(number) || number < 0.0 || number > static_cast<lua_Number>(std::numeric_limits<std::uint32_t>::max()) || std::floor(number) != number) {
            return false;
        }
        value = static_cast<std::uint32_t>(number);
        return true;
    }

    static bool lua_to_uint16(lua_State *state, int index, std::uint16_t &value) noexcept {
        std::uint32_t value32;
        if(!lua_to_uint32(state, index, value32) || value32 > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        value = static_cast<std::uint16_t>(value32);
        return true;
    }

    static bool lua_to_float(lua_State *state, int index, float &value) noexcept {
        if(!lua_isnumber(state, index)) {
            return false;
        }
        auto number = lua_tonumber(state, index);
        if(!std::isfinite(number) || number < -std::numeric_limits<float>::max() || number > std::numeric_limits<float>::max()) {
            return false;
        }
        value = static_cast<float>(number);
        return true;
    }

    static void check_timers() noexcept {
        LARGE_INTEGER now_time;
        QueryPerformanceCounter(&now_time);
        auto r = counter_time_elapsed(last_time, now_time);
        last_time = now_time;
        for(std::size_t i = 0; i < scripts.size(); i++) {
            auto &script = *scripts[i];
            auto timers = script.timers;
            for(std::size_t t = 0; t < timers.size(); t++) {
                auto &timer = timers[t];
                timer.time_passed += r * 1000;
                bool deleted = false;
                while(timer.time_passed >= timer.interval_ms) {
                    deleted = true;
                    for(std::size_t to = 0; to < script.timers.size(); to++) {
                        auto &timer_to = script.timers[to];
                        if(timer.timer_id == timer_to.timer_id) {
                            deleted = false;
                            break;
                        }
                    }
                    if(deleted) {
                        break;
                    }
                    lua_getglobal(script.state, timer.function.data());
                    for(std::size_t arg = 0; arg < timer.arguments.size(); arg++) {
                        timer.arguments[arg].push_argument(script);
                    }
                    if(pcall(script.state, timer.arguments.size(), 1) == LUA_OK) {
                        if(lua_isboolean(script.state, -1) && !lua_toboolean(script.state, -1)) {
                            deleted = true;
                            lua_pop(script.state, 1);
                            break;
                        }
                        lua_pop(script.state, 1);
                    }
                    timer.time_passed -= timer.interval_ms;
                }
                for(std::size_t to = 0; to < script.timers.size(); to++) {
                    auto &timer_to = script.timers[to];
                    if(timer.timer_id == timer_to.timer_id) {
                        if(deleted) {
                            script.timers.erase(script.timers.begin() + to);
                        }
                        else {
                            timer_to.time_passed = timer.time_passed;
                        }
                        break;
                    }
                }
            }
        }
    }

    static void clear_lua_vitality_snapshots() noexcept;

    static void map_load_callback() noexcept {
        clear_lua_vitality_snapshots();
        for(std::size_t i = 0; i < scripts.size(); i++) {
            if(!scripts[i].get()->global) {
                scripts.erase(scripts.begin() + i);
                break;
            }
        }
        load_map_script();
        for(std::size_t i = 0; i < scripts.size(); i++) {
            refresh_variables(scripts[i].get()->state);
        }
        auto cb = basic_callback(c_map_load);
        call_all_priorities(cb);
    }

    static void pretick_callback() noexcept {
        for(std::size_t i = 0; i < scripts.size(); i++) {
            refresh_client_index(scripts[i].get()->state);
        }
        auto cb = basic_callback(c_pretick);
        call_all_priorities(cb);
    }

    static void tick_callback() noexcept {
        check_timers();
        auto cb = basic_callback(c_tick);
        call_all_priorities(cb);
    }

    static void preframe_callback() noexcept {
        auto cb = basic_callback(c_preframe);
        call_all_priorities(cb);
    }

    struct LuaVitalitySnapshot {
        bool active = false;
        PlayerID player_id = HaloID::null_id();
        ObjectID object_id = HaloID::null_id();
        float maximum_health = 0.0F;
        float maximum_shields = 0.0F;
        float health = 0.0F;
        float shields = 0.0F;
        bool dead = false;
        bool shield_depleted = false;
        NetworkedDatumRole datum_role = NETWORKED_DATUM_MASTER;
        bool has_been_updated_from_network = false;
        std::uint16_t deaths_at_spawn = 0;
        TickCount last_death_time_at_spawn = 0;
    };

    static constexpr std::size_t LUA_VITALITY_PLAYER_LIMIT = 16;
    static std::array<LuaVitalitySnapshot, LUA_VITALITY_PLAYER_LIMIT> lua_vitality_snapshots {};
    static bool lua_vitality_tracking_enabled = false;

    static void clear_lua_vitality_snapshots() noexcept {
        for(auto &snapshot : lua_vitality_snapshots) {
            snapshot = LuaVitalitySnapshot {};
        }
    }

    static const char *networked_datum_role_name(NetworkedDatumRole role) noexcept {
        switch(role) {
            case NETWORKED_DATUM_MASTER: return "master";
            case NETWORKED_DATUM_PUPPET: return "puppet";
            case NETWORKED_DATUM_PUPPET_CONTROLLED_BY_LOCAL_PLAYER: return "puppet_controlled_by_local_player";
            case NETWORKED_DATUM_AUTONOMOUS: return "autonomous";
            default: return "unknown";
        }
    }

    static float effective_vitality_points(float ratio, float maximum) noexcept {
        if(!std::isfinite(ratio) || !std::isfinite(maximum) || maximum <= 0.0F) {
            return 0.0F;
        }
        return (ratio > 0.0F ? ratio : 0.0F) * maximum;
    }

    static bool vitality_float_changed(float before, float after) noexcept {
        static constexpr float EPSILON = 0.000001F;
        return std::fabs(before - after) > EPSILON;
    }

    static void dispatch_vitality_update(const LuaVitalitySnapshot &before,
                                         const LuaVitalitySnapshot &after,
                                         bool is_local_player,
                                         bool terminal_death,
                                         bool object_current_after) noexcept {
        const auto health_points_before = effective_vitality_points(before.health, before.maximum_health);
        const auto health_points_after = effective_vitality_points(after.health, after.maximum_health);
        const auto shield_points_before = effective_vitality_points(before.shields, before.maximum_shields);
        const auto shield_points_after = effective_vitality_points(after.shields, after.maximum_shields);

        const auto health_points_delta = health_points_after - health_points_before;
        const auto shield_points_delta = shield_points_after - shield_points_before;
        const auto health_points_lost = health_points_delta < 0.0F ? -health_points_delta : 0.0F;
        const auto shield_points_lost = shield_points_delta < 0.0F ? -shield_points_delta : 0.0F;
        const auto health_points_gained = health_points_delta > 0.0F ? health_points_delta : 0.0F;
        const auto shield_points_gained = shield_points_delta > 0.0F ? shield_points_delta : 0.0F;

        const bool health_changed =
            vitality_float_changed(before.health, after.health) ||
            vitality_float_changed(before.maximum_health, after.maximum_health);
        const bool shields_changed =
            vitality_float_changed(before.shields, after.shields) ||
            vitality_float_changed(before.maximum_shields, after.maximum_shields);

        auto callback = [&](EventPriority priority) noexcept {
            for(std::size_t i = 0; i < scripts.size(); i++) {
                auto &script = *scripts[i].get();
                auto &script_callback = script.c_vitality_update;
                auto *state = script.state;
                for_each_lua_callback(script_callback, priority, [&](const char *function_name) noexcept {
                    lua_getglobal(state, function_name);
                    lua_createtable(state, 0, 40);

                auto set_number = [state](const char *key, lua_Number value) {
                    lua_pushnumber(state, value);
                    lua_setfield(state, -2, key);
                };
                auto set_integer = [state](const char *key, lua_Integer value) {
                    lua_pushinteger(state, value);
                    lua_setfield(state, -2, key);
                };
                auto set_boolean = [state](const char *key, bool value) {
                    lua_pushboolean(state, value);
                    lua_setfield(state, -2, key);
                };
                auto set_string = [state](const char *key, const char *value) {
                    lua_pushstring(state, value);
                    lua_setfield(state, -2, key);
                };

                set_number("player_id", static_cast<lua_Number>(after.player_id.whole_id));
                set_number("object_id", static_cast<lua_Number>(after.object_id.whole_id));
                set_boolean("is_local_player", is_local_player);

                set_number("maximum_health_before", before.maximum_health);
                set_number("maximum_health_after", after.maximum_health);
                set_number("maximum_shields_before", before.maximum_shields);
                set_number("maximum_shields_after", after.maximum_shields);

                set_number("health_before", before.health);
                set_number("health_after", after.health);
                set_number("shields_before", before.shields);
                set_number("shields_after", after.shields);

                set_number("health_points_before", health_points_before);
                set_number("health_points_after", health_points_after);
                set_number("shield_points_before", shield_points_before);
                set_number("shield_points_after", shield_points_after);

                set_number("health_points_delta", health_points_delta);
                set_number("shield_points_delta", shield_points_delta);
                set_number("health_points_lost", health_points_lost);
                set_number("shield_points_lost", shield_points_lost);
                set_number("health_points_gained", health_points_gained);
                set_number("shield_points_gained", shield_points_gained);
                set_number("total_points_lost", health_points_lost + shield_points_lost);

                set_boolean("health_changed", health_changed);
                set_boolean("shields_changed", shields_changed);
                set_boolean("maximum_health_changed", vitality_float_changed(before.maximum_health, after.maximum_health));
                set_boolean("maximum_shields_changed", vitality_float_changed(before.maximum_shields, after.maximum_shields));

                set_boolean("dead_before", before.dead);
                set_boolean("dead_after", after.dead);
                set_boolean("killed", !before.dead && after.dead);
                set_boolean("revived", before.dead && !after.dead);
                set_boolean("shield_depleted_before", before.shield_depleted);
                set_boolean("shield_depleted_after", after.shield_depleted);

                static constexpr float SHIELD_BREAK_EPSILON = 0.000001F;
                const bool shield_broken =
                    before.shields > SHIELD_BREAK_EPSILON &&
                    after.shields <= SHIELD_BREAK_EPSILON &&
                    shield_points_lost > SHIELD_BREAK_EPSILON;
                set_boolean("shield_broken", shield_broken);

                set_boolean("terminal_death", terminal_death);
                set_boolean("object_current_after", object_current_after);

                set_integer("datum_role", static_cast<lua_Integer>(after.datum_role));
                set_string("datum_role_name", networked_datum_role_name(after.datum_role));
                set_boolean("has_been_updated_from_network", after.has_been_updated_from_network);

                    pcall(state, 1, 0);
                });
            }
        };

        call_all_priorities(callback);
    }

    static bool player_death_confirmed_since_snapshot(const Player &player,
                                                       const LuaVitalitySnapshot &snapshot) noexcept {
        return player.deaths != snapshot.deaths_at_spawn ||
               player.last_death_time != snapshot.last_death_time_at_spawn;
    }

    static void dispatch_terminal_player_death(const LuaVitalitySnapshot &snapshot,
                                               bool is_local_player) noexcept {
        LuaVitalitySnapshot terminal = snapshot;
        terminal.health = 0.0F;
        terminal.shields = 0.0F;
        terminal.dead = true;
        terminal.shield_depleted = true;
        dispatch_vitality_update(snapshot, terminal, is_local_player, true, false);
    }

    static void vitality_update_callback() noexcept {
        if(!lua_vitality_tracking_enabled) {
            return;
        }

        auto &players = PlayerTable::get_player_table();
        if(!players.first_element) {
            clear_lua_vitality_snapshots();
            return;
        }

        const auto client_player_id = get_client_player_id();

        for(std::size_t i = 0; i < LUA_VITALITY_PLAYER_LIMIT; i++) {
            auto &snapshot = lua_vitality_snapshots[i];

            if(i >= players.current_size) {
                snapshot = LuaVitalitySnapshot {};
                continue;
            }

            auto *player = players.get_element(i);
            if(!player || player->player_id == 0xFFFF) {
                snapshot = LuaVitalitySnapshot {};
                continue;
            }

            const auto player_id = player->get_full_id();
            const bool same_player = snapshot.active && snapshot.player_id == player_id;
            const bool is_local_player = player_id == client_player_id;

            if(snapshot.active && same_player) {
                const bool object_gone = player->object_id.is_null();
                const bool object_replaced = !object_gone && player->object_id != snapshot.object_id;
                if((object_gone || object_replaced) &&
                   player_death_confirmed_since_snapshot(*player, snapshot)) {
                    dispatch_terminal_player_death(snapshot, is_local_player);
                }
            }

            if(player->object_id.is_null()) {
                snapshot = LuaVitalitySnapshot {};
                continue;
            }

            auto *object = ObjectTable::get_object_table().get_dynamic_object(player->object_id);
            if(!object || object->object.type != OBJECT_TYPE_BIPED) {
                if(snapshot.active && same_player &&
                   player_death_confirmed_since_snapshot(*player, snapshot)) {
                    dispatch_terminal_player_death(snapshot, is_local_player);
                }
                snapshot = LuaVitalitySnapshot {};
                continue;
            }

            const auto &datum = object->object;
            if(!std::isfinite(datum.maximum_body_vitality) ||
               !std::isfinite(datum.maximum_shield_vitality) ||
               !std::isfinite(datum.body_vitality) ||
               !std::isfinite(datum.shield_vitality)) {
                snapshot = LuaVitalitySnapshot {};
                continue;
            }

            LuaVitalitySnapshot current;
            current.active = true;
            current.player_id = player_id;
            current.object_id = player->object_id;
            current.maximum_health = datum.maximum_body_vitality;
            current.maximum_shields = datum.maximum_shield_vitality;
            current.health = datum.body_vitality;
            current.shields = datum.shield_vitality;
            current.dead = (datum.damage_flags & (1u << OBJECT_DAMAGE_FLAGS_DEAD_BIT)) != 0;
            current.shield_depleted = (datum.damage_flags & (1u << OBJECT_DAMAGE_FLAGS_SHIELD_DEPLETED_BIT)) != 0;
            current.datum_role = datum.datum_role;
            current.has_been_updated_from_network =
                (datum.flags & (1u << OBJECT_DATA_FLAGS_HAS_BEEN_UPDATED_FROM_NETWORK_BIT)) != 0;

            if(snapshot.active &&
               snapshot.player_id == current.player_id &&
               snapshot.object_id == current.object_id) {
                current.deaths_at_spawn = snapshot.deaths_at_spawn;
                current.last_death_time_at_spawn = snapshot.last_death_time_at_spawn;
            }
            else {
                current.deaths_at_spawn = player->deaths;
                current.last_death_time_at_spawn = player->last_death_time;
            }

            if(!snapshot.active ||
               snapshot.player_id != current.player_id ||
               snapshot.object_id != current.object_id) {
                snapshot = current;
                continue;
            }

            const bool changed =
                vitality_float_changed(snapshot.maximum_health, current.maximum_health) ||
                vitality_float_changed(snapshot.maximum_shields, current.maximum_shields) ||
                vitality_float_changed(snapshot.health, current.health) ||
                vitality_float_changed(snapshot.shields, current.shields) ||
                snapshot.dead != current.dead ||
                snapshot.shield_depleted != current.shield_depleted;

            if(changed) {
                const auto before = snapshot;
                snapshot = current;
                dispatch_vitality_update(before, current, is_local_player, false, true);
            }
            else {
                snapshot = current;
            }
        }
    }

    static void frame_callback() noexcept {
        check_timers();
        vitality_update_callback();
        auto cb = basic_callback(c_frame);
        call_all_priorities(cb);
    }

    static void camera_callback() noexcept {
        auto &data = camera_data();

        auto cb = [&data](EventPriority priority) {
            for(std::size_t i = 0; i < scripts.size(); i++) {
                auto &script = *scripts[i].get();
                auto &script_callback = script.c_precamera;
                if(script_callback.callback_function != "" && script_callback.priority == priority) {
                    auto *&state = script.state;
                    lua_getglobal(state, script_callback.callback_function.data());
                    lua_pushnumber(state, data.position.x);
                    lua_pushnumber(state, data.position.y);
                    lua_pushnumber(state, data.position.z);
                    lua_pushnumber(state, data.fov);
                    lua_pushnumber(state, data.orientation[0].x);
                    lua_pushnumber(state, data.orientation[0].y);
                    lua_pushnumber(state, data.orientation[0].z);
                    lua_pushnumber(state, data.orientation[1].x);
                    lua_pushnumber(state, data.orientation[1].y);
                    lua_pushnumber(state, data.orientation[1].z);
                    if(pcall(state, 10, 10) == LUA_OK) {
                        if(priority != EVENT_PRIORITY_FINAL) {
                            #define set_if_possible(val, i) if(lua_isnumber(state, i)) val = lua_tonumber(state, i)
                            set_if_possible(data.position.x, -10);
                            set_if_possible(data.position.y, -9);
                            set_if_possible(data.position.z, -8);
                            set_if_possible(data.fov, -7);
                            set_if_possible(data.orientation[0].x, -6);
                            set_if_possible(data.orientation[0].y, -5);
                            set_if_possible(data.orientation[0].z, -4);
                            set_if_possible(data.orientation[1].x, -3);
                            set_if_possible(data.orientation[1].y, -2);
                            set_if_possible(data.orientation[1].z, -1);
                        }
                        lua_pop(state, 10);
                    }
                }
            }
        };
        call_all_priorities(cb);
    }

    #define allow_string_callback(callback) [](EventPriority priority, const char *string, bool &allow) noexcept { \
        for(std::size_t i = 0; i < scripts.size() && allow; i++) { \
            auto &script = *scripts[i].get(); \
            auto &script_callback = script.callback; \
            if(script_callback.callback_function != "" && script_callback.priority == priority) { \
                auto *&state = script.state; \
                lua_getglobal(state, script_callback.callback_function.data()); \
                lua_pushstring(state, string); \
                if(pcall(state, 1, 1) == LUA_OK) { \
                    if(!lua_isnil(state,-1) && priority != EVENT_PRIORITY_FINAL) { \
                        allow = lua_toboolean(state,-1); \
                        if(script.version < 2.02) { \
                            allow = !allow; /* BC */ \
                        } \
                    } \
                    lua_pop(state,1); \
                } \
            } \
        } \
    };

    #define call_all_priorities_allow_str(function,str,allow) \
        function(EVENT_PRIORITY_BEFORE,str,allow); \
        function(EVENT_PRIORITY_DEFAULT,str,allow); \
        function(EVENT_PRIORITY_AFTER,str,allow); \
        function(EVENT_PRIORITY_FINAL,str,allow)

    bool on_command_lua(const char *command) noexcept {
        bool allow = true;
        auto cb = allow_string_callback(c_command);
        call_all_priorities_allow_str(cb, command, allow);
        return allow;
    }

    bool rcon_message_callback(const char *message) noexcept {
        bool allow = true;
        auto cb = allow_string_callback(c_rcon_message);
        call_all_priorities_allow_str(cb, message, allow);
        return allow;
    }

    static bool preconnect_callback(EventPriority priority, std::uint32_t &ip, std::uint16_t &port) noexcept {
        bool allow = true;
        for(std::size_t i = 0; i < scripts.size() && allow; i++) {
            auto &script = *scripts[i].get();
            auto &script_callback = script.c_preconnect;
            if(script_callback.callback_function == "" || script_callback.priority != priority) {
                continue;
            }
            auto *&state = script.state;
            lua_getglobal(state, script_callback.callback_function.data());
            lua_pushnumber(state, static_cast<lua_Number>(ip));
            lua_pushinteger(state, static_cast<lua_Integer>(port));
            if(pcall(state, 2, 3) == LUA_OK) {
                if(priority != EVENT_PRIORITY_FINAL) {
                    if(lua_isboolean(state, -3)) {
                        allow = lua_toboolean(state, -3) != 0;
                    }
                    std::uint32_t new_ip;
                    std::uint16_t new_port;
                    if(lua_to_uint32(state, -2, new_ip)) {
                        ip = new_ip;
                    }
                    if(lua_to_uint16(state, -1, new_port) && new_port != 0) {
                        port = new_port;
                    }
                }
                lua_pop(state, 3);
            }
        }
        return allow;
    }

    static bool preconnect_before(std::uint32_t &ip, std::uint16_t &port, const char *) noexcept {
        return preconnect_callback(EVENT_PRIORITY_BEFORE, ip, port);
    }

    static bool preconnect_default(std::uint32_t &ip, std::uint16_t &port, const char *) noexcept {
        return preconnect_callback(EVENT_PRIORITY_DEFAULT, ip, port);
    }

    static bool preconnect_after(std::uint32_t &ip, std::uint16_t &port, const char *) noexcept {
        return preconnect_callback(EVENT_PRIORITY_AFTER, ip, port);
    }

    static bool preconnect_final(std::uint32_t &ip, std::uint16_t &port, const char *) noexcept {
        return preconnect_callback(EVENT_PRIORITY_FINAL, ip, port);
    }

    static void enable_preconnect_callbacks() {
        if(!get_chimera().feature_present("client")) {
            return;
        }
        add_preconnect_event(preconnect_before, EVENT_PRIORITY_BEFORE);
        add_preconnect_event(preconnect_default, EVENT_PRIORITY_DEFAULT);
        add_preconnect_event(preconnect_after, EVENT_PRIORITY_AFTER);
        add_preconnect_event(preconnect_final, EVENT_PRIORITY_FINAL);
    }

    static bool valid_object_id(std::uint32_t whole_id, bool allow_null) noexcept {
        ObjectID id;
        id.whole_id = whole_id;
        if(id.is_null()) {
            return allow_null;
        }
        return ObjectTable::get_object_table().get_dynamic_object(id) != nullptr;
    }

    static bool valid_player_id(std::uint32_t whole_id, bool allow_null) noexcept {
        PlayerID id;
        id.whole_id = whole_id;
        if(id.is_null()) {
            return allow_null;
        }
        return PlayerTable::get_player_table().get_player(id) != nullptr;
    }

    static bool valid_damage_effect_id(std::uint32_t whole_id) noexcept {
        TagID id;
        id.whole_id = whole_id;
        if(id.is_null()) {
            return false;
        }
        auto *tag = get_tag(id);
        return tag && tag->id.whole_id == id.whole_id && tag->primary_class == TagClassInt::TAG_CLASS_DAMAGE_EFFECT;
    }

    static bool damage_callback(EventPriority priority, ObjectID &object, TagID &damage_effect, float &multiplier, PlayerID &causing_player, ObjectID &causing_object) noexcept {
        bool allow = true;
        for(std::size_t i = 0; i < scripts.size() && allow; i++) {
            auto &script = *scripts[i].get();
            auto &script_callback = script.c_damage;
            if(script_callback.callback_function == "" || script_callback.priority != priority) {
                continue;
            }
            auto *&state = script.state;
            lua_getglobal(state, script_callback.callback_function.data());
            lua_pushnumber(state, static_cast<lua_Number>(object.whole_id));
            lua_pushnumber(state, static_cast<lua_Number>(damage_effect.whole_id));
            lua_pushnumber(state, multiplier);
            lua_pushnumber(state, static_cast<lua_Number>(causing_player.whole_id));
            lua_pushnumber(state, static_cast<lua_Number>(causing_object.whole_id));

            int argument_count = 5;
            if(script.version >= 2.066) {
                const auto &metadata = current_damage_event_metadata();
                lua_pushinteger(state, static_cast<lua_Integer>(metadata.node_index));
                lua_pushinteger(state, static_cast<lua_Integer>(metadata.region_index));
                lua_pushinteger(state, static_cast<lua_Integer>(metadata.material_index));
                argument_count = 8;
            }

            if(pcall(state, argument_count, 6) == LUA_OK) {
                if(priority != EVENT_PRIORITY_FINAL) {
                    if(lua_isboolean(state, -6)) {
                        allow = lua_toboolean(state, -6) != 0;
                    }
                    std::uint32_t new_id;
                    float new_multiplier;
                    if(lua_to_uint32(state, -5, new_id) && valid_object_id(new_id, false)) {
                        object.whole_id = new_id;
                    }
                    if(lua_to_uint32(state, -4, new_id) && valid_damage_effect_id(new_id)) {
                        damage_effect.whole_id = new_id;
                    }
                    if(lua_to_float(state, -3, new_multiplier)) {
                        multiplier = new_multiplier;
                    }
                    if(lua_to_uint32(state, -2, new_id) && valid_player_id(new_id, true)) {
                        causing_player.whole_id = new_id;
                    }
                    if(lua_to_uint32(state, -1, new_id) && valid_object_id(new_id, true)) {
                        causing_object.whole_id = new_id;
                    }
                }
                lua_pop(state, 6);
            }
        }
        return allow;
    }

    static bool damage_before(ObjectID &object, TagID &damage_effect, float &multiplier, PlayerID &causing_player, ObjectID &causing_object) noexcept {
        return damage_callback(EVENT_PRIORITY_BEFORE, object, damage_effect, multiplier, causing_player, causing_object);
    }

    static bool damage_default(ObjectID &object, TagID &damage_effect, float &multiplier, PlayerID &causing_player, ObjectID &causing_object) noexcept {
        return damage_callback(EVENT_PRIORITY_DEFAULT, object, damage_effect, multiplier, causing_player, causing_object);
    }

    static bool damage_after(ObjectID &object, TagID &damage_effect, float &multiplier, PlayerID &causing_player, ObjectID &causing_object) noexcept {
        return damage_callback(EVENT_PRIORITY_AFTER, object, damage_effect, multiplier, causing_player, causing_object);
    }

    static bool damage_final(ObjectID &object, TagID &damage_effect, float &multiplier, PlayerID &causing_player, ObjectID &causing_object) noexcept {
        return damage_callback(EVENT_PRIORITY_FINAL, object, damage_effect, multiplier, causing_player, causing_object);
    }

    static void enable_damage_callbacks() {
        if(!get_chimera().feature_present("core")) {
            return;
        }
        add_damage_event(damage_before, EVENT_PRIORITY_BEFORE);
        add_damage_event(damage_default, EVENT_PRIORITY_DEFAULT);
        add_damage_event(damage_after, EVENT_PRIORITY_AFTER);
        add_damage_event(damage_final, EVENT_PRIORITY_FINAL);
    }

    static void damage_result_callback(EventPriority priority, const DamageResultEvent &result) noexcept {
        for(std::size_t i = 0; i < scripts.size(); i++) {
            auto &script = *scripts[i].get();
            auto &script_callback = script.c_damage_result;
            auto *&state = script.state;
            for_each_lua_callback(script_callback, priority, [&](const char *function_name) noexcept {
                lua_getglobal(state, function_name);
                lua_createtable(state, 0, 30);

            auto set_id = [state](const char *key, HaloID id) {
                if(id.is_null()) {
                    lua_pushnil(state);
                }
                else {
                    lua_pushnumber(state, static_cast<lua_Number>(id.whole_id));
                }
                lua_setfield(state, -2, key);
            };
            auto set_number = [state](const char *key, lua_Number value) {
                lua_pushnumber(state, value);
                lua_setfield(state, -2, key);
            };
            auto set_integer = [state](const char *key, lua_Integer value) {
                lua_pushinteger(state, value);
                lua_setfield(state, -2, key);
            };
            auto set_boolean = [state](const char *key, bool value) {
                lua_pushboolean(state, value);
                lua_setfield(state, -2, key);
            };
            auto set_optional_number = [state](const char *key, bool valid, lua_Number value) {
                if(valid) {
                    lua_pushnumber(state, value);
                }
                else {
                    lua_pushnil(state);
                }
                lua_setfield(state, -2, key);
            };

            set_id("target_object_id", result.target_object);
            set_id("damage_effect_id", result.damage_effect);
            set_number("multiplier", result.multiplier);
            set_id("causing_player_id", result.causing_player);
            set_id("causing_object_id", result.causing_object);
            set_integer("node_index", result.node_index);
            set_integer("region_index", result.region_index);
            set_integer("material_index", result.material_index);

            set_boolean("target_existed_before", result.target_existed_before);
            set_boolean("target_exists_after", result.target_exists_after);
            set_boolean("calculation_valid", result.calculation_valid);

            set_optional_number("maximum_health", result.target_existed_before, result.maximum_health);
            set_optional_number("maximum_shields", result.target_existed_before, result.maximum_shields);
            set_optional_number("health_before", result.target_existed_before, result.health_before);
            set_optional_number("shields_before", result.target_existed_before, result.shields_before);
            set_optional_number("health_after", result.target_exists_after, result.health_after);
            set_optional_number("shields_after", result.target_exists_after, result.shields_after);

            set_optional_number("raw_health_loss", result.calculation_valid, result.raw_health_loss);
            set_optional_number("raw_shield_loss", result.calculation_valid, result.raw_shield_loss);
            set_optional_number("health_loss", result.calculation_valid, result.health_loss);
            set_optional_number("shield_loss", result.calculation_valid, result.shield_loss);
            set_optional_number("health_damage", result.calculation_valid, result.health_damage);
            set_optional_number("shield_damage", result.calculation_valid, result.shield_damage);
            set_optional_number("total_damage", result.calculation_valid, result.total_damage);

            set_boolean("hit_health", result.hit_health);
            set_boolean("hit_shield", result.hit_shield);
            set_boolean("shield_broken", result.shield_broken);
            set_boolean("dead_before", result.dead_before);
            set_boolean("dead_after", result.dead_after);
            set_boolean("killed", result.killed);

                pcall(state, 1, 0);
            });
        }
    }

    static void damage_result_before(const DamageResultEvent &result) noexcept {
        damage_result_callback(EVENT_PRIORITY_BEFORE, result);
    }

    static void damage_result_default(const DamageResultEvent &result) noexcept {
        damage_result_callback(EVENT_PRIORITY_DEFAULT, result);
    }

    static void damage_result_after(const DamageResultEvent &result) noexcept {
        damage_result_callback(EVENT_PRIORITY_AFTER, result);
    }

    static void damage_result_final(const DamageResultEvent &result) noexcept {
        damage_result_callback(EVENT_PRIORITY_FINAL, result);
    }

    static void enable_damage_result_callbacks() {
        if(!get_chimera().feature_present("core")) {
            return;
        }
        add_damage_result_event(damage_result_before, EVENT_PRIORITY_BEFORE);
        add_damage_result_event(damage_result_default, EVENT_PRIORITY_DEFAULT);
        add_damage_result_event(damage_result_after, EVENT_PRIORITY_AFTER);
        add_damage_result_event(damage_result_final, EVENT_PRIORITY_FINAL);
    }

    static void revert_callback(EventPriority priority) noexcept {
        for(std::size_t i = 0; i < scripts.size(); i++) {
            auto &script = *scripts[i].get();
            auto &script_callback = script.c_revert;
            auto *&state = script.state;
            for_each_lua_callback(script_callback, priority, [&](const char *function_name) noexcept {
                lua_getglobal(state, function_name);
                pcall(state, 0, 0);
            });
        }
    }

    static void revert_before() noexcept {
        revert_callback(EVENT_PRIORITY_BEFORE);
    }

    static void revert_default() noexcept {
        revert_callback(EVENT_PRIORITY_DEFAULT);
    }

    static void revert_after() noexcept {
        revert_callback(EVENT_PRIORITY_AFTER);
    }

    static void revert_final() noexcept {
        revert_callback(EVENT_PRIORITY_FINAL);
    }

    static void enable_revert_callbacks() {
        if(!get_chimera().feature_present("core")) {
            return;
        }
        add_revert_event(revert_before, EVENT_PRIORITY_BEFORE);
        add_revert_event(revert_default, EVENT_PRIORITY_DEFAULT);
        add_revert_event(revert_after, EVENT_PRIORITY_AFTER);
        add_revert_event(revert_final, EVENT_PRIORITY_FINAL);
    }

    static void multiplayer_event_callback(const MultiplayerEvent &event) noexcept {
        auto callback = [&event](EventPriority priority) noexcept {
            for(std::size_t i = 0; i < scripts.size(); i++) {
                auto &script = *scripts[i].get();
                auto &script_callback = script.c_multiplayer_event;
                auto *state = script.state;
                for_each_lua_callback(script_callback, priority, [&](const char *function_name) noexcept {
                    lua_getglobal(state, function_name);
                    lua_createtable(state, 0, 4);

                lua_pushstring(state, multiplayer_event_type_name(event.type));
                lua_setfield(state, -2, "type");

                auto set_player_id = [state](const char *key, PlayerID id) {
                    if(id.is_null()) {
                        lua_pushnil(state);
                    }
                    else {
                        lua_pushinteger(state, static_cast<lua_Integer>(id.whole_id));
                    }
                    lua_setfield(state, -2, key);
                };

                set_player_id("local_player_id", event.local_player);
                set_player_id("killer_player_id", event.killer_player);
                set_player_id("victim_player_id", event.victim_player);

                    pcall(state, 1, 0);
                });
            }
        };

        call_all_priorities(callback);
    }

    static void enable_multiplayer_event_callbacks() noexcept {
        if(!get_chimera().feature_present("client_multiplayer_event")) {
            return;
        }
        add_multiplayer_event(multiplayer_event_callback, EVENT_PRIORITY_DEFAULT);
    }

    static void dispatch_lua_render_callback(LuaScriptCallback LuaScript::*member, bool after) noexcept {
        auto callback = [member, after](EventPriority priority) noexcept {
            for(std::size_t i = 0; i < scripts.size(); i++) {
                auto &script = *scripts[i].get();
                auto &script_callback = script.*member;
                auto *state = script.state;
                for_each_lua_callback(script_callback, priority, [&](const char *function_name) noexcept {
                    lua_getglobal(state, function_name);
                    lua_createtable(state, 0, 1);
                    lua_pushstring(state, after ? "after" : "before");
                    lua_setfield(state, -2, "stage");
                    pcall(state, 1, 0);
                });
            }
        };
        call_all_priorities(callback);
    }

    static void hud_render_callback(bool after) noexcept {
        dispatch_lua_render_callback(&LuaScript::c_hud_render, after);
    }

    static void ui_render_callback(bool after) noexcept {
        dispatch_lua_render_callback(&LuaScript::c_ui_render, after);
    }

    static void enable_hud_render_callbacks() noexcept {
        add_hud_render_event(hud_render_callback, EVENT_PRIORITY_DEFAULT);
    }

    static void enable_ui_render_callbacks() noexcept {
        add_ui_render_event(ui_render_callback, EVENT_PRIORITY_DEFAULT);
    }

    void detach_lua_render_callbacks() noexcept {

        remove_hud_render_event(hud_render_callback);
        remove_ui_render_event(ui_render_callback);
    }

    struct UnderscoreSpaceThing {
        std::string i_text;

        bool operator ==(const char *&other) const noexcept {
            if(!other) {
                return false;
            }
            if(this->i_text == other) {
                return true;
            }
            std::string normalized(other);
            for(auto &character : normalized) {
                if(character == '_') {
                    character = ' ';
                }
            }
            return this->i_text == normalized;
        }

        UnderscoreSpaceThing(const char *string) noexcept : i_text(string) {
            for(std::size_t i = 0; i < i_text.size(); i++) {
                if(i_text[i] == '_') {
                    i_text[i] = ' ';
                }
            }
        }
    };

    static std::string normalize_callback_name(const char *callback_name) noexcept {
        std::string normalized = callback_name ? callback_name : "";
        for(auto &character : normalized) {
            if(character == '_') {
                character = ' ';
            }
        }
        return normalized;
    }

    static LuaScriptCallback *listener_callback_for_name(LuaScript &script, const char *callback_name) noexcept {
        const auto name = normalize_callback_name(callback_name);
        if(name == "frame") return &script.c_frame;
        if(name == "preframe") return &script.c_preframe;
        if(name == "map load") return &script.c_map_load;
        if(name == "tick") return &script.c_tick;
        if(name == "pretick") return &script.c_pretick;
        if(name == "unload") return &script.c_unload;
        if(name == "damage result") return &script.c_damage_result;
        if(name == "vitality update") return &script.c_vitality_update;
        if(name == "multiplayer event") return &script.c_multiplayer_event;
        if(name == "hud render") return &script.c_hud_render;
        if(name == "ui render") return &script.c_ui_render;
        if(name == "revert") return &script.c_revert;
        return nullptr;
    }

    static EventPriority lua_callback_priority(lua_State *state, int index) noexcept {
        if(index > lua_gettop(state)) {
            return EVENT_PRIORITY_DEFAULT;
        }
        const auto *value = luaL_checkstring(state, index);
        if(std::strcmp(value, "before") == 0) return EVENT_PRIORITY_BEFORE;
        if(std::strcmp(value, "default") == 0) return EVENT_PRIORITY_DEFAULT;
        if(std::strcmp(value, "after") == 0) return EVENT_PRIORITY_AFTER;
        if(std::strcmp(value, "final") == 0) return EVENT_PRIORITY_FINAL;
        return EVENT_PRIORITY_DEFAULT;
    }

    static void enable_listener_callback(const char *callback_name) noexcept {
        const auto name = normalize_callback_name(callback_name);
        if(name == "damage result") {
            enable_damage_result_callbacks();
        }
        else if(name == "vitality update") {
            clear_lua_vitality_snapshots();
            lua_vitality_tracking_enabled = true;
        }
        else if(name == "multiplayer event") {
            enable_multiplayer_event_callbacks();
        }
        else if(name == "hud render") {
            enable_hud_render_callbacks();
        }
        else if(name == "ui render") {
            enable_ui_render_callbacks();
        }
        else if(name == "revert") {
            enable_revert_callbacks();
        }
    }

    int lua_add_callback(lua_State *state) noexcept {
        const auto args = lua_gettop(state);
        if(args != 2 && args != 3) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"));
        }

        const auto *callback_name = luaL_checkstring(state, 1);
        const auto *function_name = luaL_checkstring(state, 2);
        auto &script = script_from_state(state);
        auto *callback = listener_callback_for_name(script, callback_name);
        if(!callback) {
            return luaL_error(state, localize("chimera_lua_error_invalid_callback"), callback_name);
        }

        const auto priority = lua_callback_priority(state, 3);
        auto id = script.next_callback_id++;
        if(id == 0) {
            id = script.next_callback_id++;
        }

        LuaScriptCallbackListener listener;
        listener.id = id;
        listener.callback_function = function_name ? function_name : "";
        listener.priority = priority;

        bool reused = false;
        for(auto &slot : callback->listeners) {
            if(slot.callback_function.empty()) {
                slot = listener;
                reused = true;
                break;
            }
        }
        if(!reused) {
            callback->listeners.emplace_back(std::move(listener));
        }

        enable_listener_callback(callback_name);
        lua_pushinteger(state, static_cast<lua_Integer>(id));
        return 1;
    }

    int lua_remove_callback(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"));
        }

        const auto id = static_cast<size_t>(luaL_checkinteger(state, 1));
        auto &script = script_from_state(state);
        std::array<LuaScriptCallback *, 12> callbacks {{
            &script.c_frame,
            &script.c_preframe,
            &script.c_map_load,
            &script.c_tick,
            &script.c_pretick,
            &script.c_unload,
            &script.c_damage_result,
            &script.c_vitality_update,
            &script.c_multiplayer_event,
            &script.c_hud_render,
            &script.c_ui_render,
            &script.c_revert
        }};

        for(auto *callback : callbacks) {
            for(auto &listener : callback->listeners) {
                if(listener.id == id && !listener.callback_function.empty()) {
                    listener.id = 0;
                    listener.callback_function.clear();
                    listener.priority = EVENT_PRIORITY_DEFAULT;
                    lua_pushboolean(state, true);
                    return 1;
                }
            }
        }

        lua_pushboolean(state, false);
        return 1;
    }

    void call_lua_unload_callbacks(LuaScript &script) noexcept {
        if(!script.state) {
            return;
        }
        auto *state = script.state;
        auto invoke = [&](EventPriority priority) noexcept {
            for_each_lua_callback(script.c_unload, priority, [&](const char *function_name) noexcept {
                lua_getglobal(state, function_name);
                if(!lua_isnil(state, -1)) {
                    pcall(state, 0, 0);
                }
                else {
                    lua_pop(state, 1);
                }
            });
        };
        call_all_priorities(invoke);
    }

    int lua_set_callback(lua_State *state) noexcept {
        int args = lua_gettop(state);

        if(args >= 1 && args <= 3) {
            const char *callback_name = luaL_checkstring(state,1);
            const char *function_name = "";
            if(args >= 2) {
                function_name = luaL_checkstring(state,2);
                if(function_name == nullptr) {
                    function_name = "";
                }
            }
            EventPriority priority = EVENT_PRIORITY_DEFAULT;
            if(args == 3) {
                auto callback_priority = std::string(luaL_checkstring(state,3));
                if(callback_priority == "before") {
                    priority = EVENT_PRIORITY_BEFORE;
                }
                else if(callback_priority == "default") {
                    priority = EVENT_PRIORITY_DEFAULT;
                }
                else if(callback_priority == "after") {
                    priority = EVENT_PRIORITY_AFTER;
                }
                else if(callback_priority == "final") {
                    priority = EVENT_PRIORITY_FINAL;
                }
            }

            #define cpref_(cb) c_ ## cb

            #define if_callback_then_set(cb) if(UnderscoreSpaceThing(#cb) == callback_name) { \
                auto &callback = script_from_state(state).cpref_(cb); \
                callback.callback_function = function_name; \
                callback.priority = priority; \
            }

            if_callback_then_set(command)

            else if_callback_then_set(frame)
            else if_callback_then_set(preframe)
            else if_callback_then_set(map_load)
            else if_callback_then_set(map_preload)
            else if_callback_then_set(precamera)
            else if_callback_then_set(rcon_message)
            else if_callback_then_set(spawn)
            else if_callback_then_set(prespawn)
            else if_callback_then_set(tick)
            else if_callback_then_set(pretick)
            else if_callback_then_set(unload)
            else if_callback_then_set(preconnect)
            else if_callback_then_set(damage)
            else if_callback_then_set(damage_result)
            else if_callback_then_set(vitality_update)
            else if_callback_then_set(multiplayer_event)
            else if_callback_then_set(hud_render)
            else if_callback_then_set(ui_render)
            else if_callback_then_set(revert)
            else {
                return luaL_error(state, localize("chimera_lua_error_invalid_callback"), callback_name);
            }

            if(function_name[0] != 0) {
                const auto normalized_callback_name = normalize_callback_name(callback_name);
                if(normalized_callback_name == "preconnect") {
                    enable_preconnect_callbacks();
                }
                else if(normalized_callback_name == "damage") {
                    enable_damage_callbacks();
                }
                else {
                    enable_listener_callback(callback_name);
                }
            }
        }
        else {
            luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"));
        }
        return 0;
    }

    void setup_callbacks() noexcept {
        add_map_load_event(map_load_callback, EVENT_PRIORITY_BEFORE);
        add_pretick_event(pretick_callback, EVENT_PRIORITY_BEFORE);
        add_tick_event(tick_callback, EVENT_PRIORITY_BEFORE);
        add_frame_event(frame_callback, EVENT_PRIORITY_BEFORE);
        add_preframe_event(preframe_callback, EVENT_PRIORITY_BEFORE);
        add_rcon_message_event(rcon_message_callback, EVENT_PRIORITY_BEFORE);
        add_precamera_event(camera_callback, EVENT_PRIORITY_AFTER);
        add_command_event(on_command_lua, EVENT_PRIORITY_DEFAULT);

        QueryPerformanceCounter(&last_time);
    }
}
