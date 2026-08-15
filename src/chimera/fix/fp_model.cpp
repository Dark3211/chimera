// SPDX-License-Identifier: GPL-3.0-only

#include "fp_model.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../chimera.hpp"
#include "../event/tick.hpp"
#include "../event/frame.hpp"
#include "../halo_data/rasterizer_common.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/game_functions.hpp"
#include "../halo_data/resolution.hpp"
#include "../math_trig/math_trig.hpp"


namespace Chimera {
    extern "C" void fp_model_lens_flare_asm() noexcept;

    bool lock_fp_model_fov = false;

    static float on_tick_x = 0;
    static float on_tick_y = 0;
    static bool can_update = false;
    static std::byte *fp_model_node_signature_data = nullptr;

    RenderFrustum frustum, frustum_fp;
    static float v_fov;
    static bool frustum_adjusted_for_fp_lens_flares = false;
    static bool fp_frustum_valid = false;
    static bool cached_fp_vertical_fov_valid = false;
    static std::uint16_t cached_fp_frame_extent = 0;
    static std::uint16_t cached_fp_resolution_height = 0;
    static float cached_fp_vertical_fov = 0.0f;

    void can_update_fp() noexcept {
        // Don't do the thing if there is a 2nd tick this frame.
        can_update = true;

        // Reset this as well.
        frustum_adjusted_for_fp_lens_flares = false;
    }

    void set_per_tick_fp_model_pos() noexcept {
        if(can_update) {
            auto *signature_data = fp_model_node_signature_data;
            if(!signature_data) {
                return;
            }
            auto **fp_data_pointer = *reinterpret_cast<std::byte ***>(signature_data + 2);
            auto *fp_data = fp_data_pointer ? *fp_data_pointer : nullptr;
            if(fp_data) {
                // These get updated per frame, so the difference between current and previous values ends up really small at high fps.
                // This results in the movement calculated by the fp model update function per tick being lower than expected.
                auto *current_x = reinterpret_cast<float *>(fp_data + 96);
                auto *previous_x = reinterpret_cast<float *>(fp_data + 104);
                auto *current_y = reinterpret_cast<float *>(fp_data + 100);
                auto *previous_y = reinterpret_cast<float *>(fp_data + 108);

                // Set the previous frame value to the value it was last tick.
                *previous_x = on_tick_x;
                *previous_y = on_tick_y;

                on_tick_x = *current_x;
                on_tick_y = *current_y;

                can_update = false;
            }
        }
    }

    void create_fp_frustum() noexcept {
        fp_frustum_valid = false;

        if(!global_window_parameters) {
            return;
        }

        // Back up default values.
        v_fov = global_window_parameters->camera.vertical_field_of_view;
        memcpy(&frustum, &global_window_parameters->frustum, sizeof(RenderFrustum));

        // Create new frustum scaled for fov.
        const auto &resolution = get_resolution();
        if(resolution.height == 0) {
            return;
        }

        const auto frame_extent = static_cast<std::uint16_t>(resolution.frame_bounds[2] - resolution.frame_bounds[0]);
        if(!cached_fp_vertical_fov_valid || cached_fp_frame_extent != frame_extent || cached_fp_resolution_height != resolution.height) {
            const float fov_scale_factor = static_cast<float>(frame_extent) / static_cast<float>(resolution.height);
            static const double base_fov_tangent = std::tan(DEGREES_TO_RADIANS(70.0f) * 0.5);
            cached_fp_vertical_fov = static_cast<float>(2.0 * std::atan((480.0f / 640.0f) * fov_scale_factor * base_fov_tangent));
            cached_fp_frame_extent = frame_extent;
            cached_fp_resolution_height = resolution.height;
            cached_fp_vertical_fov_valid = true;
        }

        global_window_parameters->camera.vertical_field_of_view = cached_fp_vertical_fov;
        render_camera_build_frustum(&global_window_parameters->camera, &global_window_parameters->frustum.frustum_bounds, &frustum_fp, true);
        fp_frustum_valid = true;
        global_window_parameters->camera.vertical_field_of_view = v_fov;
    }

