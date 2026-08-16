// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <cstring>

#include "rasterizer_transparent_geometry.hpp"
#include "../event/command.hpp"
#include "../halo_data/game_variables.hpp"
#include "../output/output.hpp"

namespace Chimera {

    extern DynamicVertices *dynamic_vertices;

    enum TransparentGeometryRejectReason : std::uint8_t {
        TRANSPARENT_GEOMETRY_REJECT_NONE,
        TRANSPARENT_GEOMETRY_REJECT_NULL_GROUP,
        TRANSPARENT_GEOMETRY_REJECT_TRIANGLE_RANGE,
        TRANSPARENT_GEOMETRY_REJECT_VERTEX_INDEX,
        TRANSPARENT_GEOMETRY_REJECT_VERTEX_RANGE,
        TRANSPARENT_GEOMETRY_REJECT_VERTEX_TYPE,
        NUMBER_OF_TRANSPARENT_GEOMETRY_REJECT_REASONS
    };

    struct TransparentGeometryGuardStats {
        std::uint32_t checked = 0;
        std::uint32_t rejected = 0;
        std::uint32_t reasons[NUMBER_OF_TRANSPARENT_GEOMETRY_REJECT_REASONS] = {};

        long dynamic_triangle_buffer_index = -1;
        long first_triangle_index = 0;
        long triangle_count = 0;
        long dynamic_vertex_buffer_index = -1;
        long vertex_start_index = 0;
        long vertex_count = 0;
        short vertex_type = -1;
    };

    static TransparentGeometryGuardStats geometry_guard_stats{};
    static bool geometry_guard_command_registered = false;

    static bool valid_vertex_type(short type) noexcept {
        return type >= 0 && type < NUMBER_OF_RASTERIZER_VERTEX_TYPES;
    }

    static bool reject_geometry(TransparentGeometryRejectReason reason, TransparentGeometryGroup *group,
                                const DynamicVertexBuffer *dynamic_vertex_buffer = nullptr) noexcept {
        geometry_guard_stats.rejected++;
        geometry_guard_stats.reasons[reason]++;

        if(group) {
            geometry_guard_stats.dynamic_triangle_buffer_index = group->dynamic_triangle_buffer_index;
            geometry_guard_stats.first_triangle_index = group->first_triangle_index;
            geometry_guard_stats.triangle_count = group->triangle_count;
            geometry_guard_stats.dynamic_vertex_buffer_index = group->dynamic_vertex_buffer_index;
        }

        if(dynamic_vertex_buffer) {
            geometry_guard_stats.vertex_start_index = dynamic_vertex_buffer->vertex_start_index;
            geometry_guard_stats.vertex_count = dynamic_vertex_buffer->vertex_count;
            geometry_guard_stats.vertex_type = dynamic_vertex_buffer->type;
        }
        else if(group && group->vertex_buffers) {
            geometry_guard_stats.vertex_start_index = group->vertex_buffers[0].offset;
            geometry_guard_stats.vertex_count = group->vertex_buffers[0].count;
            geometry_guard_stats.vertex_type = group->vertex_buffers[0].type;
        }
        else {
            geometry_guard_stats.vertex_start_index = 0;
            geometry_guard_stats.vertex_count = 0;
            geometry_guard_stats.vertex_type = -1;
        }

        return false;
    }

