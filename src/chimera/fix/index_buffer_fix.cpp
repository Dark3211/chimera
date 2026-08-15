// SPDX-License-Identifier: GPL-3.0-only

#include <cctype>
#include <cstring>

#include "index_buffer_fix.hpp"
#include "../chimera.hpp"
#include "../event/command.hpp"
#include "../halo_data/game_variables.hpp"
#include "../output/output.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"

namespace Chimera {
    static bool dynamic_geometry_enabled = true;
    static bool dynamic_geometry_snapshot_valid = false;
    static bool saved_draw_dynamic_unlit_geometry = true;
    static bool saved_draw_dynamic_lit_geometry = true;

    static bool dynamic_geometry_console_command(const char *command) noexcept {
        if(!command) {
            return true;
        }

        static constexpr char command_name[] = "chimera_debug_dynamic_geometry";
        static constexpr std::size_t command_name_length = sizeof(command_name) - 1;

        if(std::strncmp(command, command_name, command_name_length) != 0) {
            return true;
        }

        const char *argument = command + command_name_length;
        if(*argument != '\0' && !std::isspace(static_cast<unsigned char>(*argument))) {
            return true;
        }

        while(std::isspace(static_cast<unsigned char>(*argument))) {
            argument++;
        }

        if(*argument == '\0') {
            console_output("chimera_debug_dynamic_geometry: %s", dynamic_geometry_enabled ? "true" : "false");
            return false;
        }

        const char *argument_end = argument;
        while(*argument_end != '\0' && !std::isspace(static_cast<unsigned char>(*argument_end))) {
            argument_end++;
        }

        const std::size_t argument_length = static_cast<std::size_t>(argument_end - argument);
        while(std::isspace(static_cast<unsigned char>(*argument_end))) {
            argument_end++;
        }

        if(*argument_end != '\0') {
            console_error("chimera_debug_dynamic_geometry: expected true or false");
            return false;
        }

        bool new_enabled;
        if((argument_length == 4 && std::strncmp(argument, "true", 4) == 0) ||
           (argument_length == 1 && argument[0] == '1')) {
            new_enabled = true;
        }
        else if((argument_length == 5 && std::strncmp(argument, "false", 5) == 0) ||
                (argument_length == 1 && argument[0] == '0')) {
            new_enabled = false;
        }
        else {
            console_error("chimera_debug_dynamic_geometry: expected true or false");
            return false;
        }

        if(new_enabled == dynamic_geometry_enabled) {
            console_output("chimera_debug_dynamic_geometry: %s", new_enabled ? "true" : "false");
            return false;
        }

        if(!rasterizer_debug_options) {
            console_error("chimera_debug_dynamic_geometry: rasterizer debug options unavailable");
            return false;
        }

        if(!new_enabled) {
            // Preserve Halo's current settings so the diagnostic can restore them exactly.
            saved_draw_dynamic_unlit_geometry = rasterizer_debug_options->draw_dynamic_unlit_geometry;
            saved_draw_dynamic_lit_geometry = rasterizer_debug_options->draw_dynamic_lit_geometry;
            dynamic_geometry_snapshot_valid = true;

            // Disable world-space dynamic geometry only. Keep dynamic screen geometry
            // enabled so HUD/console remain usable while the diagnostic is active.
            rasterizer_debug_options->draw_dynamic_unlit_geometry = false;
            rasterizer_debug_options->draw_dynamic_lit_geometry = false;
        }
        else if(dynamic_geometry_snapshot_valid) {
            rasterizer_debug_options->draw_dynamic_unlit_geometry = saved_draw_dynamic_unlit_geometry;
            rasterizer_debug_options->draw_dynamic_lit_geometry = saved_draw_dynamic_lit_geometry;
            dynamic_geometry_snapshot_valid = false;
        }

        dynamic_geometry_enabled = new_enabled;
        console_output("chimera_debug_dynamic_geometry: %s", dynamic_geometry_enabled ? "true" : "false");
        return false;
    }

    void set_up_index_buffer_fix() noexcept {
        add_command_event(dynamic_geometry_console_command, EVENT_PRIORITY_BEFORE);

        if(get_chimera().feature_present("client_invalid_lock")) {
            SigByte mod[] = { 0x08 };
            write_code_s(get_chimera().get_signature("d3d_lock_index_buffer_sig").data() + 6, mod);
        }
    }
}
