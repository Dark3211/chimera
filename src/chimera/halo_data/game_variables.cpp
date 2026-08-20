// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <cstring>

#include "game_variables.hpp"
#include "../chimera.hpp"
#include "../signature/signature.hpp"
#include "../signature/hook.hpp"
#include "../event/frame.hpp"
#include "../rasterizer/rasterizer.hpp"
#include "../config/ini.hpp"
#include "../output/output.hpp"

namespace Chimera {
    DynamicVertices *dynamic_vertices;
    VertexShader *vertex_shaders;
    short *vertex_shader_permutations;
    VertexDeclaration *vertex_declarations;
    RasterizerFrameParameters *global_frame_parameters;
    RasterizerGlobals *rasterizer_globals;
    RasterizerWindowParameters *global_window_parameters;
    bool *fog_enabled;
    RasterizerGlobalData **global_rasterizer_data;
    D3DPRESENT_PARAMETERS *d3d_present_parameters;
    std::uint32_t *local_random_seed;
    RasterizerDebugOptions *rasterizer_debug_options;
    bool *water_visible_for_window_flag;
    bool *model_sky_flag;
    unsigned char **local_node_remap_table;
    std::int32_t *local_node_remap_table_size;
    GameStateGlobals *game_state_globals;
    StructureBsp **global_structure_bsp;
    WindGlobals *wind_globals;

    namespace {
        // IDirect3DDevice9On12. Querying this interface lets us distinguish an
        // actually-created 9On12 device from a requested backend that fell back
        // to native D3D9.
        constexpr GUID IID_IDIRECT3DDEVICE9ON12_VALUE = {
            0xE7FDA234, 0xB589, 0x4049, {0x94, 0x0D, 0x88, 0x78, 0x97, 0x75, 0x31, 0xC8}
        };

        bool d3d9_on12_requested = false;
        bool d3d9_on12_device_checked = false;
        bool d3d9_on12_device_verified = false;
        IDirect3DDevice9 *d3d9_on12_checked_device = nullptr;

        bool halo_vertex_mode_reported = false;
        unsigned int halo_software_vp_scope_depth = 0;
        bool halo_software_vp_saved_state = false;

        const void *original_transparent_geometry_group_draw = nullptr;

        bool verify_d3d9_on12_device() noexcept {
            if(!d3d9_on12_requested || !rasterizer_globals || !global_d3d9_device || !*global_d3d9_device) {
                return false;
            }

            auto *device = *global_d3d9_device;
            if(device != d3d9_on12_checked_device) {
                d3d9_on12_checked_device = device;
                d3d9_on12_device_checked = false;
                d3d9_on12_device_verified = false;
            }

            if(!d3d9_on12_device_checked) {
                d3d9_on12_device_checked = true;

                IUnknown *on_12_interface = nullptr;
                auto result = device->QueryInterface(
                    IID_IDIRECT3DDEVICE9ON12_VALUE,
                    reinterpret_cast<void **>(&on_12_interface)
                );
                if(SUCCEEDED(result) && on_12_interface) {
                    on_12_interface->Release();
                    d3d9_on12_device_verified = true;
                }
            }

            return d3d9_on12_device_verified;
        }

        bool enter_halo_software_vertex_processing_scope() noexcept {
            if(!verify_d3d9_on12_device()) {
                return false;
            }

            if(halo_software_vp_scope_depth == 0) {
                halo_software_vp_saved_state = rasterizer_globals->using_software_vertex_processing;
                rasterizer_globals->using_software_vertex_processing = true;
            }
            halo_software_vp_scope_depth++;

            if(!halo_vertex_mode_reported) {
                console_output("D3D9 backend: selective Halo software vertex preprocessing enabled for model paths.");
                halo_vertex_mode_reported = true;
            }

            return true;
        }

        void leave_halo_software_vertex_processing_scope() noexcept {
            if(halo_software_vp_scope_depth == 0) {
                return;
            }

            halo_software_vp_scope_depth--;
            if(halo_software_vp_scope_depth == 0 && rasterizer_globals) {
                rasterizer_globals->using_software_vertex_processing = halo_software_vp_saved_state;
            }
        }

        void begin_halo_software_vertex_processing_scope() noexcept {
            enter_halo_software_vertex_processing_scope();
        }

        void end_halo_software_vertex_processing_scope() noexcept {
            leave_halo_software_vertex_processing_scope();
        }

        void reset_halo_software_vertex_processing_scope() noexcept {
            if(halo_software_vp_scope_depth != 0 && rasterizer_globals) {
                rasterizer_globals->using_software_vertex_processing = halo_software_vp_saved_state;
            }
            halo_software_vp_scope_depth = 0;
        }

        using TransparentGeometryDrawFunction = void (*)(TransparentGeometryGroup *, bool);

        void selective_transparent_geometry_group_draw(TransparentGeometryGroup *group, bool is_dirty) noexcept {
            auto original = reinterpret_cast<TransparentGeometryDrawFunction>(original_transparent_geometry_group_draw);
            if(!original) {
                return;
            }

            bool needs_software_vertex_processing = false;
            if(group) {
                constexpr std::uint32_t dont_skin_mask = 1u << RASTERIZER_GEOMETRY_FLAGS_DONT_SKIN;
                constexpr std::uint32_t local_nodes_mask = 1u << RASTERIZER_GEOMETRY_FLAGS_PARTS_DEFINE_LOCAL_NODES_BIT;

                needs_software_vertex_processing =
                    !(group->geometry_flags & dont_skin_mask) &&
                    (group->node_matrix_count > 0 || (group->geometry_flags & local_nodes_mask));
            }

            bool entered = false;
            if(needs_software_vertex_processing) {
                entered = enter_halo_software_vertex_processing_scope();
            }

            original(group, is_dirty);

            if(entered) {
                leave_halo_software_vertex_processing_scope();
            }
        }

