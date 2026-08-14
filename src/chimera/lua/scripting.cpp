// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include "../chimera.hpp"
#include "lua_callback.hpp"
#include "lua_script.hpp"

namespace fs = std::filesystem;

namespace Chimera {
    static void setup_lua_folder() noexcept {
        auto lua_directory = get_chimera().get_path() / "lua";

        std::error_code filesystem_error;
        bool scripts_directory_exists = fs::exists(lua_directory / "scripts", filesystem_error);
        bool update_dir = !filesystem_error && !scripts_directory_exists;

        // Create directories without allowing filesystem failures to escape into Halo startup.
        const fs::path directories[] = {
            lua_directory / "scripts" / "global",
            lua_directory / "scripts" / "map",
            lua_directory / "data" / "global",
            lua_directory / "data" / "map",
            lua_directory / "modules"
        };

        for(const auto &directory : directories) {
            filesystem_error.clear();
            fs::create_directories(directory, filesystem_error);
        }

        if(update_dir) {
            // Move scripts from old directories. Failed entries are left in place.
            auto move_scripts = [](const fs::path &origin, const fs::path &destination) noexcept {
                std::error_code error;
                if(!fs::is_directory(origin, error) || error) {
                    return;
                }

                bool moved_everything = true;
                fs::directory_iterator iterator(origin, error);
                fs::directory_iterator end;
                while(!error && iterator != end) {
                    auto source = iterator->path();
                    auto target = destination / source.filename();

                    std::error_code rename_error;
                    fs::rename(source, target, rename_error);
                    if(rename_error) {
                        moved_everything = false;
                    }

                    iterator.increment(error);
                }

                if(error) {
                    moved_everything = false;
                }

                if(moved_everything) {
                    fs::remove(origin, error);
                }
            };

            move_scripts(lua_directory / "global", lua_directory / "scripts" / "global");
            move_scripts(lua_directory / "map", lua_directory / "scripts" / "map");
        }
    }

    void setup_lua_scripting() {
        static bool already_setup = false;
        if(already_setup) {
            return;
        }

        setup_lua_folder();
        setup_callbacks();
        load_global_scripts();
        load_map_script();

        already_setup = true;
    }

    void destroy_lua_scripting() {
        unload_scripts();
    }
}
