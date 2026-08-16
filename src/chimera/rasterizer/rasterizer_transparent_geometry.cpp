// SPDX-License-Identifier: GPL-3.0-only

#include <cctype>
#include <cstdint>
#include <cstring>

#include "rasterizer_transparent_geometry.hpp"
#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_defs.hpp"
#include "../output/output.hpp"
#include "../event/command.hpp"


namespace Chimera {

    extern DynamicVertices *dynamic_vertices;
    extern "C" void *rasterizer_transparent_geometry_group_draw_func;

    namespace {
        enum class EnvironmentTransparentDiagnosticMode {
            NORMAL,
            ENGINE_OFF,
            GROUP_DRAWS_OFF
        };

        struct TransparentGroupStats {
            std::uint64_t total = 0;
            std::uint64_t shader_types[NUMBER_OF_SHADER_TYPES] = {};
            std::uint64_t shader_other = 0;
            std::uint64_t vertex_types[NUMBER_OF_RASTERIZER_VERTEX_TYPES] = {};
            std::uint64_t vertex_other = 0;
            std::uint64_t static_vertices = 0;
            std::uint64_t dynamic_vertices = 0;
            std::uint64_t no_vertices = 0;
            std::uint64_t static_triangles = 0;
            std::uint64_t dynamic_triangles = 0;
            std::uint64_t no_triangles = 0;
            std::uint64_t owner_and_source_null = 0;
            std::uint64_t owner_null_source_set = 0;
            std::uint64_t owner_set_source_null = 0;
            std::uint64_t owner_and_source_set = 0;
            std::uint64_t no_sort = 0;
            std::uint64_t no_queue = 0;
            std::uint64_t sky = 0;
            std::uint64_t viewspace = 0;
            std::uint64_t first_person = 0;
        };

        static EnvironmentTransparentDiagnosticMode diagnostic_mode = EnvironmentTransparentDiagnosticMode::NORMAL;
        static TransparentGroupStats stats = {};
        static Hook group_draw_hook;
        static const void *group_draw_original = nullptr;
        static bool engine_flag_snapshot_valid = false;
        static bool engine_flag_snapshot = true;

        static const char *diagnostic_mode_name() noexcept {
            switch(diagnostic_mode) {
                case EnvironmentTransparentDiagnosticMode::ENGINE_OFF:
                    return "engine_off";
                case EnvironmentTransparentDiagnosticMode::GROUP_DRAWS_OFF:
                    return "group_draws";
                default:
                    return "normal";
            }
        }

        static const char *shader_type_name(short type) noexcept {
            switch(type) {
                case SHADER_TYPE_SCREEN: return "screen";
                case SHADER_TYPE_EFFECT: return "effect";
                case SHADER_TYPE_DECAL: return "decal";
                case SHADER_TYPE_ENVIRONMENT: return "environment";
                case SHADER_TYPE_MODEL: return "model";
                case SHADER_TYPE_TRANSPARENT_GENERIC: return "generic";
                case SHADER_TYPE_TRANSPARENT_CHICAGO: return "chicago";
                case SHADER_TYPE_TRANSPARENT_CHICAGO_EXTENDED: return "chicago_extended";
                case SHADER_TYPE_TRANSPARENT_WATER: return "water";
                case SHADER_TYPE_TRANSPARENT_GLASS: return "glass";
                case SHADER_TYPE_TRANSPARENT_METER: return "meter";
                case SHADER_TYPE_TRANSPARENT_PLASMA: return "plasma";
                default: return "other";
            }
        }

        static const char *vertex_type_name(short type) noexcept {
            switch(type) {
                case RASTERIZER_VERTEX_TYPE_ENVIRONMENT_UNCOMPRESSED: return "environment_uncompressed";
                case RASTERIZER_VERTEX_TYPE_ENVIRONMENT_COMPRESSED: return "environment_compressed";
                case RASTERIZER_VERTEX_TYPE_ENVIRONMENT_LIGHTMAP_UNCOMPRESSED: return "environment_lightmap_uncompressed";
                case RASTERIZER_VERTEX_TYPE_ENVIRONMENT_LIGHTMAP_COMPRESSED: return "environment_lightmap_compressed";
                case RASTERIZER_VERTEX_TYPE_MODEL_UNCOMPRESSED: return "model_uncompressed";
                case RASTERIZER_VERTEX_TYPE_MODEL_COMPRESSED: return "model_compressed";
                case RASTERIZER_VERTEX_TYPE_DYNAMIC_UNLIT: return "dynamic_unlit";
                case RASTERIZER_VERTEX_TYPE_DYNAMIC_LIT: return "dynamic_lit";
                case RASTERIZER_VERTEX_TYPE_DYNAMIC_SCREEN: return "dynamic_screen";
                case RASTERIZER_VERTEX_TYPE_DEBUG: return "debug";
                case RASTERIZER_VERTEX_TYPE_DECAL: return "decal";
                case RASTERIZER_VERTEX_TYPE_DETAIL_OBJECT: return "detail_object";
                case RASTERIZER_VERTEX_TYPE_ENVIRONMENT_UNCOMPRESSED_FF: return "environment_uncompressed_ff";
                case RASTERIZER_VERTEX_TYPE_ENVIRONMENT_LIGHTMAP_UNCOMPRESSED_FF: return "environment_lightmap_uncompressed_ff";
                case RASTERIZER_VERTEX_TYPE_MODEL_UNCOMPRESSED_FF: return "model_uncompressed_ff";
                case RASTERIZER_VERTEX_TYPE_MODEL_PROCESSED: return "model_processed";
                case RASTERIZER_VERTEX_TYPE_UNLIT_ZSPRITE: return "unlit_zsprite";
                case RASTERIZER_VERTEX_TYPE_SCREEN_TRANSFORMED_LIT: return "screen_transformed_lit";
                case RASTERIZER_VERTEX_TYPE_SCREEN_TRANSFORMED_LIT_SPECULAR: return "screen_transformed_lit_specular";
                case RASTERIZER_VERTEX_TYPE_ENVIRONMENT_SINGLE_STREAM_FF: return "environment_single_stream_ff";
                default: return "other";
            }
        }

