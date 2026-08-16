// SPDX-License-Identifier: GPL-3.0-only

#include <bit>
#include <d3d9.h>

#include "af.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"

namespace Chimera {
    extern "C" {
        void schi_set_sampler_states_for_af_asm() noexcept;
        void scex_set_sampler_states_for_af_asm() noexcept;
    }

    static IDirect3DDevice9 **global_d3d9_device = nullptr;
    bool *af_is_enabled = nullptr;
    static D3DCAPS9 *d3d9_device_caps = nullptr;
    std::uint32_t global_max_anisotropy = 16;
    bool af_trial = false;

    static constexpr float high_quality_mip_lod_bias = -0.25f;
    static constexpr float environment_detail_mip_lod_bias = -0.45f;
    static constexpr float environment_bump_mip_lod_bias = -0.40f;

    struct EnvironmentSamplerState {
        bool valid = false;
        DWORD mip_filter = 0;
        DWORD mip_lod_bias = 0;
    };

    static EnvironmentSamplerState environment_detail_sampler_states[3] {};
    static bool environment_detail_sampler_states_saved = false;
    static EnvironmentSamplerState environment_bump_sampler_state {};
    static bool environment_bump_sampler_state_saved = false;
    static bool environment_bump_sampling_enabled = true;

    static bool af_supported() noexcept {
        return d3d9_device_caps &&
               (d3d9_device_caps->RasterCaps & D3DPRASTERCAPS_ANISOTROPY) != 0 &&
               1 < d3d9_device_caps->MaxAnisotropy;
    }

    static std::uint32_t effective_max_anisotropy() noexcept {
        return d3d9_device_caps->MaxAnisotropy < global_max_anisotropy ? d3d9_device_caps->MaxAnisotropy : global_max_anisotropy;
    }

    static void clear_environment_detail_sampler_states() noexcept {
        for(auto &state : environment_detail_sampler_states) {
            state = {};
        }
        environment_detail_sampler_states_saved = false;
    }

    static void clear_environment_bump_sampler_state() noexcept {
        environment_bump_sampler_state = {};
        environment_bump_sampler_state_saved = false;
    }

    static void restore_environment_detail_sampler_states() noexcept {
        if(!environment_detail_sampler_states_saved) {
            return;
        }

        if(global_d3d9_device && *global_d3d9_device) {
            for(std::uint32_t sampler = 1; sampler <= 3; sampler++) {
                const auto &state = environment_detail_sampler_states[sampler - 1];
                if(!state.valid) {
                    continue;
                }

                IDirect3DDevice9_SetSamplerState(*global_d3d9_device, sampler, D3DSAMP_MIPFILTER, state.mip_filter);
                IDirect3DDevice9_SetSamplerState(*global_d3d9_device, sampler, D3DSAMP_MIPMAPLODBIAS, state.mip_lod_bias);
            }
        }

        clear_environment_detail_sampler_states();
    }

    static void restore_environment_bump_sampler_state() noexcept {
        if(!environment_bump_sampler_state_saved) {
            return;
        }

        if(global_d3d9_device && *global_d3d9_device && environment_bump_sampler_state.valid) {
            IDirect3DDevice9_SetSamplerState(
                *global_d3d9_device,
                0,
                D3DSAMP_MIPFILTER,
                environment_bump_sampler_state.mip_filter
            );
            IDirect3DDevice9_SetSamplerState(
                *global_d3d9_device,
                0,
                D3DSAMP_MIPMAPLODBIAS,
                environment_bump_sampler_state.mip_lod_bias
            );
        }

        clear_environment_bump_sampler_state();
    }

    static void restore_environment_sampling_states() noexcept {
        restore_environment_detail_sampler_states();
        restore_environment_bump_sampler_state();
    }

