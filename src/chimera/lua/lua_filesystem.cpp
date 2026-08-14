// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include <string>
#include <sstream>
#include <fstream>
#include "../localization/localization.hpp"
#include "lua_script.hpp"
#include "lua_filesystem.hpp"

namespace fs = std::filesystem;

namespace Chimera {
    /**
     * Get data directory of a given script state
     * @param state     Lua script state
     * @return          Path to data directory; empty on filesystem failure
     */
    static fs::path get_script_data_path(lua_State *state) noexcept {
        if(!state) {
            return {};
        }

        auto &script = script_from_state(state);
        std::error_code filesystem_error;
        auto chimera_absolute_path = fs::absolute(get_chimera().get_path(), filesystem_error);
        if(filesystem_error) {
            return {};
        }

        auto script_filename = fs::path(script.name).filename().string();
        if(script_filename.size() >= 4) {
            script_filename.resize(script_filename.size() - 4);
        }

        auto scripts_data_directory = chimera_absolute_path / "lua" / "data";
        auto data_path = scripts_data_directory / (script.global ? "global" : "map") / script_filename;

        fs::create_directories(data_path, filesystem_error);
        if(filesystem_error) {
            return {};
        }

        return data_path;
    }

    /**
     * Resolve a script-provided path and verify that it remains inside the script data directory.
     * Existing symlinks/junctions are resolved by weakly_canonical().
     */
    static bool resolve_script_path(lua_State *state, const fs::path &path, fs::path &resolved_path) noexcept {
        auto data_directory = get_script_data_path(state);
        if(data_directory.empty()) {
            return false;
        }

        std::error_code filesystem_error;
        auto canonical_data_directory = fs::weakly_canonical(data_directory, filesystem_error);
        if(filesystem_error) {
            return false;
        }

        auto candidate = fs::weakly_canonical(data_directory / path, filesystem_error);
        if(filesystem_error) {
            return false;
        }

        auto data_component = canonical_data_directory.begin();
        auto candidate_component = candidate.begin();
        for(; data_component != canonical_data_directory.end(); ++data_component, ++candidate_component) {
            if(candidate_component == candidate.end() || *data_component != *candidate_component) {
                return false;
            }
        }

        resolved_path = candidate;
        return true;
    }

