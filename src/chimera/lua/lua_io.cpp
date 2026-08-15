// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include "../localization/localization.hpp"
#include "lua_io.hpp"

namespace Chimera {
    static constexpr std::uintptr_t SANDBOX_ADDRESS_MIN = 0x40000000u;
    static constexpr std::uintptr_t SANDBOX_ADDRESS_MAX = 0x41B00000u;

    // Preserve Chimera's long-standing raw-read semantics: many real-world Lua
    // scripts perform dozens/hundreds of reads per frame. Calling VirtualQuery
    // for each one is both a compatibility regression and a major hot-path cost.
    // Sandboxed writes remain range-restricted, as they were before Stage G.
    static bool sandbox_allows_range(lua_State *state, std::uintptr_t address, std::size_t size) noexcept {
        if(!script_from_state(state).sandbox) {
            return true;
        }
        if(size == 0 || address < SANDBOX_ADDRESS_MIN || address > SANDBOX_ADDRESS_MAX) {
            return false;
        }
        return size - 1 <= SANDBOX_ADDRESS_MAX - address;
    }

    static bool validate_read_address(lua_State *state, std::uintptr_t address, const char *function_name) noexcept {
        if(address == 0) {
            luaL_error(state, localize("chimera_lua_error_invalid_function_argument"), 1, function_name);
            return false;
        }
        return true;
    }

    static bool validate_write_range(lua_State *state, std::uintptr_t address, std::size_t size, const char *function_name) noexcept {
        if(size == 0 || address == 0 || size - 1 > std::numeric_limits<std::uintptr_t>::max() - address) {
            luaL_error(state, localize("chimera_lua_error_invalid_function_argument"), 1, function_name);
            return false;
        }
        if(!sandbox_allows_range(state, address, size)) {
            luaL_error(state, localize("chimera_lua_error_script_sandbox_invalid_address"));
            return false;
        }
        return true;
    }