    void set_environment_bump_sampling_enabled(bool enabled) noexcept {
        if(environment_bump_sampling_enabled == enabled) {
            return;
        }

        // If the diagnostic is switched off while a lightmap draw still has the
        // temporary bump sampler state active, restore it immediately.
        if(!enabled) {
            restore_environment_bump_sampler_state();
        }
        environment_bump_sampling_enabled = enabled;
    }

    bool get_environment_bump_sampling_enabled() noexcept {
        return environment_bump_sampling_enabled;
    }

    extern "C" void restore_environment_detail_af() noexcept {
        // Historical name retained for the assembly hooks. It now restores every
        // temporary environment sampling override, including the bump sampler.
        restore_environment_sampling_states();
    }

    extern "C" void apply_environment_detail_af() noexcept {
        // A stale state should never be allowed to accumulate into the next material.
        restore_environment_sampling_states();

        if(!af_is_enabled || !*af_is_enabled || !af_supported() ||
           !global_d3d9_device || !*global_d3d9_device) {
            return;
        }

        const bool lod_bias_supported =
            (d3d9_device_caps->RasterCaps & D3DPRASTERCAPS_MIPMAPLODBIAS) != 0;
        const bool linear_mips_supported =
            (d3d9_device_caps->TextureFilterCaps & D3DPTFILTERCAPS_MIPFLINEAR) != 0;
        if(!lod_bias_supported && !linear_mips_supported) {
            return;
        }

        // CEnshine's reconstructed Halo environment shader confirms that sampler 0
        // is the base map, while samplers 1/2/3 are primary, secondary and micro
        // detail maps. Never touch sampler 0 here so the authored base texture keeps
        // the normal AF sampling profile.
        const std::uint32_t highest_sampler =
            d3d9_device_caps->PixelShaderVersion > 0xffff0100 ? 3U : 1U;

        bool saved_any_state = false;
        for(std::uint32_t sampler = 1; sampler <= highest_sampler; sampler++) {
            DWORD mip_filter = 0;
            DWORD mip_lod_bias = 0;

            const auto mip_result = IDirect3DDevice9_GetSamplerState(
                *global_d3d9_device,
                sampler,
                D3DSAMP_MIPFILTER,
                &mip_filter
            );
            const auto bias_result = IDirect3DDevice9_GetSamplerState(
                *global_d3d9_device,
                sampler,
                D3DSAMP_MIPMAPLODBIAS,
                &mip_lod_bias
            );

            // Only change a sampler if both states can be restored exactly later.
            if(FAILED(mip_result) || FAILED(bias_result)) {
                continue;
            }

            auto &saved = environment_detail_sampler_states[sampler - 1];
            saved.valid = true;
            saved.mip_filter = mip_filter;
            saved.mip_lod_bias = mip_lod_bias;
            saved_any_state = true;

            if(linear_mips_supported) {
                IDirect3DDevice9_SetSamplerState(
                    *global_d3d9_device,
                    sampler,
                    D3DSAMP_MIPFILTER,
                    D3DTEXF_LINEAR
                );
            }
            if(lod_bias_supported) {
                IDirect3DDevice9_SetSamplerState(
                    *global_d3d9_device,
                    sampler,
                    D3DSAMP_MIPMAPLODBIAS,
                    std::bit_cast<DWORD>(environment_detail_mip_lod_bias)
                );
            }
        }

        environment_detail_sampler_states_saved = saved_any_state;
    }