        static void reset_stats() noexcept {
            std::memset(&stats, 0, sizeof(stats));
        }

        static void record_group(TransparentGeometryGroup *group) noexcept {
            stats.total++;
            if(!group) {
                stats.shader_other++;
                stats.vertex_other++;
                stats.no_vertices++;
                stats.no_triangles++;
                return;
            }

            short shader_type = -1;
            if(group->shader) {
                shader_type = reinterpret_cast<_shader *>(group->shader)->type;
            }
            if(shader_type >= 0 && shader_type < NUMBER_OF_SHADER_TYPES) {
                stats.shader_types[shader_type]++;
            }
            else {
                stats.shader_other++;
            }

            short vertex_type = rasterizer_transparent_geometry_get_primary_vertex_type(group);
            if(vertex_type >= 0 && vertex_type < NUMBER_OF_RASTERIZER_VERTEX_TYPES) {
                stats.vertex_types[vertex_type]++;
            }
            else {
                stats.vertex_other++;
            }

            if(group->dynamic_vertex_buffer_index != -1) {
                stats.dynamic_vertices++;
            }
            else if(group->vertex_buffers) {
                stats.static_vertices++;
            }
            else {
                stats.no_vertices++;
            }

            if(group->dynamic_triangle_buffer_index != -1) {
                stats.dynamic_triangles++;
            }
            else if(group->triangle_buffer) {
                stats.static_triangles++;
            }
            else {
                stats.no_triangles++;
            }

            const bool owner_null = group->object_index.is_null();
            const bool source_null = group->source_object_index.is_null();
            if(owner_null && source_null) {
                stats.owner_and_source_null++;
            }
            else if(owner_null) {
                stats.owner_null_source_set++;
            }
            else if(source_null) {
                stats.owner_set_source_null++;
            }
            else {
                stats.owner_and_source_set++;
            }

            if(TEST_FLAG(group->geometry_flags, RASTERIZER_GEOMETRY_FLAGS_NO_SORT_BIT)) stats.no_sort++;
            if(TEST_FLAG(group->geometry_flags, RASTERIZER_GEOMETRY_FLAGS_NO_QUEUE_BIT)) stats.no_queue++;
            if(TEST_FLAG(group->geometry_flags, RASTERIZER_GEOMETRY_FLAGS_SKY_BIT)) stats.sky++;
            if(TEST_FLAG(group->geometry_flags, RASTERIZER_GEOMETRY_FLAGS_VIEWSPACE_BIT)) stats.viewspace++;
            if(TEST_FLAG(group->geometry_flags, RASTERIZER_GEOMETRY_FLAGS_FIRST_PERSON_BIT)) stats.first_person++;
        }

        static void print_stats() noexcept {
            console_output("chimera_debug_environment_transparent: %s", diagnostic_mode_name());
            console_output("group_draw_calls=%llu", static_cast<unsigned long long>(stats.total));
            console_output("vertices: static=%llu dynamic=%llu none=%llu",
                static_cast<unsigned long long>(stats.static_vertices),
                static_cast<unsigned long long>(stats.dynamic_vertices),
                static_cast<unsigned long long>(stats.no_vertices));
            console_output("triangles: static=%llu dynamic=%llu none=%llu",
                static_cast<unsigned long long>(stats.static_triangles),
                static_cast<unsigned long long>(stats.dynamic_triangles),
                static_cast<unsigned long long>(stats.no_triangles));
            console_output("owners: both_null=%llu owner_null=%llu source_null=%llu both_set=%llu",
                static_cast<unsigned long long>(stats.owner_and_source_null),
                static_cast<unsigned long long>(stats.owner_null_source_set),
                static_cast<unsigned long long>(stats.owner_set_source_null),
                static_cast<unsigned long long>(stats.owner_and_source_set));
            console_output("flags: no_sort=%llu no_queue=%llu sky=%llu viewspace=%llu first_person=%llu",
                static_cast<unsigned long long>(stats.no_sort),
                static_cast<unsigned long long>(stats.no_queue),
                static_cast<unsigned long long>(stats.sky),
                static_cast<unsigned long long>(stats.viewspace),
                static_cast<unsigned long long>(stats.first_person));

            for(short type = 0; type < NUMBER_OF_SHADER_TYPES; type++) {
                if(stats.shader_types[type]) {
                    console_output("shader.%s=%llu", shader_type_name(type), static_cast<unsigned long long>(stats.shader_types[type]));
                }
            }
            if(stats.shader_other) {
                console_output("shader.other=%llu", static_cast<unsigned long long>(stats.shader_other));
            }

            for(short type = 0; type < NUMBER_OF_RASTERIZER_VERTEX_TYPES; type++) {
                if(stats.vertex_types[type]) {
                    console_output("vertex.%s=%llu", vertex_type_name(type), static_cast<unsigned long long>(stats.vertex_types[type]));
                }
            }
            if(stats.vertex_other) {
                console_output("vertex.other=%llu", static_cast<unsigned long long>(stats.vertex_other));
            }
        }

