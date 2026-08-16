// SPDX-License-Identifier: GPL-3.0-only

#include <cctype>
#include <cstring>

#include "rasterizer_transparent_geometry.hpp"
#include "../event/command.hpp"
#include "../halo_data/game_variables.hpp"
#include "../output/output.hpp"

namespace Chimera {

    static bool transparent_generic_enabled = true;
    static bool transparent_generic_command_registered = false;

    static bool transparent_generic_console_command(const char *command) noexcept {
        if(!command) {
            return true;
        }

        static constexpr char command_name[] = "chimera_debug_transparent_generic";
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
            console_output("chimera_debug_transparent_generic: %s", transparent_generic_enabled ? "true" : "false");
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
            console_error("chimera_debug_transparent_generic: expected true or false");
            return false;
        }

        if((argument_length == 4 && std::strncmp(argument, "true", 4) == 0) ||
           (argument_length == 1 && argument[0] == '1')) {
            transparent_generic_enabled = true;
        }
        else if((argument_length == 5 && std::strncmp(argument, "false", 5) == 0) ||
                (argument_length == 1 && argument[0] == '0')) {
            transparent_generic_enabled = false;
        }
        else {
            console_error("chimera_debug_transparent_generic: expected true or false");
            return false;
        }

        console_output("chimera_debug_transparent_generic: %s", transparent_generic_enabled ? "true" : "false");
        return false;
    }

    void set_up_transparent_generic_diagnostic() noexcept {
        if(!transparent_generic_command_registered) {
            add_command_event(transparent_generic_console_command, EVENT_PRIORITY_BEFORE);
            transparent_generic_command_registered = true;
        }
    }

    short rasterizer_dynamic_vertices_get_type(long dynamic_vertex_buffer_index) noexcept {
        if(dynamic_vertex_buffer_index < 0 || !dynamic_vertices || dynamic_vertex_buffer_index >= dynamic_vertices->buffer_count) {
            return -1;
        }
        return dynamic_vertices->buffers[dynamic_vertex_buffer_index].type;
    }

    short rasterizer_transparent_geometry_get_primary_vertex_type(TransparentGeometryGroup *group) noexcept {
        // This helper is used by Chimera's transparent-generic draw path. Returning an
        // invalid type makes that path stop before issuing the draw call, which gives us
        // a narrow diagnostic without touching Halo's index-buffer fix or solid geometry.
        if(!transparent_generic_enabled) {
            return -1;
        }
        if(!group) {
            return -1;
        }
        if(group->vertex_buffers) {
            return group->vertex_buffers[0].type;
        }
        if(group->dynamic_vertex_buffer_index != -1) {
            return rasterizer_dynamic_vertices_get_type(group->dynamic_vertex_buffer_index);
        }
        return -1;
    }

}
