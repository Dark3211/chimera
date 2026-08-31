// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_RASTERIZER_HIGHLIGHT_PROTECTION_HPP
#define CHIMERA_RASTERIZER_HIGHLIGHT_PROTECTION_HPP

#include <cstddef>
#include <cstring>
#include <string>
#include <d3dcompiler.h>

namespace Chimera {
    namespace HighlightProtection {
        // Protect only highlight energy added by Chimera's existing brightness,
        // contrast and saturation correction. Neutral 1.0/1.0/1.0 settings leave
        // the image untouched, and scene-native highlights/reflections are never
        // reduced below their original luminance. A pressure-derived soft shoulder
        // progressively compresses only positive color-correction gain near white.
        // Broad bright areas receive extra protection only when brightness persists
        // several pixels away in both screen axes, keeping small specular highlights
        // outside the additional diffuse-area reduction.
        inline void inject(std::string &shader_source) {
            constexpr const char *COLOR_APPLY =
                "color = lerp(color, saturate(corrected), saturate(color_options.w));";
            const auto color_apply_position = shader_source.find(COLOR_APPLY);
            if(color_apply_position == std::string::npos) {
                return;
            }

            constexpr const char *HIGHLIGHT_PROTECTION = R"HLSL(const float3 highlight_base = color;
                const float highlight_base_luma = dot(highlight_base, float3(0.299, 0.587, 0.114));
                const float highlight_base_peak = max(highlight_base.r, max(highlight_base.g, highlight_base.b));
                const float highlight_corrected_luma = dot(corrected, float3(0.299, 0.587, 0.114));

                const float highlight_brightness_boost = max(color_options.x - 1.0, 0.0);
                const float highlight_contrast_boost = max(color_options.y - 1.0, 0.0);
                const float highlight_saturation_boost = max(color_options.z - 1.0, 0.0);
                const float highlight_pressure = saturate((
                    highlight_brightness_boost * 0.90 +
                    highlight_contrast_boost * 1.25 +
                    highlight_saturation_boost * 0.35
                ) * saturate(color_options.w));
                const float highlight_activation = saturate(highlight_pressure * 2.25);

                // Start the normal shoulder around 0.90 for mild correction and move
                // it smoothly toward 0.80 as B/C/S becomes more aggressive.
                const float highlight_shoulder_start = lerp(0.90, 0.80, highlight_pressure);
                float highlight_shoulder_t = saturate(
                    (highlight_corrected_luma - highlight_shoulder_start) /
                    max(1.0 - highlight_shoulder_start, 0.0001)
                );
                highlight_shoulder_t = highlight_shoulder_t * highlight_shoulder_t *
                    (3.0 - 2.0 * highlight_shoulder_t);

                // Detect broad diffuse highlights using four samples several pixels
                // away from the current pixel. Requiring bright support in both axes
                // rejects isolated point highlights and thin specular streaks.
                const float2 highlight_area_step = frame_options.xy * 6.0;
                const float3 highlight_area_l = tex2D(frame_sampler, uv + float2(-highlight_area_step.x, 0.0)).rgb;
                const float3 highlight_area_r = tex2D(frame_sampler, uv + float2( highlight_area_step.x, 0.0)).rgb;
                const float3 highlight_area_u = tex2D(frame_sampler, uv + float2(0.0, -highlight_area_step.y)).rgb;
                const float3 highlight_area_d = tex2D(frame_sampler, uv + float2(0.0,  highlight_area_step.y)).rgb;

                const float highlight_area_l_raw = dot(highlight_area_l, float3(0.299, 0.587, 0.114));
                const float highlight_area_r_raw = dot(highlight_area_r, float3(0.299, 0.587, 0.114));
                const float highlight_area_u_raw = dot(highlight_area_u, float3(0.299, 0.587, 0.114));
                const float highlight_area_d_raw = dot(highlight_area_d, float3(0.299, 0.587, 0.114));
                const float highlight_color_mix = saturate(color_options.w);
                const float highlight_area_l_luma = lerp(
                    highlight_area_l_raw,
                    ((highlight_area_l_raw - 0.5) * color_options.y + 0.5) * color_options.x,
                    highlight_color_mix
                );
                const float highlight_area_r_luma = lerp(
                    highlight_area_r_raw,
                    ((highlight_area_r_raw - 0.5) * color_options.y + 0.5) * color_options.x,
                    highlight_color_mix
                );
                const float highlight_area_u_luma = lerp(
                    highlight_area_u_raw,
                    ((highlight_area_u_raw - 0.5) * color_options.y + 0.5) * color_options.x,
                    highlight_color_mix
                );
                const float highlight_area_d_luma = lerp(
                    highlight_area_d_raw,
                    ((highlight_area_d_raw - 0.5) * color_options.y + 0.5) * color_options.x,
                    highlight_color_mix
                );

                const float highlight_area_support_start = max(highlight_shoulder_start - 0.04, 0.72);
                float highlight_area_sl = saturate((highlight_area_l_luma - highlight_area_support_start) / 0.14);
                float highlight_area_sr = saturate((highlight_area_r_luma - highlight_area_support_start) / 0.14);
                float highlight_area_su = saturate((highlight_area_u_luma - highlight_area_support_start) / 0.14);
                float highlight_area_sd = saturate((highlight_area_d_luma - highlight_area_support_start) / 0.14);
                highlight_area_sl = highlight_area_sl * highlight_area_sl * (3.0 - 2.0 * highlight_area_sl);
                highlight_area_sr = highlight_area_sr * highlight_area_sr * (3.0 - 2.0 * highlight_area_sr);
                highlight_area_su = highlight_area_su * highlight_area_su * (3.0 - 2.0 * highlight_area_su);
                highlight_area_sd = highlight_area_sd * highlight_area_sd * (3.0 - 2.0 * highlight_area_sd);
                float highlight_diffuse_support = min(
                    max(highlight_area_sl, highlight_area_sr),
                    max(highlight_area_su, highlight_area_sd)
                );
                highlight_diffuse_support = highlight_diffuse_support * highlight_diffuse_support *
                    (3.0 - 2.0 * highlight_diffuse_support);

                // Broad illuminated surfaces begin compressing slightly earlier than
                // isolated highlights, but only the B/C/S-added luminance is reduced.
                const float highlight_diffuse_start = max(highlight_shoulder_start - 0.05, 0.70);
                float highlight_diffuse_shoulder_t = saturate(
                    (highlight_corrected_luma - highlight_diffuse_start) /
                    max(1.0 - highlight_diffuse_start, 0.0001)
                );
                highlight_diffuse_shoulder_t = highlight_diffuse_shoulder_t * highlight_diffuse_shoulder_t *
                    (3.0 - 2.0 * highlight_diffuse_shoulder_t);
                const float highlight_effective_shoulder = max(
                    highlight_shoulder_t,
                    highlight_diffuse_support * highlight_diffuse_shoulder_t * 0.95
                );

                // Only positive luminance gain introduced by color correction may
                // be compressed. Original scene brightness is always the floor.
                const float highlight_positive_gain = step(
                    highlight_base_luma + 0.000001,
                    highlight_corrected_luma
                );
                const float highlight_reduction = lerp(0.25, 0.70, highlight_pressure);
                const float highlight_diffuse_extra = lerp(0.15, 0.35, highlight_pressure);
                const float highlight_effective_reduction = saturate(
                    highlight_reduction + highlight_diffuse_support * highlight_diffuse_extra
                );
                const float highlight_soft_mask = saturate(
                    highlight_effective_shoulder * highlight_activation * highlight_positive_gain
                );
                const float highlight_delta_scale = 1.0 -
                    highlight_soft_mask * highlight_effective_reduction;
                corrected = highlight_base + (corrected - highlight_base) * highlight_delta_scale;

                // Guard individual channels after the soft shoulder. This also
                // catches highly saturated colors whose luminance is not near white.
                // The correction delta is scaled uniformly so hue is preserved, and
                // the ceiling can never be lower than the original scene peak.
                const float highlight_guard_peak = max(corrected.r, max(corrected.g, corrected.b));
                const float highlight_guard_ceiling = max(highlight_base_peak, 0.985);
                const float highlight_guard_delta = max(highlight_guard_peak - highlight_base_peak, 0.0);
                const float highlight_guard_headroom = max(highlight_guard_ceiling - highlight_base_peak, 0.0);
                const float highlight_guard_needed = step(
                    highlight_guard_headroom + 0.000001,
                    highlight_guard_delta
                ) * highlight_activation;
                const float highlight_guard_scale = saturate(
                    highlight_guard_headroom / max(highlight_guard_delta, 0.000001)
                );
                corrected = highlight_base + (corrected - highlight_base) *
                    lerp(1.0, highlight_guard_scale, highlight_guard_needed);
                )HLSL";