        static void restore_engine_flag() noexcept {
            if(engine_flag_snapshot_valid && rasterizer_debug_options) {
                rasterizer_debug_options->draw_environment_transparent_geometry = engine_flag_snapshot;
            }
            engine_flag_snapshot_valid = false;
        }

        static void group_draw_diagnostic(TransparentGeometryGroup *group, bool is_dirty) noexcept {
            using GroupDrawFunction = void (*)(TransparentGeometryGroup *, bool);
            auto original = reinterpret_cast<GroupDrawFunction>(const_cast<void *>(group_draw_original));
            if(!original) {
                return;
            }

            record_group(group);
            if(diagnostic_mode == EnvironmentTransparentDiagnosticMode::GROUP_DRAWS_OFF) {
                return;
            }

            original(group, is_dirty);
        }

        static bool environment_transparent_diagnostic_command(const char *command) noexcept {
            if(!command) {
                return true;
            }

            static constexpr char command_name[] = "chimera_debug_environment_transparent";
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

            if(*argument == '\0' || std::strcmp(argument, "stats") == 0) {
                print_stats();
                console_output("modes: normal engine_off group_draws stats reset");
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
                console_error("chimera_debug_environment_transparent: expected one mode");
                return false;
            }

            auto matches = [&](const char *value) noexcept {
                return std::strlen(value) == argument_length && std::strncmp(argument, value, argument_length) == 0;
            };

            if(matches("reset")) {
                reset_stats();
                console_output("chimera_debug_environment_transparent: counters reset");
                return false;
            }

            EnvironmentTransparentDiagnosticMode new_mode;
            if(matches("normal")) {
                new_mode = EnvironmentTransparentDiagnosticMode::NORMAL;
            }
            else if(matches("engine_off")) {
                new_mode = EnvironmentTransparentDiagnosticMode::ENGINE_OFF;
            }
            else if(matches("group_draws")) {
                new_mode = EnvironmentTransparentDiagnosticMode::GROUP_DRAWS_OFF;
            }
            else {
                console_error("chimera_debug_environment_transparent: unknown mode");
                console_output("modes: normal engine_off group_draws stats reset");
                return false;
            }

            if(new_mode == EnvironmentTransparentDiagnosticMode::ENGINE_OFF && !rasterizer_debug_options) {
                console_error("chimera_debug_environment_transparent: rasterizer debug options unavailable");
                return false;
            }

            restore_engine_flag();
            diagnostic_mode = new_mode;

            if(new_mode == EnvironmentTransparentDiagnosticMode::ENGINE_OFF) {
                engine_flag_snapshot = rasterizer_debug_options->draw_environment_transparent_geometry;
                engine_flag_snapshot_valid = true;
                rasterizer_debug_options->draw_environment_transparent_geometry = false;
            }

            reset_stats();
            console_output("chimera_debug_environment_transparent: %s", diagnostic_mode_name());
            return false;
        }
    }

    short rasterizer_dynamic_vertices_get_type(long dynamic_vertex_buffer_index) noexcept {
        if(dynamic_vertex_buffer_index < 0 || !dynamic_vertices || dynamic_vertex_buffer_index >= dynamic_vertices->buffer_count) {
            return -1;
        }
        return dynamic_vertices->buffers[dynamic_vertex_buffer_index].type;
    }

    short rasterizer_transparent_geometry_get_primary_vertex_type(TransparentGeometryGroup *group) noexcept {
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

    void set_up_environment_transparent_diagnostic() noexcept {
        static bool installed = false;
        if(installed || !rasterizer_transparent_geometry_group_draw_func) {
            return;
        }

        write_function_override(
            rasterizer_transparent_geometry_group_draw_func,
            group_draw_hook,
            reinterpret_cast<const void *>(group_draw_diagnostic),
            &group_draw_original
        );
        add_command_event(environment_transparent_diagnostic_command, EVENT_PRIORITY_BEFORE);
        installed = group_draw_original != nullptr;
    }

}
