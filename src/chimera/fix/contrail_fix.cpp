// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../halo_data/object.hpp"
#include "../halo_data/contrail.hpp"
#include "../event/tick.hpp"
#include "../event/revert.hpp"

extern "C" {
    const void *original_contrail_update_function;
    const void *original_instruction;
    std::byte *skip_update = nullptr;
    std::uint32_t can_update_contrail = 0;
    std::uint32_t apply_interpolation_hack = 1;
    float update_contrail_by = 1.0F / 30.0F;
    void new_contrail_update_function();
    void interpolation_memes();
}

namespace Chimera {
    #define CONTRAIL_BUFFER_SIZE 256
    struct ContrailParent {
        /** Roll back contrail parent object's position data to previous ticks value.*/
        bool rollback = false;

        /** Object ID of the parent object. */
        ObjectID object_id;

        /** Tag ID of the parent object. */
        TagID tag_id;

        /** This is the position of the object's center. */
        Point3D center;

        /** This is the number of nodes this object has. */
        std::size_t node_count;

        /** These are the model nodes used by the object. */
        ModelNode nodes[MAX_NODES];
    };

    // This is the contrail parent object data.
    static ContrailParent object_buffers[2][CONTRAIL_BUFFER_SIZE] = {};

    // These are pointers to each buffer. These swap every tick.
    static auto *current_tick = object_buffers[0];
    static auto *previous_tick = object_buffers[1];

    // If true, a tick has passed and it's time to re-copy the object data.
    static bool tick_passed = false;
    extern bool interpolation_enabled;

    static void copy_objects() noexcept;

    void fix_contrail_before() noexcept {
        if(!interpolation_enabled) {
            return;
        }

        // Check if a tick has passed. If so, swap buffers and copy new objects.
        if(tick_passed) {
            if(current_tick == object_buffers[0]) {
                current_tick = object_buffers[1];
                previous_tick = object_buffers[0];
            }
            else {
                current_tick = object_buffers[0];
                previous_tick = object_buffers[1];
            }

            copy_objects();
            tick_passed = false;
        }

        auto &object_table = ObjectTable::get_object_table();
        auto &contrail_table = ContrailTable::get_contrail_table();
        auto max_objects = contrail_table.current_size;

        for(std::size_t i = 0; i < max_objects && i < CONTRAIL_BUFFER_SIZE; i++) {
            auto &current_tick_object = current_tick[i];
            auto &previous_tick_object = previous_tick[i];

            // Skip if we don't want to bodge projectile positions this frame.
            if(!current_tick_object.rollback || !previous_tick_object.rollback) {
                continue;
            }

            // Never read beyond either fixed-size node snapshot.
            if(current_tick_object.node_count > MAX_NODES || previous_tick_object.node_count > MAX_NODES) {
                continue;
            }

            auto *object = object_table.get_dynamic_object(current_tick_object.object_id);
            if(!object) {
                continue;
            }

            auto *nodes = object->nodes();
            if(!nodes) {
                continue;
            }

            // Skip if the tags do not match.
            auto &tag_id = object->definition_index;
            if(tag_id != current_tick_object.tag_id || previous_tick_object.tag_id != tag_id) {
                continue;
            }

            // Skip if object IDs do not match.
            if(current_tick_object.object_id != previous_tick_object.object_id) {
                continue;
            }

            // Copy previous tick positions to object table to fudge contrails.
            object->object.bounding_sphere_center = previous_tick_object.center;
            std::copy(previous_tick_object.nodes, previous_tick_object.nodes + previous_tick_object.node_count, nodes);
        }
    }