    extern "C" void apply_environment_bump_af() noexcept {
        // The lightmap pass follows the texture/detail pass. Restore all temporary
        // detail states first, then isolate the bump-map override to sampler 0.
        restore_environment_sampling_states();

        if(!environment_bump_sampling_enabled ||
           !af_is_enabled || !*af_is_enabled || !af_supported() ||
           !global_d3d9_device || !*global_d3d9_device) {
            return;
        }

        const bool lod_bias_supported =
            (d3d9_device_caps->RasterCaps & D3DPRASTERCAPS_MIPMAPLODBIAS) != 0;
        const bool linear_mips_supported =
            (d3d9_device_caps->TextureFilterCaps & D3DPTFILTERCAPS_MIPFLINEAR) != 0;
        if(!lod_bias_supported && !linear_mips_supported) {
            return;
        }

        DWORD mip_filter = 0;
        DWORD mip_lod_bias = 0;
        const auto mip_result = IDirect3DDevice9_GetSamplerState(
            *global_d3d9_device,
            0,
            D3DSAMP_MIPFILTER,
            &mip_filter
        );
        const auto bias_result = IDirect3DDevice9_GetSamplerState(
            *global_d3d9_device,
            0,
            D3DSAMP_MIPMAPLODBIAS,
            &mip_lod_bias
        );

        if(FAILED(mip_result) || FAILED(bias_result)) {
            return;
        }

        environment_bump_sampler_state.valid = true;
        environment_bump_sampler_state.mip_filter = mip_filter;
        environment_bump_sampler_state.mip_lod_bias = mip_lod_bias;
        environment_bump_sampler_state_saved = true;

        if(linear_mips_supported) {
            IDirect3DDevice9_SetSamplerState(
                *global_d3d9_device,
                0,
                D3DSAMP_MIPFILTER,
                D3DTEXF_LINEAR
            );
        }
        if(lod_bias_supported) {
            IDirect3DDevice9_SetSamplerState(
                *global_d3d9_device,
                0,
                D3DSAMP_MIPMAPLODBIAS,
                std::bit_cast<DWORD>(environment_bump_mip_lod_bias)
            );
        }
    }

