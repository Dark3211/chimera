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

        bool d3d9_on12_device_verified = false;
        bool halo_vertex_mode_reported = false;

        void synchronize_halo_vertex_processing_mode() {
            if(!rasterizer_globals || !global_d3d9_device || !*global_d3d9_device) {
                return;
            }

            if(!d3d9_on12_device_verified) {
                IUnknown *on_12_interface = nullptr;
                auto result = (*global_d3d9_device)->QueryInterface(
                    IID_IDIRECT3DDEVICE9ON12_VALUE,
                    reinterpret_cast<void **>(&on_12_interface)
                );
                if(FAILED(result) || !on_12_interface) {
                    return;
                }

                on_12_interface->Release();
                d3d9_on12_device_verified = true;
            }

            // A full D3DCREATE_SOFTWARE_VERTEXPROCESSING device fixed Halo's
            // corrupted vehicles/weapons, while mixed-mode toggling alone did not.
            // Halo keeps its own renderer-side copy of the vertex-processing mode
            // and uses it while preparing model vertices/declarations. Keep that
            // engine-side state on the software-compatible path before each frame;
            // d3d9_backend.cpp may still switch known-safe draws back to HWVP.
            rasterizer_globals->using_software_vertex_processing = true;

            if(!halo_vertex_mode_reported) {
                console_output("D3D9 backend: Halo renderer preprocessing synchronized to software vertex mode.");
                halo_vertex_mode_reported = true;
            }
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
            if(backend && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0)) {
                add_preframe_event(synchronize_halo_vertex_processing_mode, EVENT_PRIORITY_BEFORE);
            }

            game_variables_enabled = true;
        }
    }
}