    extern "C" bool rasterizer_validate_transparent_geometry_group(TransparentGeometryGroup *group) noexcept {
        geometry_guard_stats.checked++;

        if(!group) {
            return reject_geometry(TRANSPARENT_GEOMETRY_REJECT_NULL_GROUP, nullptr);
        }

        // Negative draw ranges are never valid. Zero triangles are harmless and are
        // left to Halo so we do not change benign edge-case behavior.
        if(group->first_triangle_index < 0 || group->triangle_count < 0) {
            return reject_geometry(TRANSPARENT_GEOMETRY_REJECT_TRIANGLE_RANGE, group);
        }

        // Static triangle buffers expose their count directly, so reject ranges that
        // would run past the buffer. Dynamic triangle-buffer capacity is not exposed
        // by Chimera yet, so do not guess a limit for that path.
        if(group->triangle_buffer) {
            const auto buffer_count = group->triangle_buffer->count;
            if(buffer_count < 0 || group->first_triangle_index > buffer_count ||
               group->triangle_count > buffer_count - group->first_triangle_index) {
                return reject_geometry(TRANSPARENT_GEOMETRY_REJECT_TRIANGLE_RANGE, group);
            }
        }

        if(group->vertex_buffers) {
            const auto &vertex_buffer = group->vertex_buffers[0];
            if(!valid_vertex_type(vertex_buffer.type)) {
                return reject_geometry(TRANSPARENT_GEOMETRY_REJECT_VERTEX_TYPE, group);
            }
            if(vertex_buffer.count < 0 || vertex_buffer.offset < 0) {
                return reject_geometry(TRANSPARENT_GEOMETRY_REJECT_VERTEX_RANGE, group);
            }
            return true;
        }

        if(group->dynamic_vertex_buffer_index == -1) {
            // Some special paths may not use a normal vertex buffer. Preserve Halo's
            // original behavior instead of rejecting an unknown-but-possibly-valid case.
            return true;
        }

        if(!dynamic_vertices || dynamic_vertices->buffer_count < 0 || dynamic_vertices->buffer_count > 1024 ||
           group->dynamic_vertex_buffer_index < 0 || group->dynamic_vertex_buffer_index >= dynamic_vertices->buffer_count) {
            return reject_geometry(TRANSPARENT_GEOMETRY_REJECT_VERTEX_INDEX, group);
        }

        const auto &dynamic_vertex_buffer = dynamic_vertices->buffers[group->dynamic_vertex_buffer_index];
        if(!valid_vertex_type(dynamic_vertex_buffer.type)) {
            return reject_geometry(TRANSPARENT_GEOMETRY_REJECT_VERTEX_TYPE, group, &dynamic_vertex_buffer);
        }
        if(dynamic_vertex_buffer.vertex_start_index < 0 || dynamic_vertex_buffer.vertex_count < 0) {
            return reject_geometry(TRANSPARENT_GEOMETRY_REJECT_VERTEX_RANGE, group, &dynamic_vertex_buffer);
        }

        const auto &vertex_group = dynamic_vertices->groups[dynamic_vertex_buffer.type];
        if(vertex_group.max_vertex_count < 0 || dynamic_vertex_buffer.vertex_start_index > vertex_group.max_vertex_count ||
           dynamic_vertex_buffer.vertex_count > vertex_group.max_vertex_count - dynamic_vertex_buffer.vertex_start_index) {
            return reject_geometry(TRANSPARENT_GEOMETRY_REJECT_VERTEX_RANGE, group, &dynamic_vertex_buffer);
        }

        return true;
    }

    static bool transparent_geometry_guard_console_command(const char *command) noexcept {
        if(!command) {
            return true;
        }

        if(std::strcmp(command, "chimera_debug_geometry_guard reset") == 0) {
            geometry_guard_stats = {};
            geometry_guard_stats.dynamic_triangle_buffer_index = -1;
            geometry_guard_stats.dynamic_vertex_buffer_index = -1;
            geometry_guard_stats.vertex_type = -1;
            console_output("chimera_debug_geometry_guard: reset");
            return false;
        }

        if(std::strcmp(command, "chimera_debug_geometry_guard") != 0) {
            return true;
        }

        console_output("chimera_debug_geometry_guard: checked=%lu rejected=%lu",
                       static_cast<unsigned long>(geometry_guard_stats.checked),
                       static_cast<unsigned long>(geometry_guard_stats.rejected));
        console_output("triangle_range=%lu vertex_index=%lu vertex_range=%lu vertex_type=%lu",
                       static_cast<unsigned long>(geometry_guard_stats.reasons[TRANSPARENT_GEOMETRY_REJECT_TRIANGLE_RANGE]),
                       static_cast<unsigned long>(geometry_guard_stats.reasons[TRANSPARENT_GEOMETRY_REJECT_VERTEX_INDEX]),
                       static_cast<unsigned long>(geometry_guard_stats.reasons[TRANSPARENT_GEOMETRY_REJECT_VERTEX_RANGE]),
                       static_cast<unsigned long>(geometry_guard_stats.reasons[TRANSPARENT_GEOMETRY_REJECT_VERTEX_TYPE]));

        if(geometry_guard_stats.rejected > 0) {
            console_output("last: dvb=%ld start=%ld count=%ld type=%d dtb=%ld first=%ld triangles=%ld",
                           geometry_guard_stats.dynamic_vertex_buffer_index,
                           geometry_guard_stats.vertex_start_index,
                           geometry_guard_stats.vertex_count,
                           static_cast<int>(geometry_guard_stats.vertex_type),
                           geometry_guard_stats.dynamic_triangle_buffer_index,
                           geometry_guard_stats.first_triangle_index,
                           geometry_guard_stats.triangle_count);
        }

        return false;
    }

    void set_up_transparent_geometry_guard_diagnostics() noexcept {
        if(!geometry_guard_command_registered) {
            add_command_event(transparent_geometry_guard_console_command, EVENT_PRIORITY_BEFORE);
            geometry_guard_command_registered = true;
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

}