            shader_source.insert(color_apply_position, HIGHLIGHT_PROTECTION);
        }

        // Debanding grades neighboring samples independently. Apply the same B/C/S
        // delta-only soft shoulder before those samples are saturated so debanding
        // cannot reintroduce clipping around protected highlights. The debanding
        // neighborhood already provides spatial support for the diffuse-area test.
        inline void inject_deband(std::string &shader_source) {
            constexpr const char *DEBAND_APPLY =
                "deband_corrected = lerp(deband_raw, saturate(deband_corrected), saturate(color_options.w));";
            const auto deband_apply_position = shader_source.find(DEBAND_APPLY);
            if(deband_apply_position == std::string::npos) {
                return;
            }

            constexpr const char *DEBAND_HIGHLIGHT_PROTECTION = R"HLSL(const float deband_highlight_base_luma = dot(deband_raw, float3(0.299, 0.587, 0.114));
                const float deband_highlight_base_peak = max(deband_raw.r, max(deband_raw.g, deband_raw.b));
                const float deband_highlight_corrected_luma = dot(deband_corrected, float3(0.299, 0.587, 0.114));

                const float deband_highlight_brightness_boost = max(color_options.x - 1.0, 0.0);
                const float deband_highlight_contrast_boost = max(color_options.y - 1.0, 0.0);
                const float deband_highlight_saturation_boost = max(color_options.z - 1.0, 0.0);
                const float deband_highlight_pressure = saturate((
                    deband_highlight_brightness_boost * 0.90 +
                    deband_highlight_contrast_boost * 1.25 +
                    deband_highlight_saturation_boost * 0.35
                ) * saturate(color_options.w));
                const float deband_highlight_activation = saturate(deband_highlight_pressure * 2.25);

                const float deband_highlight_shoulder_start = lerp(
                    0.90,
                    0.80,
                    deband_highlight_pressure
                );
                float deband_highlight_shoulder_t = saturate(
                    (deband_highlight_corrected_luma - deband_highlight_shoulder_start) /
                    max(1.0 - deband_highlight_shoulder_start, 0.0001)
                );
                deband_highlight_shoulder_t = deband_highlight_shoulder_t *
                    deband_highlight_shoulder_t *
                    (3.0 - 2.0 * deband_highlight_shoulder_t);

                const float deband_highlight_color_mix = saturate(color_options.w);
                const float deband_highlight_l_raw = dot(deband_l, float3(0.299, 0.587, 0.114));
                const float deband_highlight_r_raw = dot(deband_r, float3(0.299, 0.587, 0.114));
                const float deband_highlight_u_raw = dot(deband_u, float3(0.299, 0.587, 0.114));
                const float deband_highlight_d_raw = dot(deband_d, float3(0.299, 0.587, 0.114));
                const float deband_highlight_l_luma = lerp(
                    deband_highlight_l_raw,
                    ((deband_highlight_l_raw - 0.5) * color_options.y + 0.5) * color_options.x,
                    deband_highlight_color_mix
                );
                const float deband_highlight_r_luma = lerp(
                    deband_highlight_r_raw,
                    ((deband_highlight_r_raw - 0.5) * color_options.y + 0.5) * color_options.x,
                    deband_highlight_color_mix
                );
                const float deband_highlight_u_luma = lerp(
                    deband_highlight_u_raw,
                    ((deband_highlight_u_raw - 0.5) * color_options.y + 0.5) * color_options.x,
                    deband_highlight_color_mix
                );
                const float deband_highlight_d_luma = lerp(
                    deband_highlight_d_raw,
                    ((deband_highlight_d_raw - 0.5) * color_options.y + 0.5) * color_options.x,
                    deband_highlight_color_mix
                );
                const float deband_highlight_area_support_start = max(
                    deband_highlight_shoulder_start - 0.04,
                    0.72
                );
                float deband_highlight_sl = saturate(
                    (deband_highlight_l_luma - deband_highlight_area_support_start) / 0.14
                );
                float deband_highlight_sr = saturate(
                    (deband_highlight_r_luma - deband_highlight_area_support_start) / 0.14
                );
                float deband_highlight_su = saturate(
                    (deband_highlight_u_luma - deband_highlight_area_support_start) / 0.14
                );
                float deband_highlight_sd = saturate(
                    (deband_highlight_d_luma - deband_highlight_area_support_start) / 0.14
                );
                deband_highlight_sl = deband_highlight_sl * deband_highlight_sl * (3.0 - 2.0 * deband_highlight_sl);
                deband_highlight_sr = deband_highlight_sr * deband_highlight_sr * (3.0 - 2.0 * deband_highlight_sr);
                deband_highlight_su = deband_highlight_su * deband_highlight_su * (3.0 - 2.0 * deband_highlight_su);
                deband_highlight_sd = deband_highlight_sd * deband_highlight_sd * (3.0 - 2.0 * deband_highlight_sd);
                float deband_highlight_diffuse_support = min(
                    max(deband_highlight_sl, deband_highlight_sr),
                    max(deband_highlight_su, deband_highlight_sd)
                );
                deband_highlight_diffuse_support = deband_highlight_diffuse_support *
                    deband_highlight_diffuse_support *
                    (3.0 - 2.0 * deband_highlight_diffuse_support);

                const float deband_highlight_diffuse_start = max(
                    deband_highlight_shoulder_start - 0.05,
                    0.70
                );
                float deband_highlight_diffuse_shoulder_t = saturate(
                    (deband_highlight_corrected_luma - deband_highlight_diffuse_start) /
                    max(1.0 - deband_highlight_diffuse_start, 0.0001)
                );
                deband_highlight_diffuse_shoulder_t = deband_highlight_diffuse_shoulder_t *
                    deband_highlight_diffuse_shoulder_t *
                    (3.0 - 2.0 * deband_highlight_diffuse_shoulder_t);
                const float deband_highlight_effective_shoulder = max(
                    deband_highlight_shoulder_t,
                    deband_highlight_diffuse_support * deband_highlight_diffuse_shoulder_t * 0.95
                );

                const float deband_highlight_positive_gain = step(
                    deband_highlight_base_luma + 0.000001,
                    deband_highlight_corrected_luma
                );
                const float deband_highlight_reduction = lerp(
                    0.25,
                    0.70,
                    deband_highlight_pressure
                );
                const float deband_highlight_diffuse_extra = lerp(
                    0.15,
                    0.35,
                    deband_highlight_pressure
                );
                const float deband_highlight_effective_reduction = saturate(
                    deband_highlight_reduction +
                    deband_highlight_diffuse_support * deband_highlight_diffuse_extra
                );
                const float deband_highlight_soft_mask = saturate(
                    deband_highlight_effective_shoulder *
                    deband_highlight_activation *
                    deband_highlight_positive_gain
                );
                const float deband_highlight_delta_scale = 1.0 -
                    deband_highlight_soft_mask * deband_highlight_effective_reduction;
                deband_corrected = deband_raw +
                    (deband_corrected - deband_raw) * deband_highlight_delta_scale;

                const float deband_highlight_guard_peak = max(
                    deband_corrected.r,
                    max(deband_corrected.g, deband_corrected.b)
                );
                const float deband_highlight_guard_ceiling = max(
                    deband_highlight_base_peak,
                    0.985
                );
                const float deband_highlight_guard_delta = max(
                    deband_highlight_guard_peak - deband_highlight_base_peak,
                    0.0
                );
                const float deband_highlight_guard_headroom = max(
                    deband_highlight_guard_ceiling - deband_highlight_base_peak,
                    0.0
                );
                const float deband_highlight_guard_needed = step(
                    deband_highlight_guard_headroom + 0.000001,
                    deband_highlight_guard_delta
                ) * deband_highlight_activation;
                const float deband_highlight_guard_scale = saturate(
                    deband_highlight_guard_headroom /
                    max(deband_highlight_guard_delta, 0.000001)
                );
                deband_corrected = deband_raw + (deband_corrected - deband_raw) *
                    lerp(1.0, deband_highlight_guard_scale, deband_highlight_guard_needed);
                )HLSL";

