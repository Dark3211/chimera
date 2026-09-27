// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_LUA_CALLBACK_HPP
#define CHIMERA_LUA_CALLBACK_HPP

#include "lua_script.hpp"

namespace Chimera {
    /**
     * Set up Lua callbacks
    */
    int lua_set_callback(lua_State *state) noexcept;
    int lua_add_callback(lua_State *state) noexcept;
    int lua_remove_callback(lua_State *state) noexcept;
    void call_lua_unload_callbacks(LuaScript &script) noexcept;
    void detach_lua_render_callbacks() noexcept;
    void setup_callbacks() noexcept;
}

#endif