    template <typename T>
    static int lua_read_int(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "read_int");
        }

        auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
        if(!validate_read_address(state, address, "read_int")) {
            return 0;
        }

        T value = {};
        std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
        lua_pushinteger(state, static_cast<lua_Integer>(value));
        return 1;
    }

    template <typename T>
    static int lua_write_int(lua_State *state) noexcept {
        if(lua_gettop(state) != 2) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "write_int");
        }

        auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
        if(!validate_write_range(state, address, sizeof(T), "write_int")) {
            return 0;
        }

        T value = static_cast<T>(luaL_checkinteger(state, 2));
        std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(value));
        return 0;
    }

    template <typename T>
    static int lua_read_float(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "read_float");
        }

        auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
        if(!validate_read_address(state, address, "read_float")) {
            return 0;
        }

        T value = {};
        std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
        lua_pushnumber(state, static_cast<lua_Number>(value));
        return 1;
    }

    template <typename T>
    static int lua_write_float(lua_State *state) noexcept {
        if(lua_gettop(state) != 2) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "write_float");
        }

        auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
        if(!validate_write_range(state, address, sizeof(T), "write_float")) {
            return 0;
        }

        T value = static_cast<T>(luaL_checknumber(state, 2));
        std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(value));
        return 0;
    }

    static int lua_read_string8(lua_State *state) noexcept {
        if(lua_gettop(state) != 1) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "read_string");
        }

        auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
        // lua_pushstring(NULL) is defined to push nil. Preserve that useful
        // behavior instead of turning a transient null pointer into a script error.
        if(address == 0) {
            lua_pushnil(state);
            return 1;
        }

        lua_pushstring(state, reinterpret_cast<const char *>(address));
        return 1;
    }

    static int lua_write_string8(lua_State *state) noexcept {
        if(lua_gettop(state) != 2) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "write_string");
        }

        auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
        std::size_t string_length = 0;
        auto *string = luaL_checklstring(state, 2, &string_length);
        if(string_length == std::numeric_limits<std::size_t>::max()) {
            return luaL_error(state, localize("chimera_lua_error_invalid_function_argument"), 2, "write_string");
        }
        auto bytes_to_write = string_length + 1;
        if(!validate_write_range(state, address, bytes_to_write, "write_string")) {
            return 0;
        }

        std::memcpy(reinterpret_cast<void *>(address), string, string_length);
        reinterpret_cast<char *>(address)[string_length] = 0;
        return 0;
    }

    static int lua_read_bit(lua_State *state) noexcept {
        if(lua_gettop(state) != 2) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "read_bit");
        }

        auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
        std::uint32_t bit = static_cast<std::uint32_t>(luaL_checkinteger(state, 2));
        if(bit >= 32) {
            return luaL_error(state, localize("chimera_lua_error_invalid_function_argument"), 2, "read_bit");
        }
        if(!validate_read_address(state, address, "read_bit")) {
            return 0;
        }

        std::uint32_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
        lua_pushinteger(state, (value >> bit) & 1u);
        return 1;
    }

    static int lua_write_bit(lua_State *state) noexcept {
        if(lua_gettop(state) != 3) {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "write_bit");
        }

        auto address = static_cast<std::uintptr_t>(luaL_checkinteger(state, 1));
        std::uint32_t bit = static_cast<std::uint32_t>(luaL_checkinteger(state, 2));
        if(bit >= 32) {
            return luaL_error(state, localize("chimera_lua_error_invalid_function_argument"), 2, "write_bit");
        }
        if(!validate_write_range(state, address, sizeof(std::uint32_t), "write_bit")) {
            return 0;
        }

        bool bit_to_set = false;
        if(lua_isboolean(state, 3)) {
            bit_to_set = lua_toboolean(state, 3) != 0;
        }
        else {
            auto new_bit = luaL_checkinteger(state, 3);
            if(new_bit < 0 || new_bit > 1) {
                return luaL_error(state, localize("chimera_lua_error_invalid_function_argument"), 3, "write_bit");
            }
            bit_to_set = new_bit != 0;
        }

        std::uint32_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
        auto mask = static_cast<std::uint32_t>(1u << bit);
        value = bit_to_set ? (value | mask) : (value & ~mask);
        std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(value));
        return 0;
    }

    void set_io_functions(lua_State *state) noexcept {
        if(!state) {
            return;
        }

        #define lua_register_read_write_int_named(tname,ttype) \
            lua_register(state,"read_" tname,lua_read_int<ttype>); \
            lua_register(state,"write_" tname,lua_write_int<ttype>);

        #define lua_register_read_write_int(ttype) \
            lua_register_read_write_int_named(#ttype, ttype);

        #define lua_register_read_write_float_named(tname,ttype) \
            lua_register(state,"read_" tname,lua_read_float<ttype>); \
            lua_register(state,"write_" tname,lua_write_float<ttype>);

        #define lua_register_read_write_float(ttype) \
            lua_register_read_write_float_named(#ttype, ttype);

        lua_register_read_write_int_named("i8", std::int8_t);
        lua_register_read_write_int_named("i16", std::int16_t);
        lua_register_read_write_int_named("i32", std::int32_t);
        lua_register_read_write_int_named("u8", std::uint8_t);
        lua_register_read_write_int_named("u16", std::uint16_t);
        lua_register_read_write_int_named("u32", std::uint32_t);
        lua_register_read_write_int_named("byte", BYTE);
        lua_register_read_write_int_named("word", WORD);
        lua_register_read_write_int_named("dword", DWORD);
        lua_register_read_write_int(char);
        lua_register_read_write_int(short);
        lua_register_read_write_int(int);
        lua_register_read_write_int(long);
        lua_register_read_write_float_named("f32", float);
        lua_register_read_write_float_named("f64", double);
        lua_register_read_write_float(float);
        lua_register_read_write_float(double);
        lua_register(state, "read_bit", lua_read_bit);
        lua_register(state, "write_bit", lua_write_bit);
        lua_register(state, "read_string8", lua_read_string8);
        lua_register(state, "write_string8", lua_write_string8);
        lua_register(state, "read_string", lua_read_string8);
        lua_register(state, "write_string", lua_write_string8);
    }
}