    void adjust_frustum_for_fp_draw() noexcept {
        if(lock_fp_model_fov && global_window_parameters && fp_frustum_valid) {
            // Copy our adjusted frustum into window globals.
            memcpy(&global_window_parameters->frustum, &frustum_fp, sizeof(RenderFrustum));
        }
    }

    void restore_frustum() noexcept {
        if(lock_fp_model_fov && global_window_parameters) {
            memcpy(&global_window_parameters->frustum, &frustum, sizeof(RenderFrustum));
        }
    }

    extern "C" void fp_model_lens_flare_adjust_frustum(std::byte *lens_flare_draw_params) noexcept {
        if(lock_fp_model_fov && global_window_parameters && fp_frustum_valid && lens_flare_draw_params) {
            auto *index = reinterpret_cast<std::uint8_t *>(lens_flare_draw_params + 34);
            // The last bit is the fp flag.
            if(TEST_FLAG(*index, 7)) {
                adjust_frustum_for_fp_draw();
                rasterizer_set_frustum_z(0, 0);
                frustum_adjusted_for_fp_lens_flares = true;
            }
        }
    }

    void restore_frustum_lens_flare() noexcept {
        if(lock_fp_model_fov) {
            if(frustum_adjusted_for_fp_lens_flares) {
                restore_frustum();
                rasterizer_set_frustum_z(0, 0);
                frustum_adjusted_for_fp_lens_flares = false;
            }
        }
    }

    void set_up_fp_model_fix() noexcept {
        fp_model_node_signature_data = get_chimera().get_signature("first_person_node_base_address_sig").data();

        // Set up animation fix.
        add_pretick_event(set_per_tick_fp_model_pos);
        add_frame_event(can_update_fp);

        // Set up FoV fix.
        static Hook create_frus_hook;
        write_jmp_call(get_chimera().get_signature("rasterizer_window_begin_sig").data() + 5, create_frus_hook, nullptr, reinterpret_cast<const void *>(create_fp_frustum));

        // Model draw
        static Hook model_begin_hook, model_end_hook;
        write_jmp_call(get_chimera().get_signature("rasterizer_model_begin_fp_sig").data() + 2, model_begin_hook, reinterpret_cast<const void *>(adjust_frustum_for_fp_draw), nullptr);
        write_jmp_call(get_chimera().get_signature("rasterizer_model_end_fp_sig").data() + 4, model_end_hook, reinterpret_cast<const void *>(restore_frustum), nullptr);

        // Transparent geometry draw
        static Hook transparent_begin_hook, transparent_end_hook;
        write_jmp_call(get_chimera().get_signature("rasterizer_transparent_geo_fp_begin_sig").data() + 2, transparent_begin_hook, reinterpret_cast<const void *>(adjust_frustum_for_fp_draw), nullptr);
        write_jmp_call(get_chimera().get_signature("rasterizer_transparent_geo_fp_end_sig").data() + 2, transparent_end_hook, reinterpret_cast<const void *>(restore_frustum), nullptr);

        // Lens flare draw
        static Hook lens_flare_begin, lens_flare_end;
        write_jmp_call(get_chimera().get_signature("lens_flares_draw_fp_begin_sig").data() + 7, lens_flare_begin, reinterpret_cast<const void *>(fp_model_lens_flare_asm), nullptr);
        write_jmp_call(get_chimera().get_signature("lens_flares_draw_fp_end_sig").data(), lens_flare_end, nullptr, reinterpret_cast<const void *>(restore_frustum_lens_flare));

        // Lens flare occlusion test
        static Hook occlusion_begin, occlusion_end;
        write_jmp_call(get_chimera().get_signature("lens_flare_occlusion_fp_begin_sig").data() + 4, occlusion_begin, reinterpret_cast<const void *>(fp_model_lens_flare_asm), nullptr);
        write_jmp_call(get_chimera().get_signature("lens_flare_occlusion_fp_end_sig").data(), occlusion_end, reinterpret_cast<const void *>(restore_frustum_lens_flare), nullptr);
    }
}
