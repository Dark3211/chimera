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

            void consider_history(
                float4 candidate,
                float3 current_ycocg,
                float penalty,
                inout float4 best_history,
                inout float best_score,
                inout float best_error
            ) {
                const float candidate_error = history_error(rgb_to_ycocg(candidate.rgb), current_ycocg);
                const float candidate_score = candidate_error + penalty;
                if(candidate_score < best_score) {
                    best_history = candidate;
                    best_score = candidate_score;
                    best_error = candidate_error;
                }
            }

            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                const float4 current = tex2D(current_sampler, uv);
                const float4 previous_center = tex2D(previous_sampler, uv);

                if(frame_options.z <= 0.5) {
                    return lerp(current, previous_center, 0.5);
                }

                const float2 t = frame_options.xy;

                const float4 current_l = tex2D(current_sampler, uv + float2(-t.x, 0.0));
                const float4 current_r = tex2D(current_sampler, uv + float2(t.x, 0.0));
                const float4 current_u = tex2D(current_sampler, uv + float2(0.0, -t.y));
                const float4 current_d = tex2D(current_sampler, uv + float2(0.0, t.y));
                const float4 current_ul = tex2D(current_sampler, uv + float2(-t.x, -t.y));
                const float4 current_ur = tex2D(current_sampler, uv + float2(t.x, -t.y));
                const float4 current_dl = tex2D(current_sampler, uv + float2(-t.x, t.y));
                const float4 current_dr = tex2D(current_sampler, uv + float2(t.x, t.y));

                const float3 c0 = rgb_to_ycocg(current.rgb);
                const float3 c1 = rgb_to_ycocg(current_l.rgb);
                const float3 c2 = rgb_to_ycocg(current_r.rgb);
                const float3 c3 = rgb_to_ycocg(current_u.rgb);
                const float3 c4 = rgb_to_ycocg(current_d.rgb);
                const float3 c5 = rgb_to_ycocg(current_ul.rgb);
                const float3 c6 = rgb_to_ycocg(current_ur.rgb);
                const float3 c7 = rgb_to_ycocg(current_dl.rgb);
                const float3 c8 = rgb_to_ycocg(current_dr.rgb);

                const float3 mean = (c0 + c1 + c2 + c3 + c4 + c5 + c6 + c7 + c8) / 9.0;
                const float3 moment2 =
                    (c0 * c0 + c1 * c1 + c2 * c2 + c3 * c3 + c4 * c4 +
                     c5 * c5 + c6 * c6 + c7 * c7 + c8 * c8) / 9.0;
                const float3 sigma = sqrt(max(moment2 - mean * mean, 0.0));

                float3 neighborhood_min = min(c0, min(min(c1, c2), min(c3, c4)));
                neighborhood_min = min(neighborhood_min, min(min(c5, c6), min(c7, c8)));
                float3 neighborhood_max = max(c0, max(max(c1, c2), max(c3, c4)));
                neighborhood_max = max(neighborhood_max, max(max(c5, c6), max(c7, c8)));

                const float3 local_span = neighborhood_max - neighborhood_min;
                const float3 margin = float3(0.010, 0.018, 0.018) + local_span * 0.06;
                const float3 variance_radius = sigma * float3(2.75, 2.50, 2.50);

                const float3 clip_min = max(
                    neighborhood_min - margin,
                    mean - variance_radius - margin * 0.5
                );
                const float3 clip_max = min(
                    neighborhood_max + margin,
                    mean + variance_radius + margin * 0.5
                );

                float4 best_history = previous_center;
                float best_error = history_error(rgb_to_ycocg(previous_center.rgb), c0);
                float best_score = best_error;

                consider_history(tex2D(previous_sampler, uv + float2(-t.x, 0.0)), c0, 0.018, best_history, best_score, best_error);
                consider_history(tex2D(previous_sampler, uv + float2(t.x, 0.0)), c0, 0.018, best_history, best_score, best_error);
                consider_history(tex2D(previous_sampler, uv + float2(0.0, -t.y)), c0, 0.018, best_history, best_score, best_error);
                consider_history(tex2D(previous_sampler, uv + float2(0.0, t.y)), c0, 0.018, best_history, best_score, best_error);
                consider_history(tex2D(previous_sampler, uv + float2(-t.x, -t.y)), c0, 0.026, best_history, best_score, best_error);
                consider_history(tex2D(previous_sampler, uv + float2(t.x, -t.y)), c0, 0.026, best_history, best_score, best_error);
                consider_history(tex2D(previous_sampler, uv + float2(-t.x, t.y)), c0, 0.026, best_history, best_score, best_error);
                consider_history(tex2D(previous_sampler, uv + float2(t.x, t.y)), c0, 0.026, best_history, best_score, best_error);

                const float3 history_ycocg = rgb_to_ycocg(best_history.rgb);
                const float3 clipped_ycocg = clamp(history_ycocg, clip_min, clip_max);
                const float clip_error = history_error(history_ycocg, clipped_ycocg);

                const float local_complexity = local_span.x + 0.20 * (local_span.y + local_span.z);
                const float reject_start = 0.050 + local_complexity * 0.20;
                const float reject_end = 0.220 + local_complexity * 0.45;
                const float mismatch = max(best_error, clip_error * 2.5);
                const float confidence =
                    1.0 - smoothstep(reject_start, max(reject_end, reject_start + 0.001), mismatch);

                const float hard_change =
                    max(abs(c0.x - history_ycocg.x),
                        0.35 * max(abs(c0.y - history_ycocg.y), abs(c0.z - history_ycocg.z)));
                const float hard_start = 0.180 + local_span.x * 0.35;
                const float hard_end = 0.420 + local_span.x * 0.70;
                const float hard_confidence =
                    1.0 - smoothstep(hard_start, max(hard_end, hard_start + 0.001), hard_change);

                const float history_weight = 0.5 * confidence * hard_confidence;
                const float3 history_rgb = saturate(ycocg_to_rgb(clipped_ycocg));
                const float3 resolved_rgb = lerp(current.rgb, history_rgb, history_weight);
                const float resolved_alpha = lerp(current.a, best_history.a, history_weight);
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