    static int lua_create_directory(lua_State *state) noexcept {
        int args = lua_gettop(state);
        if(args == 1) {
            const char *path = luaL_checkstring(state, 1);
            fs::path resolved_path;
            if(resolve_script_path(state, path, resolved_path)) {
                std::error_code filesystem_error;
                bool created = fs::create_directories(resolved_path, filesystem_error);
                lua_pushboolean(state, !filesystem_error && created);
                return 1;
            }
            else {
                return luaL_error(state, localize("chimera_lua_error_scope_path"));
            }
        }
        else {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "create_directory");
        }
    }

    static int lua_remove_directory(lua_State *state) noexcept {
        int args = lua_gettop(state);
        if(args == 1) {
            const char *path = luaL_checkstring(state, 1);
            fs::path resolved_path;
            if(resolve_script_path(state, path, resolved_path)) {
                std::error_code filesystem_error;
                if(fs::is_directory(resolved_path, filesystem_error) && !filesystem_error) {
                    auto removed = fs::remove_all(resolved_path, filesystem_error);
                    lua_pushboolean(state, !filesystem_error && removed != 0);
                }
                else {
                    lua_pushboolean(state, false);
                }
                return 1;
            }
            else {
                return luaL_error(state, localize("chimera_lua_error_scope_path"));
            }
        }
        else {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "remove_directory");
        }
    }

    static int lua_list_directory(lua_State *state) noexcept {
        int args = lua_gettop(state);
        if(args == 1) {
            const char *path = luaL_checkstring(state, 1);
            fs::path resolved_path;
            if(resolve_script_path(state, path, resolved_path)) {
                std::error_code filesystem_error;
                if(fs::is_directory(resolved_path, filesystem_error) && !filesystem_error) {
                    lua_newtable(state);
                    std::size_t table_index = 1;

                    fs::directory_iterator iterator(resolved_path, filesystem_error);
                    fs::directory_iterator end;
                    while(!filesystem_error && iterator != end) {
                        const auto &entry = *iterator;
                        auto filename = entry.path().filename().string();

                        std::error_code entry_error;
                        if(entry.is_directory(entry_error) && !entry_error) {
                            filename += "\\";
                        }

                        lua_pushstring(state, filename.c_str());
                        lua_rawseti(state, -2, table_index++);
                        iterator.increment(filesystem_error);
                    }

                    if(filesystem_error) {
                        lua_pop(state, 1);
                        lua_pushboolean(state, false);
                    }
                }
                else {
                    lua_pushboolean(state, false);
                }
                return 1;
            }
            else {
                return luaL_error(state, localize("chimera_lua_error_scope_path"));
            }
        }
        else {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "list_directory");
        }
    }

    static int lua_directory_exists(lua_State *state) noexcept {
        int args = lua_gettop(state);
        if(args == 1) {
            const char *path = luaL_checkstring(state, 1);
            fs::path resolved_path;
            if(resolve_script_path(state, path, resolved_path)) {
                std::error_code filesystem_error;
                lua_pushboolean(state, fs::is_directory(resolved_path, filesystem_error) && !filesystem_error);
                return 1;
            }
            else {
                return luaL_error(state, localize("chimera_lua_error_scope_path"));
            }
        }
        else {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "directory_exists");
        }
    }

    static int lua_write_file(lua_State *state) noexcept {
        int args = lua_gettop(state);
        if(args == 2 || args == 3) {
            const char *path = luaL_checkstring(state, 1);
            fs::path resolved_path;
            if(resolve_script_path(state, path, resolved_path)) {
                std::string content = luaL_checkstring(state, 2);
                bool append_content = (args == 3 && (lua_isboolean(state, 3) && lua_toboolean(state, 3)));

                std::ofstream file;
                file.open(resolved_path, (append_content ? std::ios::app : std::ios::trunc));
                if(file.is_open()) {
                    file << content;
                    lua_pushboolean(state, file.good());
                    file.close();
                }
                else {
                    lua_pushboolean(state, false);
                }
                return 1;
            }
            else {
                return luaL_error(state, localize("chimera_lua_error_scope_path"));
            }
        }
        else {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "write_file");
        }
    }

    static int lua_read_file(lua_State *state) noexcept {
        int args = lua_gettop(state);
        if(args == 1) {
            const char *path = luaL_checkstring(state, 1);
            fs::path resolved_path;
            if(resolve_script_path(state, path, resolved_path)) {
                std::ifstream file;
                file.open(resolved_path);
                if(file.is_open()) {
                    std::stringstream file_content_stream;
                    std::string line_buffer;
                    while(file.good() && std::getline(file, line_buffer)) {
                        file_content_stream << line_buffer << std::endl;
                    }
                    auto file_content = file_content_stream.str();
                    if(!line_buffer.empty()) {
                        // Remove last newline character
                        file_content.pop_back();
                    }
                    lua_pushstring(state, file_content.c_str());
                    file.close();
                }
                else {
                    lua_pushnil(state);
                }
                return 1;
            }
            else {
                return luaL_error(state, localize("chimera_lua_error_scope_path"));
            }
        }
        else {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "read_file");
        }
    }

    static int lua_delete_file(lua_State *state) noexcept {
        int args = lua_gettop(state);
        if(args == 1) {
            const char *path = luaL_checkstring(state, 1);
            fs::path resolved_path;
            if(resolve_script_path(state, path, resolved_path)) {
                std::error_code filesystem_error;
                if(fs::is_regular_file(resolved_path, filesystem_error) && !filesystem_error) {
                    bool removed = fs::remove(resolved_path, filesystem_error);
                    lua_pushboolean(state, !filesystem_error && removed);
                }
                else {
                    lua_pushboolean(state, false);
                }
                return 1;
            }
            else {
                return luaL_error(state, localize("chimera_lua_error_scope_path"));
            }
        }
        else {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "delete_file");
        }
    }

    static int lua_file_exists(lua_State *state) noexcept {
        int args = lua_gettop(state);
        if(args == 1) {
            const char *path = luaL_checkstring(state, 1);
            fs::path resolved_path;
            if(resolve_script_path(state, path, resolved_path)) {
                std::error_code filesystem_error;
                lua_pushboolean(state, fs::is_regular_file(resolved_path, filesystem_error) && !filesystem_error);
                return 1;
            }
            else {
                return luaL_error(state, localize("chimera_lua_error_scope_path"));
            }
        }
        else {
            return luaL_error(state, localize("chimera_lua_error_wrong_number_of_arguments"), "file_exists");
        }
    }

    void set_fs_functions(lua_State *state) noexcept {
        if(!state) {
            return;
        }

        lua_register(state, "create_directory", lua_create_directory);
        lua_register(state, "remove_directory", lua_remove_directory);
        lua_register(state, "list_directory", lua_list_directory);
        lua_register(state, "directory_exists", lua_directory_exists);
        lua_register(state, "write_file", lua_write_file);
        lua_register(state, "read_file", lua_read_file);
        lua_register(state, "delete_file", lua_delete_file);
        lua_register(state, "file_exists", lua_file_exists);
    }
}
