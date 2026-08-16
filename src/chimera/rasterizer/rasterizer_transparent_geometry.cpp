// SPDX-License-Identifier: GPL-3.0-only

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
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
        constexpr long TRACKED_DYNAMIC_TRIANGLE_SLOTS = 512;
        constexpr std::size_t TOP_SLOT_COUNT = 8;

        enum class ChicagoTriangleDiagnosticMode {
            NORMAL,
            SKIP_ALL,
            SKIP_SLOT
        };

        struct ChicagoTriangleSlotStats {
            bool initialized = false;
            bool range_initialized = false;
            bool last_range_valid = false;
            std::uint64_t calls = 0;
            std::uint64_t frames_seen = 0;
            std::uint64_t same_frame_calls = 0;
            std::uint64_t same_frame_range_changes = 0;
            std::uint64_t same_frame_overlaps_previous = 0;
            std::int64_t last_frame = -1;
            long last_first_triangle = 0;
            long last_triangle_count = 0;
            long min_first_triangle = 0;
            long max_first_triangle = 0;
            long min_triangle_count = 0;
            long max_triangle_count = 0;
        };

        struct ChicagoTriangleStats {
            std::uint64_t group_draw_calls = 0;
            std::uint64_t suspect_calls = 0;
            std::uint64_t skipped = 0;

            bool slot_range_initialized = false;
            long min_slot = 0;
            long max_slot = 0;
            std::uint64_t tracked_slots_seen = 0;
            std::uint64_t slot_overflow_calls = 0;
            std::uint64_t invalid_negative_slot_calls = 0;

            bool triangle_range_initialized = false;
            long min_first_triangle = 0;
            long max_first_triangle = 0;
            long min_triangle_count = 0;
            long max_triangle_count = 0;
            std::int64_t max_triangle_end = 0;
            std::uint64_t negative_first_triangle_calls = 0;
            std::uint64_t nonpositive_triangle_count_calls = 0;

            std::uint64_t frame_unavailable_calls = 0;
            std::uint64_t same_slot_same_frame_calls = 0;
            std::uint64_t same_slot_same_frame_range_changes = 0;
            std::uint64_t same_slot_same_frame_overlaps_previous = 0;

            ChicagoTriangleSlotStats slots[TRACKED_DYNAMIC_TRIANGLE_SLOTS] = {};
        };

        static ChicagoTriangleDiagnosticMode diagnostic_mode = ChicagoTriangleDiagnosticMode::NORMAL;
        static long selected_slot = -1;
        static ChicagoTriangleStats stats = {};
        static Hook group_draw_hook;
        static const void *group_draw_original = nullptr;

        static bool token_equals(const char *token, std::size_t token_length, const char *value) noexcept {
            return std::strlen(value) == token_length && std::strncmp(token, value, token_length) == 0;
        }

        static bool is_suspect_chicago_group(TransparentGeometryGroup *group) noexcept {
            if(!group || group->dynamic_triangle_buffer_index == -1 || !group->shader) {
                return false;
            }

            if(reinterpret_cast<_shader *>(group->shader)->type != SHADER_TYPE_TRANSPARENT_CHICAGO) {
                return false;
            }

            return rasterizer_transparent_geometry_get_primary_vertex_type(group)
                == RASTERIZER_VERTEX_TYPE_ENVIRONMENT_UNCOMPRESSED;
        }

        static void reset_stats() noexcept {
            stats = {};
        }

        static void update_global_triangle_range(long first_triangle, long triangle_count) noexcept {
            if(first_triangle < 0) {
                stats.negative_first_triangle_calls++;
            }
            if(triangle_count <= 0) {
                stats.nonpositive_triangle_count_calls++;
            }
            if(first_triangle < 0 || triangle_count <= 0) {
                return;
            }

            const std::int64_t triangle_end = static_cast<std::int64_t>(first_triangle)
                + static_cast<std::int64_t>(triangle_count);

            if(!stats.triangle_range_initialized) {
                stats.triangle_range_initialized = true;
                stats.min_first_triangle = first_triangle;
                stats.max_first_triangle = first_triangle;
                stats.min_triangle_count = triangle_count;
                stats.max_triangle_count = triangle_count;
                stats.max_triangle_end = triangle_end;
            }
            else {
                if(first_triangle < stats.min_first_triangle) stats.min_first_triangle = first_triangle;
                if(first_triangle > stats.max_first_triangle) stats.max_first_triangle = first_triangle;
                if(triangle_count < stats.min_triangle_count) stats.min_triangle_count = triangle_count;
                if(triangle_count > stats.max_triangle_count) stats.max_triangle_count = triangle_count;
                if(triangle_end > stats.max_triangle_end) stats.max_triangle_end = triangle_end;
            }
        }

        static void record_slot(long slot, long first_triangle, long triangle_count) noexcept {
            if(slot < 0) {
                stats.invalid_negative_slot_calls++;
                return;
            }

            if(!stats.slot_range_initialized) {
                stats.slot_range_initialized = true;
                stats.min_slot = slot;
                stats.max_slot = slot;
            }
            else {
                if(slot < stats.min_slot) stats.min_slot = slot;
                if(slot > stats.max_slot) stats.max_slot = slot;
            }

            if(slot >= TRACKED_DYNAMIC_TRIANGLE_SLOTS) {
                stats.slot_overflow_calls++;
                return;
            }

            auto &slot_stats = stats.slots[slot];
            const std::int64_t frame = rasterizer_globals ? rasterizer_globals->frame_index : -1;
            const bool current_range_valid = first_triangle >= 0 && triangle_count > 0;
            const bool same_frame = slot_stats.initialized && frame >= 0 && slot_stats.last_frame == frame;

            if(!slot_stats.initialized) {
                slot_stats.initialized = true;
                stats.tracked_slots_seen++;
            }

            slot_stats.calls++;

            if(frame < 0) {
                stats.frame_unavailable_calls++;
            }
            else if(slot_stats.last_frame != frame) {
                slot_stats.frames_seen++;
            }

            if(same_frame) {
                slot_stats.same_frame_calls++;
                stats.same_slot_same_frame_calls++;

                if(slot_stats.last_first_triangle != first_triangle || slot_stats.last_triangle_count != triangle_count) {
                    slot_stats.same_frame_range_changes++;
                    stats.same_slot_same_frame_range_changes++;
                }

                if(current_range_valid && slot_stats.last_range_valid) {
                    const std::int64_t current_start = first_triangle;
                    const std::int64_t current_end = current_start + triangle_count;
                    const std::int64_t previous_start = slot_stats.last_first_triangle;
                    const std::int64_t previous_end = previous_start + slot_stats.last_triangle_count;
                    if(current_start < previous_end && previous_start < current_end) {
                        slot_stats.same_frame_overlaps_previous++;
                        stats.same_slot_same_frame_overlaps_previous++;
                    }
                }
            }

            if(current_range_valid) {
                if(!slot_stats.range_initialized) {
                    slot_stats.range_initialized = true;
                    slot_stats.min_first_triangle = first_triangle;
                    slot_stats.max_first_triangle = first_triangle;
                    slot_stats.min_triangle_count = triangle_count;
                    slot_stats.max_triangle_count = triangle_count;
                }
                else {
                    if(first_triangle < slot_stats.min_first_triangle) slot_stats.min_first_triangle = first_triangle;
                    if(first_triangle > slot_stats.max_first_triangle) slot_stats.max_first_triangle = first_triangle;
                    if(triangle_count < slot_stats.min_triangle_count) slot_stats.min_triangle_count = triangle_count;
                    if(triangle_count > slot_stats.max_triangle_count) slot_stats.max_triangle_count = triangle_count;
                }
            }

            slot_stats.last_frame = frame;
            slot_stats.last_first_triangle = first_triangle;
            slot_stats.last_triangle_count = triangle_count;
            slot_stats.last_range_valid = current_range_valid;
        }

        static void record_suspect_group(TransparentGeometryGroup *group) noexcept {
            stats.suspect_calls++;

            const long slot = group->dynamic_triangle_buffer_index;
            const long first_triangle = group->first_triangle_index;
            const long triangle_count = group->triangle_count;

            update_global_triangle_range(first_triangle, triangle_count);
            record_slot(slot, first_triangle, triangle_count);
        }

        static bool should_skip_suspect_group(TransparentGeometryGroup *group) noexcept {
            switch(diagnostic_mode) {
                case ChicagoTriangleDiagnosticMode::SKIP_ALL:
                    return true;
                case ChicagoTriangleDiagnosticMode::SKIP_SLOT:
                    return group && group->dynamic_triangle_buffer_index == selected_slot;
                default:
                    return false;
            }
        }

        static void print_top_slots() noexcept {
            long top_slots[TOP_SLOT_COUNT];
            std::uint64_t top_calls[TOP_SLOT_COUNT] = {};
            for(std::size_t i = 0; i < TOP_SLOT_COUNT; i++) {
                top_slots[i] = -1;
            }

            for(long slot = 0; slot < TRACKED_DYNAMIC_TRIANGLE_SLOTS; slot++) {
                const auto calls = stats.slots[slot].calls;
                if(calls == 0) {
                    continue;
                }

                for(std::size_t position = 0; position < TOP_SLOT_COUNT; position++) {
                    if(calls > top_calls[position]) {
                        for(std::size_t move = TOP_SLOT_COUNT - 1; move > position; move--) {
                            top_calls[move] = top_calls[move - 1];
                            top_slots[move] = top_slots[move - 1];
                        }
                        top_calls[position] = calls;
                        top_slots[position] = slot;
                        break;
                    }
                }
            }

            for(std::size_t i = 0; i < TOP_SLOT_COUNT; i++) {
                const long slot = top_slots[i];
                if(slot < 0) {
                    break;
                }

                const auto &slot_stats = stats.slots[slot];
                if(slot_stats.range_initialized) {
                    console_output("slot.%ld calls=%llu frames=%llu same_frame=%llu changes=%llu overlaps=%llu first=%ld..%ld count=%ld..%ld",
                        slot,
                        static_cast<unsigned long long>(slot_stats.calls),
                        static_cast<unsigned long long>(slot_stats.frames_seen),
                        static_cast<unsigned long long>(slot_stats.same_frame_calls),
                        static_cast<unsigned long long>(slot_stats.same_frame_range_changes),
                        static_cast<unsigned long long>(slot_stats.same_frame_overlaps_previous),
                        slot_stats.min_first_triangle,
                        slot_stats.max_first_triangle,
                        slot_stats.min_triangle_count,
                        slot_stats.max_triangle_count);
                }
                else {
                    console_output("slot.%ld calls=%llu frames=%llu same_frame=%llu changes=%llu overlaps=%llu no_valid_range",
                        slot,
                        static_cast<unsigned long long>(slot_stats.calls),
                        static_cast<unsigned long long>(slot_stats.frames_seen),
                        static_cast<unsigned long long>(slot_stats.same_frame_calls),
                        static_cast<unsigned long long>(slot_stats.same_frame_range_changes),
                        static_cast<unsigned long long>(slot_stats.same_frame_overlaps_previous));
                }
            }
        }

        static void print_stats() noexcept {
            if(diagnostic_mode == ChicagoTriangleDiagnosticMode::SKIP_SLOT) {
                console_output("chimera_debug_chicago_triangle: slot %ld", selected_slot);
            }
            else {
                console_output("chimera_debug_chicago_triangle: %s",
                    diagnostic_mode == ChicagoTriangleDiagnosticMode::SKIP_ALL ? "skip" : "normal");
            }

            console_output("group_draw_calls=%llu suspect_calls=%llu skipped=%llu",
                static_cast<unsigned long long>(stats.group_draw_calls),
                static_cast<unsigned long long>(stats.suspect_calls),
                static_cast<unsigned long long>(stats.skipped));

            if(stats.slot_range_initialized) {
                console_output("slots: min=%ld max=%ld tracked=%llu overflow_calls=%llu invalid_negative=%llu",
                    stats.min_slot,
                    stats.max_slot,
                    static_cast<unsigned long long>(stats.tracked_slots_seen),
                    static_cast<unsigned long long>(stats.slot_overflow_calls),
                    static_cast<unsigned long long>(stats.invalid_negative_slot_calls));
            }
            else {
                console_output("slots: none tracked=%llu overflow_calls=%llu invalid_negative=%llu",
                    static_cast<unsigned long long>(stats.tracked_slots_seen),
                    static_cast<unsigned long long>(stats.slot_overflow_calls),
                    static_cast<unsigned long long>(stats.invalid_negative_slot_calls));
            }

            if(stats.triangle_range_initialized) {
                console_output("ranges: first=%ld..%ld count=%ld..%ld max_end=%lld negative_first=%llu nonpositive_count=%llu",
                    stats.min_first_triangle,
                    stats.max_first_triangle,
                    stats.min_triangle_count,
                    stats.max_triangle_count,
                    static_cast<long long>(stats.max_triangle_end),
                    static_cast<unsigned long long>(stats.negative_first_triangle_calls),
                    static_cast<unsigned long long>(stats.nonpositive_triangle_count_calls));
            }
            else {
                console_output("ranges: no_valid_range negative_first=%llu nonpositive_count=%llu",
                    static_cast<unsigned long long>(stats.negative_first_triangle_calls),
                    static_cast<unsigned long long>(stats.nonpositive_triangle_count_calls));
            }

            console_output("reuse_heuristic: same_slot_same_frame=%llu range_changes=%llu overlaps_previous=%llu frame_unavailable=%llu",
                static_cast<unsigned long long>(stats.same_slot_same_frame_calls),
                static_cast<unsigned long long>(stats.same_slot_same_frame_range_changes),
                static_cast<unsigned long long>(stats.same_slot_same_frame_overlaps_previous),
                static_cast<unsigned long long>(stats.frame_unavailable_calls));

            print_top_slots();
        }

        static void print_usage() noexcept {
            console_output("modes: normal skip slot <index> stats reset");
        }

        static void group_draw_diagnostic(TransparentGeometryGroup *group, bool is_dirty) noexcept {
            using GroupDrawFunction = void (*)(TransparentGeometryGroup *, bool);
            auto original = reinterpret_cast<GroupDrawFunction>(const_cast<void *>(group_draw_original));
            if(!original) {
                return;
            }

            stats.group_draw_calls++;

            if(is_suspect_chicago_group(group)) {
                record_suspect_group(group);
                if(should_skip_suspect_group(group)) {
                    stats.skipped++;
                    return;
                }
            }

            original(group, is_dirty);
        }

        static bool chicago_triangle_diagnostic_command(const char *command) noexcept {
            if(!command) {
                return true;
            }

            static constexpr char command_name[] = "chimera_debug_chicago_triangle";
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
                print_stats();
                print_usage();
                return false;
            }

            const char *token_end = argument;
            while(*token_end != '\0' && !std::isspace(static_cast<unsigned char>(*token_end))) {
                token_end++;
            }
            const std::size_t token_length = static_cast<std::size_t>(token_end - argument);

            const char *rest = token_end;
            while(std::isspace(static_cast<unsigned char>(*rest))) {
                rest++;
            }

            if(token_equals(argument, token_length, "stats")) {
                if(*rest != '\0') {
                    console_error("chimera_debug_chicago_triangle: stats takes no arguments");
                }
                else {
                    print_stats();
                }
                return false;
            }

            if(token_equals(argument, token_length, "reset")) {
                if(*rest != '\0') {
                    console_error("chimera_debug_chicago_triangle: reset takes no arguments");
                }
                else {
                    reset_stats();
                    console_output("chimera_debug_chicago_triangle: counters reset");
                }
                return false;
            }

            if(token_equals(argument, token_length, "normal") || token_equals(argument, token_length, "skip")) {
                if(*rest != '\0') {
                    console_error("chimera_debug_chicago_triangle: mode takes no extra arguments");
                    return false;
                }

                diagnostic_mode = token_equals(argument, token_length, "skip")
                    ? ChicagoTriangleDiagnosticMode::SKIP_ALL
                    : ChicagoTriangleDiagnosticMode::NORMAL;
                selected_slot = -1;
                reset_stats();
                console_output("chimera_debug_chicago_triangle: %s",
                    diagnostic_mode == ChicagoTriangleDiagnosticMode::SKIP_ALL ? "skip" : "normal");
                return false;
            }

            if(token_equals(argument, token_length, "slot")) {
                if(*rest == '\0') {
                    console_error("chimera_debug_chicago_triangle: slot requires an index");
                    return false;
                }

                errno = 0;
                char *number_end = nullptr;
                const long slot = std::strtol(rest, &number_end, 10);
                if(rest == number_end || errno == ERANGE || slot < 0) {
                    console_error("chimera_debug_chicago_triangle: invalid slot index");
                    return false;
                }
                while(std::isspace(static_cast<unsigned char>(*number_end))) {
                    number_end++;
                }
                if(*number_end != '\0') {
                    console_error("chimera_debug_chicago_triangle: expected one slot index");
                    return false;
                }

                diagnostic_mode = ChicagoTriangleDiagnosticMode::SKIP_SLOT;
                selected_slot = slot;
                reset_stats();
                console_output("chimera_debug_chicago_triangle: slot %ld", selected_slot);
                return false;
            }

            console_error("chimera_debug_chicago_triangle: unknown mode");
            print_usage();
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
        add_command_event(chicago_triangle_diagnostic_command, EVENT_PRIORITY_BEFORE);
        installed = group_draw_original != nullptr;
    }

}