    // Copy objects from Halo's data to buffer.
    static void copy_objects() noexcept {
        auto &object_table = ObjectTable::get_object_table();
        auto &contrail_table = ContrailTable::get_contrail_table();

        for(std::size_t i = 0; i < CONTRAIL_BUFFER_SIZE; i++) {
            auto &current_tick_object = current_tick[i];

            // Set this to false so invalid parents are never restored.
            current_tick_object.rollback = false;
            current_tick_object.node_count = 0;

            if(contrail_table.first_element[i].id == 0) {
                continue;
            }

            current_tick_object.object_id = contrail_table.first_element[i].parent_object_id;

            auto *object = object_table.get_dynamic_object(current_tick_object.object_id);
            if(!object) {
                continue;
            }

            auto *nodes = object->nodes();
            if(!nodes) {
                continue;
            }

            current_tick_object.tag_id = object->definition_index;
            auto *object_tag = get_tag(current_tick_object.tag_id.index.index);
            if(!object_tag || !object_tag->data) {
                continue;
            }

            if(object->object.type == ObjectType::OBJECT_TYPE_PROJECTILE) {
                current_tick_object.node_count = 1;
            }
            else {
                const auto &model_tag_id = *reinterpret_cast<const TagID *>(object_tag->data + 0x28 + 0xC);
                auto *model_tag = get_tag(model_tag_id);
                if(!model_tag || !model_tag->data) {
                    continue;
                }
                current_tick_object.node_count = *reinterpret_cast<std::uint32_t *>(model_tag->data + 0xB8);
            }

            // The snapshot is fixed-size. Refuse malformed or unsupported models
            // rather than copying beyond the buffer and corrupting adjacent state.
            if(current_tick_object.node_count > MAX_NODES) {
                current_tick_object.node_count = 0;
                continue;
            }

            current_tick_object.rollback = true;
            std::copy(nodes, nodes + current_tick_object.node_count, current_tick_object.nodes);
            current_tick_object.center = object->object.bounding_sphere_center;
        }
    }

    void fix_contrail_after() noexcept {
        if(!interpolation_enabled) {
            return;
        }

        auto &object_table = ObjectTable::get_object_table();
        auto &contrail_table = ContrailTable::get_contrail_table();
        auto max_objects = contrail_table.current_size;

        for(std::size_t i = 0; i < max_objects && i < CONTRAIL_BUFFER_SIZE; i++) {
            auto &current_tick_object = current_tick[i];

            if(!current_tick_object.rollback || current_tick_object.node_count > MAX_NODES) {
                continue;
            }

            auto *object = object_table.get_dynamic_object(current_tick_object.object_id);
            if(!object) {
                continue;
            }

            auto *nodes = object->nodes();
            if(!nodes) {
                continue;
            }

            object->object.bounding_sphere_center = current_tick_object.center;
            std::copy(current_tick_object.nodes, current_tick_object.nodes + current_tick_object.node_count, nodes);
        }
    }

    // Erase object buffers to prevent stale state on revert.
    void fix_contrail_clear() noexcept {
        std::memset(object_buffers, 0, sizeof(object_buffers));
        current_tick = object_buffers[0];
        previous_tick = object_buffers[1];
        tick_passed = false;
        can_update_contrail = 0;
    }

    static void allow_updates() {
        can_update_contrail = 1;
        update_contrail_by = 1.0F / effective_tick_rate();
        tick_passed = true;

        apply_interpolation_hack = interpolation_enabled ? 1U : 0U;
    }

    void set_up_contrail_fix() noexcept {
        auto *update_fix = get_chimera().get_signature("contrail_update_sig").data();
        auto *contrail_func_ptr = get_chimera().get_signature("contrail_update_func_sig").data();
        auto *interp_fix = get_chimera().get_signature("contrail_interpolation_fix_sig").data();
        skip_update = get_chimera().get_signature("contrail_skip_sig").data();
        static Hook contrail_update_hook;
        static Hook contrail_func_hook;
        static Hook contrail_interp_fix_hook;

        write_function_override(update_fix, contrail_update_hook, reinterpret_cast<const void *>(new_contrail_update_function), &original_contrail_update_function);
        // Make sure contrails trail their parent object when interpolating said objects.
        write_jmp_call(contrail_func_ptr, contrail_func_hook, reinterpret_cast<const void *>(fix_contrail_before), reinterpret_cast<const void *>(fix_contrail_after));
        // Prevent contrail updating on first tick after creation to prevent visual bugs.
        write_function_override(interp_fix, contrail_interp_fix_hook, reinterpret_cast<const void *>(interpolation_memes), &original_instruction);

        add_pretick_event(allow_updates);
        add_revert_event(fix_contrail_clear);
    }
}