        void set_up_selective_halo_vertex_processing() noexcept {
            static bool enabled = false;
            if(enabled) {
                return;
            }
            enabled = true;

            // These brackets cover Halo's opaque model preprocessing. They are
            // installed before the fog/FP hooks and safely chain with those hooks.
            static Hook model_begin_hook;
            static Hook model_end_hook;
            write_jmp_call(
                get_chimera().get_signature("rasterizer_model_begin_fog_sig").data(),
                model_begin_hook,
                reinterpret_cast<const void *>(begin_halo_software_vertex_processing_scope),
                nullptr
            );
            write_jmp_call(
                get_chimera().get_signature("rasterizer_model_end_fog_sig").data(),
                model_end_hook,
                reinterpret_cast<const void *>(end_halo_software_vertex_processing_scope),
                nullptr
            );

            // First-person models have their own renderer path, so scope that path
            // separately in case it does not pass through the generic model bracket.
            static Hook fp_model_begin_hook;
            static Hook fp_model_end_hook;
            write_jmp_call(
                get_chimera().get_signature("rasterizer_model_begin_fp_sig").data() + 2,
                fp_model_begin_hook,
                reinterpret_cast<const void *>(begin_halo_software_vertex_processing_scope),
                nullptr
            );
            write_jmp_call(
                get_chimera().get_signature("rasterizer_model_end_fp_sig").data() + 4,
                fp_model_end_hook,
                reinterpret_cast<const void *>(end_halo_software_vertex_processing_scope),
                nullptr
            );

            static Hook fp_transparent_begin_hook;
            static Hook fp_transparent_end_hook;
            write_jmp_call(
                get_chimera().get_signature("rasterizer_transparent_geo_fp_begin_sig").data() + 2,
                fp_transparent_begin_hook,
                reinterpret_cast<const void *>(begin_halo_software_vertex_processing_scope),
                nullptr
            );
            write_jmp_call(
                get_chimera().get_signature("rasterizer_transparent_geo_fp_end_sig").data() + 2,
                fp_transparent_end_hook,
                reinterpret_cast<const void *>(end_halo_software_vertex_processing_scope),
                nullptr
            );

            // Queued transparent model geometry is rendered later, outside the
            // opaque model bracket. Only groups that actually need skinning/local
            // node remapping take Halo's software-compatible preprocessing path.
            static Hook transparent_geometry_draw_hook;
            write_function_override(
                get_chimera().get_signature("transparent_geometry_group_draw_sig").data(),
                transparent_geometry_draw_hook,
                reinterpret_cast<const void *>(selective_transparent_geometry_group_draw),
                &original_transparent_geometry_group_draw
            );

            // If an unusual early-out ever leaves a model bracket unbalanced,
            // restore Halo's previous renderer state before the next frame.
            add_preframe_event(reset_halo_software_vertex_processing_scope, EVENT_PRIORITY_BEFORE);
        }
    }

    void set_up_game_variables() noexcept {
        static bool game_variables_enabled = false;
        if(!game_variables_enabled) {
            dynamic_vertices = reinterpret_cast<DynamicVertices *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("dynamic_vertices_sig").data() + 1));
            vertex_shaders = reinterpret_cast<VertexShader *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("vertex_shaders_sig").data() + 3));
            vertex_shader_permutations = reinterpret_cast<short *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("vertex_shader_permutations_sig").data() + 7));
            vertex_declarations = reinterpret_cast<VertexDeclaration *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("vertex_shader_declaration_sig").data() + 6));
            global_frame_parameters = reinterpret_cast<RasterizerFrameParameters *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("global_frame_parameters_sig").data() + 2));
            rasterizer_globals = reinterpret_cast<RasterizerGlobals *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("rasterizer_globals_sig").data() + 4));
            global_window_parameters = reinterpret_cast<RasterizerWindowParameters*>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("global_window_parameters_sig").data() + 3));
            fog_enabled = reinterpret_cast<bool *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("fog_enabled_sig").data() + 6));
            global_rasterizer_data = reinterpret_cast<RasterizerGlobalData **>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("global_rasterizer_data_sig").data() + 1));
            d3d_present_parameters = reinterpret_cast<D3DPRESENT_PARAMETERS *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("d3d_present_params_sig").data() + 6));
            local_random_seed = reinterpret_cast<std::uint32_t *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("local_random_seed_sig").data() + 6));
            rasterizer_debug_options = reinterpret_cast<RasterizerDebugOptions *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("rasterizer_debug_globals_sig").data() + 1));
            water_visible_for_window_flag = reinterpret_cast<bool *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("water_visible_for_window_flag_sig").data() + 9));
            model_sky_flag = reinterpret_cast<bool *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("model_sky_flag_sig").data() + 9));
            local_node_remap_table = reinterpret_cast<unsigned char **>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("rasterizer_set_up_node_parts_sig").data() + 15));
            local_node_remap_table_size = reinterpret_cast<std::int32_t *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("rasterizer_set_up_node_parts_sig").data() + 21));
            game_state_globals = reinterpret_cast<GameStateGlobals *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("game_state_globals_sig").data() + 21));
            global_structure_bsp = *reinterpret_cast<StructureBsp ***>(get_chimera().get_signature("current_bsp_tag_sig").data() + 1);
            wind_globals = *reinterpret_cast<WindGlobals **>(get_chimera().get_signature("wind_globals_sig").data() + 8);

            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            d3d9_on12_requested = backend && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
            if(d3d9_on12_requested) {
                set_up_selective_halo_vertex_processing();
            }

            game_variables_enabled = true;
        }
    }
}