    static void apply_high_quality_af(std::uint32_t sampler, std::uint32_t max_anisotropy) noexcept {
        IDirect3DDevice9_SetSamplerState(*global_d3d9_device, sampler, D3DSAMP_MAXANISOTROPY, max_anisotropy);
        IDirect3DDevice9_SetSamplerState(*global_d3d9_device, sampler, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
        IDirect3DDevice9_SetSamplerState(*global_d3d9_device, sampler, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);

        // Keep mip transitions smooth while anisotropic filtering is active. Halo's
        // original AF paths do not consistently request trilinear mip sampling for
        // every material path, which can leave visible mip bands at oblique angles.
        IDirect3DDevice9_SetSamplerState(*global_d3d9_device, sampler, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

        // Use a small negative LOD bias to retain a little more texture detail at
        // distance. Keeping this conservative lets AF absorb most of the additional
        // aliasing risk while avoiding the over-sharpened look of aggressive biases.
        IDirect3DDevice9_SetSamplerState(*global_d3d9_device, sampler, D3DSAMP_MIPMAPLODBIAS, std::bit_cast<DWORD>(high_quality_mip_lod_bias));
    }

    void set_sampler_states_for_models() noexcept {
        restore_environment_sampling_states();

        if(*af_is_enabled && af_supported()) {
            auto max_anisotropy = effective_max_anisotropy();

            apply_high_quality_af(0, max_anisotropy);
            apply_high_quality_af(1, max_anisotropy);

            // Samplers 2 and 3 aren't used if ps < 1.1, not that thats ever really going to come up in the year 2025.
            if(d3d9_device_caps->PixelShaderVersion > 0xffff0100) {
                apply_high_quality_af(2, max_anisotropy);
                apply_high_quality_af(3, max_anisotropy);
            }
        }
    }

    void set_sampler_states_for_structures() noexcept {
        restore_environment_sampling_states();

        // This is only for the demo. The base game does it for retail/custom edition on 1.0.10.
        if(*af_is_enabled && af_supported()) {
            auto max_anisotropy = effective_max_anisotropy();

            apply_high_quality_af(0, max_anisotropy);
            apply_high_quality_af(1, max_anisotropy);

            // Samplers 2 and 3 aren't used if ps < 1.1.
            if(d3d9_device_caps->PixelShaderVersion > 0xffff0100) {
                apply_high_quality_af(2, max_anisotropy);
                apply_high_quality_af(3, max_anisotropy);
            }
        }
    }

    void set_sampler_states_for_decals() noexcept {
        restore_environment_sampling_states();

        if(*af_is_enabled && af_supported()) {
            apply_high_quality_af(0, effective_max_anisotropy());
        }
    }

    void set_sampler_states_for_plasma() noexcept {
        restore_environment_sampling_states();

        if(*af_is_enabled && af_supported()) {
            auto max_anisotropy = effective_max_anisotropy();

            // Barely makes much difference with shader_transparent_plasma. Whatever.
            apply_high_quality_af(0, max_anisotropy);
            apply_high_quality_af(1, max_anisotropy);
        }
    }

    extern "C" void set_sampler_states_for_chicago(std::byte *map, std::uint32_t map_index) noexcept {
        restore_environment_sampling_states();

        if(*af_is_enabled) {
            auto *map_flags = reinterpret_cast<std::uint16_t *>(map);
            // If unfiltered flag is true, do nothing.
            if(!(*map_flags & 1) && af_supported()) {
                apply_high_quality_af(map_index, effective_max_anisotropy());
            }
        }
    }

    void set_up_model_af() noexcept {
        global_d3d9_device = reinterpret_cast<IDirect3DDevice9 **>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("model_af_set_sampler_states_sig").data() + 1));
        d3d9_device_caps = reinterpret_cast<D3DCAPS9 *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("d3d9_device_caps_sig").data() + 1));

        // Demo doesn't have built in AF so we need to handle everything with chimera.
        if(get_chimera().feature_present("client_af")) {
            af_is_enabled = reinterpret_cast<bool *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("af_is_enabled_sig").data() + 1));
        }
        else {
            af_is_enabled = &af_trial;
        }

        static Hook model_af;
        static Hook decal_af;
        static Hook glass_af;
        static Hook chicago_af;
        static Hook extended_af;
        static Hook plasma_af;

        write_jmp_call(get_chimera().get_signature("model_af_set_sampler_states_sig").data() + 0x1A, model_af, reinterpret_cast<const void *>(set_sampler_states_for_models), nullptr);
        write_jmp_call(get_chimera().get_signature("decal_af_set_sampler_states_sig").data(), decal_af, reinterpret_cast<const void *>(set_sampler_states_for_decals), nullptr);
        write_jmp_call(get_chimera().get_signature("glass_af_set_sampler_states_sig").data(), glass_af, nullptr, reinterpret_cast<const void *>(set_sampler_states_for_decals));
        write_jmp_call(get_chimera().get_signature("chicago_af_set_sampler_states_sig").data(), chicago_af, nullptr, reinterpret_cast<const void *>(schi_set_sampler_states_for_af_asm));
        write_jmp_call(get_chimera().get_signature("extended_af_set_sampler_states_sig").data(), extended_af, nullptr, reinterpret_cast<const void *>(scex_set_sampler_states_for_af_asm));
        write_jmp_call(get_chimera().get_signature("plasma_af_set_sampler_states_sig").data(), plasma_af, nullptr, reinterpret_cast<const void *>(set_sampler_states_for_plasma));
        if(get_chimera().feature_present("client_demo")) {
            static Hook structure_af;
            write_jmp_call(get_chimera().get_signature("structure_af_set_sampler_states_sig").data(), structure_af, nullptr, reinterpret_cast<const void *>(set_sampler_states_for_structures));
        }

        auto af_level = get_chimera().get_ini()->get_value_long("video_mode.af_level").value_or(16);
        if(af_level > 0 && af_level <= 16) {
            global_max_anisotropy = af_level;
        }

        if(get_chimera().feature_present("client_af")) {
            // Set max supported filtering level for level geo.
            overwrite(get_chimera().get_signature("af_level_sig").data() + 1, static_cast<std::uint32_t>(global_max_anisotropy));
        }
    }
}