            shader_source.insert(deband_apply_position, DEBAND_HIGHLIGHT_PROTECTION);
        }

        inline HRESULT compile(
            LPCVOID source_data,
            SIZE_T source_size,
            LPCSTR source_name,
            D3D_SHADER_MACRO *defines,
            ID3DInclude *include,
            LPCSTR entrypoint,
            LPCSTR target,
            UINT flags1,
            UINT flags2,
            ID3DBlob **code,
            ID3DBlob **errors
        ) {
            if(!source_data || source_size == 0) {
                return ::D3DCompile(
                    source_data,
                    source_size,
                    source_name,
                    defines,
                    include,
                    entrypoint,
                    target,
                    flags1,
                    flags2,
                    code,
                    errors
                );
            }

            std::string protected_source(
                static_cast<const char *>(source_data),
                static_cast<std::size_t>(source_size)
            );
            inject(protected_source);
            inject_deband(protected_source);

            return ::D3DCompile(
                protected_source.data(),
                protected_source.size(),
                source_name,
                defines,
                include,
                entrypoint,
                target,
                flags1,
                flags2,
                code,
                errors
            );
        }
    }

    // rasterizer.cpp passes a mutable D3D_SHADER_MACRO pointer to D3DCompile.
    // Keep that exact pointer type here so MinGW prefers this namespace-local
    // overload over the SDK's const-pointer declaration instead of reporting an
    // ambiguous call. The wrapper itself still calls the original ::D3DCompile.
    inline HRESULT D3DCompile(
        LPCVOID source_data,
        SIZE_T source_size,
        LPCSTR source_name,
        D3D_SHADER_MACRO *defines,
        ID3DInclude *include,
        LPCSTR entrypoint,
        LPCSTR target,
        UINT flags1,
        UINT flags2,
        ID3DBlob **code,
        ID3DBlob **errors
    ) {
        return HighlightProtection::compile(
            source_data,
            source_size,
            source_name,
            defines,
            include,
            entrypoint,
            target,
            flags1,
            flags2,
            code,
            errors
        );
    }
}

#endif
