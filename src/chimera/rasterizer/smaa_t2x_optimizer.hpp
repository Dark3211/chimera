// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_RASTERIZER_SMAA_T2X_OPTIMIZER_HPP
#define CHIMERA_RASTERIZER_SMAA_T2X_OPTIMIZER_HPP

#include "smaa.hpp"

namespace Chimera {
    namespace SMAAT2XOptimizer {
        static constexpr const char *RESOLVE_SHADER = R"HLSL(
            sampler2D current_sampler : register(s0);
            sampler2D previous_sampler : register(s1);
            float4 frame_options : register(c0);

            float3 rgb_to_ycocg(float3 color) {
                const float y = dot(color, float3(0.25, 0.5, 0.25));
                const float co = color.r - color.b;
                const float cg = color.g - 0.5 * (color.r + color.b);
                return float3(y, co, cg);
            }

            float3 ycocg_to_rgb(float3 color) {
                const float t = color.x - 0.5 * color.z;
                const float g = color.z + t;
                const float r = t + 0.5 * color.y;
                const float b = t - 0.5 * color.y;
                return float3(r, g, b);
            }

            float history_error(float3 a, float3 b) {
                const float3 delta = abs(a - b);
                return delta.x + 0.25 * (delta.y + delta.z);
            }

            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                const float4 current = tex2D(current_sampler, uv);
                const float4 previous = tex2D(previous_sampler, uv);

                if(frame_options.z <= 0.5) {
                    return lerp(current, previous, 0.5);
                }

                const float2 t = frame_options.xy;
                const float resolution_factor = saturate(
                    (max(t.x, t.y) - (1.0 / 1080.0)) /
                    ((1.0 / 480.0) - (1.0 / 1080.0))
                );

                const float3 current_ycocg = rgb_to_ycocg(current.rgb);
                const float3 history_ycocg = rgb_to_ycocg(previous.rgb);
                const float3 gradient = max(
                    abs(ddx(current_ycocg)),
                    abs(ddy(current_ycocg))
                );

                const float3 base_radius = lerp(
                    float3(0.012, 0.020, 0.020),
                    float3(0.040, 0.060, 0.060),
                    resolution_factor
                );
                const float gradient_scale = lerp(1.25, 2.10, resolution_factor);
                const float3 radius = base_radius + gradient * gradient_scale;
                const float3 clipped_history = clamp(
                    history_ycocg,
                    current_ycocg - radius,
                    current_ycocg + radius
                );

                const float clip_error = history_error(history_ycocg, clipped_history);
                const float gradient_energy =
                    gradient.x + 0.20 * (gradient.y + gradient.z);
                const float reject_start =
                    lerp(0.028, 0.065, resolution_factor) + gradient_energy * 0.10;
                const float reject_end =
                    lerp(0.125, 0.260, resolution_factor) + gradient_energy * 0.30;
                const float confidence = 1.0 - smoothstep(
                    reject_start,
                    max(reject_end, reject_start + 0.001),
                    clip_error
                );

                const float history_weight = 0.5 * confidence;
                const float3 history_rgb = saturate(ycocg_to_rgb(clipped_history));
                const float3 resolved_rgb = lerp(current.rgb, history_rgb, history_weight);
                const float resolved_alpha = lerp(current.a, previous.a, history_weight);
                return float4(resolved_rgb, resolved_alpha);
            }
        )HLSL";

        inline IDirect3DPixelShader9 *&installed_shader() noexcept {
            static IDirect3DPixelShader9 *shader = nullptr;
            return shader;
        }

        inline void install() noexcept {
            auto &s = SMAA::state();
            auto &installed = installed_shader();

            if(!SMAA::temporal_requested() || s.runtime_disabled || !s.jitter_applied) {
                return;
            }
            if(!s.resolve_shader || !s.device) {
                if(!s.resolve_shader) {
                    installed = nullptr;
                }
                return;
            }
            if(!global_d3d9_device || !*global_d3d9_device || s.device != *global_d3d9_device) {
                return;
            }
            if(s.resolve_shader == installed) {
                return;
            }

            IDirect3DPixelShader9 *optimized = nullptr;
            if(!SMAA::create_shader(s.device, RESOLVE_SHADER, &optimized) || !optimized) {
                return;
            }

            SMAA::release_com(s.resolve_shader);
            s.resolve_shader = optimized;
            installed = optimized;
        }
    }
}

#endif
