// SPDX-License-Identifier: GPL-3.0-only
//
// The SMAA shader equations in this file are adapted from the reference SMAA
// implementation by Jorge Jimenez, Jose I. Echevarria, Belen Masia,
// Fernando Navarro and Diego Gutierrez (https://github.com/iryoku/smaa),
// distributed under the MIT license. The Chimera/D3D9 integration is GPLv3.

#ifndef CHIMERA_RASTERIZER_SMAA_HPP
#define CHIMERA_RASTERIZER_SMAA_HPP

#include <cmath>
#include <cstdint>
#include <cstring>
#include <d3d9.h>
#include <d3dcompiler.h>

#include "rasterizer.hpp"
#include "enhanced_graphics.hpp"
#include "graphics_runtime_metrics.hpp"
#include "smaa_lut_generator.hpp"
#include "../chimera.hpp"
#include "../event/d3d9_reset.hpp"
#include "../event/game_loop.hpp"
#include "../halo_data/game_functions.hpp"
#include "../halo_data/game_variables.hpp"
#include "../output/output.hpp"
#include "../signature/hook.hpp"

namespace Chimera {
    namespace SMAA {
        inline bool requested() noexcept {
            const auto *ini = get_chimera().get_ini();
            if(!ini) {
                return false;
            }
            const char *aa = ini->get_value("graphics.anti_aliasing");
            return aa && (
                std::strcmp(aa, "smaa") == 0 ||
                std::strcmp(aa, "SMAA") == 0 ||
                std::strcmp(aa, "smaa_t2x") == 0 ||
                std::strcmp(aa, "SMAA_T2X") == 0
            );
        }

        inline bool temporal_requested() noexcept {
            const auto *ini = get_chimera().get_ini();
            if(!ini) {
                return false;
            }
            const char *aa = ini->get_value("graphics.anti_aliasing");
            return aa && (
                std::strcmp(aa, "smaa_t2x") == 0 ||
                std::strcmp(aa, "SMAA_T2X") == 0
            );
        }

        struct State {
            IDirect3DDevice9 *device = nullptr;

            IDirect3DTexture9 *edges_texture = nullptr;
            IDirect3DSurface9 *edges_surface = nullptr;
            IDirect3DTexture9 *weights_texture = nullptr;
            IDirect3DSurface9 *weights_surface = nullptr;
            IDirect3DTexture9 *area_texture = nullptr;
            IDirect3DTexture9 *search_texture = nullptr;

            // T2x stores the spatial SMAA result for the current jittered sample and
            // the previous jittered sample. The official non-reprojection resolve is
            // then a 50/50 point-sampled blend of these two textures.
            IDirect3DTexture9 *current_texture = nullptr;
            IDirect3DSurface9 *current_surface = nullptr;
            IDirect3DTexture9 *history_texture = nullptr;
            IDirect3DSurface9 *history_surface = nullptr;

            IDirect3DPixelShader9 *edge_shader = nullptr;
            IDirect3DPixelShader9 *weight_shader = nullptr;
            IDirect3DPixelShader9 *neighborhood_shader = nullptr;
            IDirect3DPixelShader9 *resolve_shader = nullptr;
            IDirect3DPixelShader9 *copy_shader = nullptr;

            UINT width = 0;
            UINT height = 0;
            D3DFORMAT color_format = D3DFMT_UNKNOWN;
            bool runtime_disabled = false;
            bool failure_reported = false;
            bool processing = false;
            bool history_valid = false;
            bool jitter_applied = false;
            bool history_clamp = true;
            std::uint32_t temporal_sample = 0;
            std::uint32_t tracked_resources = 0;
        };

        inline State &state() noexcept {
            static State instance;
            return instance;
        }

        template<typename T> inline void release_com(T *&resource) noexcept {
            if(resource) {
                resource->Release();
                resource = nullptr;
            }
        }

        inline void release_resources() noexcept {
            auto &s = state();
            if(s.tracked_resources != 0) {
                GraphicsRuntimeMetrics::resources_released(s.tracked_resources);
                s.tracked_resources = 0;
            }
            release_com(s.copy_shader);
            release_com(s.resolve_shader);
            release_com(s.neighborhood_shader);
            release_com(s.weight_shader);
            release_com(s.edge_shader);
            release_com(s.history_surface);
            release_com(s.history_texture);
            release_com(s.current_surface);
            release_com(s.current_texture);
            release_com(s.search_texture);
            release_com(s.area_texture);
            release_com(s.weights_surface);
            release_com(s.weights_texture);
            release_com(s.edges_surface);
            release_com(s.edges_texture);
            s.device = nullptr;
            s.width = 0;
            s.height = 0;
            s.color_format = D3DFMT_UNKNOWN;
            s.processing = false;
            s.history_valid = false;
            s.jitter_applied = false;
            s.temporal_sample = 0;
        }

        inline void report_failure_once(const char *message) noexcept {
            auto &s = state();
            if(!s.failure_reported) {
                console_error(message);
                s.failure_reported = true;
            }
        }

        inline void disable_for_session(const char *message) noexcept {
            report_failure_once(message);
            release_resources();
            state().runtime_disabled = true;
        }

        inline bool create_shader(
            IDirect3DDevice9 *device,
            const char *source,
            IDirect3DPixelShader9 **shader
        ) noexcept {
            if(!device || !source || !shader) {
                return false;
            }
            *shader = nullptr;

            ID3DBlob *compiled = nullptr;
            if(!rasterizer_compile_shader(source, "main", "ps_3_0", nullptr, &compiled) || !compiled) {
                EnhancedGraphics::release_com(compiled);
                return false;
            }

            const HRESULT result = IDirect3DDevice9_CreatePixelShader(
                device,
                reinterpret_cast<const DWORD *>(compiled->GetBufferPointer()),
                shader
            );
            EnhancedGraphics::release_com(compiled);
            if(FAILED(result) || !*shader) {
                *shader = nullptr;
                return false;
            }
            return true;
        }

        inline bool create_render_target(
            IDirect3DDevice9 *device,
            UINT width,
            UINT height,
            IDirect3DTexture9 **texture,
            IDirect3DSurface9 **surface
        ) noexcept {
            if(!device || !texture || !surface || width == 0 || height == 0) {
                return false;
            }

            *texture = nullptr;
            *surface = nullptr;
            HRESULT result = IDirect3DDevice9_CreateTexture(
                device,
                width,
                height,
                1,
                D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT,
                texture,
                nullptr
            );
            if(FAILED(result) || !*texture) {
                return false;
            }

            result = IDirect3DTexture9_GetSurfaceLevel(*texture, 0, surface);
            if(FAILED(result) || !*surface) {
                release_com(*texture);
                return false;
            }
            return true;
        }

        // Reference SMAA Ultra luma edge detection. This is the luma variant of
        // SMAALumaEdgeDetectionPS adapted to a fullscreen ps_3_0 pass.
        static constexpr const char *EDGE_SHADER = R"HLSL(
            sampler2D frame_sampler : register(s0);
            float4 metrics : register(c0); // inv width, inv height, width, height

            float luma(float3 c) {
                return dot(c, float3(0.2126, 0.7152, 0.0722));
            }

            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                const float2 t = metrics.xy;
                const float L = luma(tex2D(frame_sampler, uv).rgb);
                const float Lleft = luma(tex2D(frame_sampler, uv + float2(-t.x, 0.0)).rgb);
                const float Ltop = luma(tex2D(frame_sampler, uv + float2(0.0, -t.y)).rgb);

                float4 delta = 0.0;
                delta.xy = abs(L.xx - float2(Lleft, Ltop));
                float2 edges = step(0.05.xx, delta.xy);
                if(dot(edges, 1.0.xx) == 0.0) {
                    return 0.0;
                }

                const float Lright = luma(tex2D(frame_sampler, uv + float2(t.x, 0.0)).rgb);
                const float Lbottom = luma(tex2D(frame_sampler, uv + float2(0.0, t.y)).rgb);
                delta.zw = abs(L.xx - float2(Lright, Lbottom));
                float2 max_delta = max(delta.xy, delta.zw);

                const float Lleftleft = luma(tex2D(frame_sampler, uv + float2(-2.0 * t.x, 0.0)).rgb);
                const float Ltoptop = luma(tex2D(frame_sampler, uv + float2(0.0, -2.0 * t.y)).rgb);
                delta.zw = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));
                max_delta = max(max_delta, delta.zw);
                const float final_delta = max(max_delta.x, max_delta.y);

                edges *= step(final_delta.xx, 2.0 * delta.xy);
                return float4(edges, 0.0, 0.0);
            }
        )HLSL";

        // Reference SMAA 1x/T2x blending-weight pass. The same AreaTex/SearchTex
        // equations are used by both modes. T2x supplies the official per-jitter
        // subsample indices through c1; 1x supplies zero.
        static constexpr const char *WEIGHT_SHADER = R"HLSL(
            sampler2D edge_sampler : register(s0);
            sampler2D area_sampler : register(s1);
            sampler2D search_sampler : register(s2);
            float4 metrics : register(c0); // inv width, inv height, width, height
            float4 subsample_indices : register(c1);

            float4 edge4(float2 uv) {
                return tex2Dlod(edge_sampler, float4(uv, 0.0, 0.0));
            }
            float4 edge_offset(float2 uv, float2 offset) {
                return edge4(uv + offset * metrics.xy);
            }
            float4 area4(float2 uv) {
                return tex2Dlod(area_sampler, float4(uv, 0.0, 0.0));
            }
            float4 search4(float2 uv) {
                return tex2Dlod(search_sampler, float4(uv, 0.0, 0.0));
            }

            float2 decode_diag(float2 e) {
                e.r = e.r * abs(5.0 * e.r - 3.75);
                return round(e);
            }
            float4 decode_diag4(float4 e) {
                e.r = e.r * abs(5.0 * e.r - 3.75);
                e.b = e.b * abs(5.0 * e.b - 3.75);
                return round(e);
            }

            float2 search_diag1(float2 uv, float2 dir, out float2 e) {
                float distance = -1.0;
                float continuation = 1.0;
                e = 0.0;
                [loop]
                for(int i = 0; i < 16; i++) {
                    if(distance >= 15.0 || continuation <= 0.9) break;
                    uv += dir * metrics.xy;
                    distance += 1.0;
                    e = edge4(uv).rg;
                    continuation = dot(e, 0.5.xx);
                }
                return float2(distance, continuation);
            }

            float2 search_diag2(float2 uv, float2 dir, out float2 e) {
                float distance = -1.0;
                float continuation = 1.0;
                uv.x += 0.25 * metrics.x;
                e = 0.0;
                [loop]
                for(int i = 0; i < 16; i++) {
                    if(distance >= 15.0 || continuation <= 0.9) break;
                    uv += dir * metrics.xy;
                    distance += 1.0;
                    e = decode_diag(edge4(uv).rg);
                    continuation = dot(e, 0.5.xx);
                }
                return float2(distance, continuation);
            }

            float2 area_diag(float2 distance, float2 e, float offset) {
                float2 tc = 20.0 * e + distance;
                tc = tc * (1.0 / float2(160.0, 560.0)) + 0.5 / float2(160.0, 560.0);
                tc.x += 0.5;
                tc.y += (1.0 / 7.0) * offset;
                return area4(tc).rg;
            }

            float2 calculate_diag_weights(float2 uv, float2 e) {
                float2 weights = 0.0;
                float4 d = 0.0;
                float2 end_edge = 0.0;

                if(e.r > 0.0) {
                    d.xz = search_diag1(uv, float2(-1.0, 1.0), end_edge);
                    d.x += (end_edge.y > 0.9) ? 1.0 : 0.0;
                }
                d.yw = search_diag1(uv, float2(1.0, -1.0), end_edge);

                if(d.x + d.y > 2.0) {
                    const float4 coords = uv.xyxy +
                        float4(-d.x + 0.25, d.x, d.y, -d.y - 0.25) * metrics.xyxy;
                    float4 c;
                    c.xy = edge_offset(coords.xy, float2(-1.0, 0.0)).rg;
                    c.zw = edge_offset(coords.zw, float2(1.0, 0.0)).rg;
                    c.yxwz = decode_diag4(c.xyzw);
                    float2 crossing = 2.0 * c.xz + c.yw;
                    if(d.z >= 0.9) crossing.x = 0.0;
                    if(d.w >= 0.9) crossing.y = 0.0;
                    weights += area_diag(d.xy, crossing, subsample_indices.z);
                }

                d = 0.0;
                d.xz = search_diag2(uv, float2(-1.0, -1.0), end_edge);
                if(edge_offset(uv, float2(1.0, 0.0)).r > 0.0) {
                    d.yw = search_diag2(uv, float2(1.0, 1.0), end_edge);
                    d.y += (end_edge.y > 0.9) ? 1.0 : 0.0;
                }

                if(d.x + d.y > 2.0) {
                    const float4 coords = uv.xyxy +
                        float4(-d.x, -d.x, d.y, d.y) * metrics.xyxy;
                    float4 c;
                    c.x = edge_offset(coords.xy, float2(-1.0, 0.0)).g;
                    c.y = edge_offset(coords.xy, float2(0.0, -1.0)).r;
                    c.zw = edge_offset(coords.zw, float2(1.0, 0.0)).gr;
                    float2 crossing = 2.0 * c.xz + c.yw;
                    if(d.z >= 0.9) crossing.x = 0.0;
                    if(d.w >= 0.9) crossing.y = 0.0;
                    weights += area_diag(d.xy, crossing, subsample_indices.w).gr;
                }
                return weights;
            }

            float search_length(float2 e, float offset) {
                float2 scale = float2(66.0, 33.0) * float2(0.5, -1.0);
                float2 bias = float2(66.0, 33.0) * float2(offset, 1.0);
                scale += float2(-1.0, 1.0);
                bias += float2(0.5, -0.5);
                scale *= 1.0 / float2(64.0, 16.0);
                bias *= 1.0 / float2(64.0, 16.0);
                return search4(scale * e + bias).r;
            }

            float search_x_left(float2 uv, float end) {
                float2 e = float2(0.0, 1.0);
                [loop]
                for(int i = 0; i < 32; i++) {
                    if(!(uv.x > end && e.g > 0.8281 && e.r == 0.0)) break;
                    e = edge4(uv).rg;
                    uv -= float2(2.0 * metrics.x, 0.0);
                }
                const float offset = -(255.0 / 127.0) * search_length(e, 0.0) + 3.25;
                return uv.x + metrics.x * offset;
            }

            float search_x_right(float2 uv, float end) {
                float2 e = float2(0.0, 1.0);
                [loop]
                for(int i = 0; i < 32; i++) {
                    if(!(uv.x < end && e.g > 0.8281 && e.r == 0.0)) break;
                    e = edge4(uv).rg;
                    uv += float2(2.0 * metrics.x, 0.0);
                }
                const float offset = -(255.0 / 127.0) * search_length(e, 0.5) + 3.25;
                return uv.x - metrics.x * offset;
            }

            float search_y_up(float2 uv, float end) {
                float2 e = float2(1.0, 0.0);
                [loop]
                for(int i = 0; i < 32; i++) {
                    if(!(uv.y > end && e.r > 0.8281 && e.g == 0.0)) break;
                    e = edge4(uv).rg;
                    uv -= float2(0.0, 2.0 * metrics.y);
                }
                const float offset = -(255.0 / 127.0) * search_length(e.gr, 0.0) + 3.25;
                return uv.y + metrics.y * offset;
            }

            float search_y_down(float2 uv, float end) {
                float2 e = float2(1.0, 0.0);
                [loop]
                for(int i = 0; i < 32; i++) {
                    if(!(uv.y < end && e.r > 0.8281 && e.g == 0.0)) break;
                    e = edge4(uv).rg;
                    uv += float2(0.0, 2.0 * metrics.y);
                }
                const float offset = -(255.0 / 127.0) * search_length(e.gr, 0.5) + 3.25;
                return uv.y - metrics.y * offset;
            }

            float2 area(float2 distance, float e1, float e2, float offset) {
                float2 tc = 16.0 * round(4.0 * float2(e1, e2)) + distance;
                tc = tc * (1.0 / float2(160.0, 560.0)) + 0.5 / float2(160.0, 560.0);
                tc.y += (1.0 / 7.0) * offset;
                return area4(tc).rg;
            }

            void detect_horizontal_corner(inout float2 weights, float4 tc, float2 d) {
                const float2 left_right = step(d.xy, d.yx);
                float2 rounding = 0.75 * left_right;
                rounding /= max(left_right.x + left_right.y, 1.0e-5);
                float2 factor = 1.0.xx;
                factor.x -= rounding.x * edge_offset(tc.xy, float2(0.0, 1.0)).r;
                factor.x -= rounding.y * edge_offset(tc.zw, float2(1.0, 1.0)).r;
                factor.y -= rounding.x * edge_offset(tc.xy, float2(0.0, -2.0)).r;
                factor.y -= rounding.y * edge_offset(tc.zw, float2(1.0, -2.0)).r;
                weights *= saturate(factor);
            }

            void detect_vertical_corner(inout float2 weights, float4 tc, float2 d) {
                const float2 left_right = step(d.xy, d.yx);
                float2 rounding = 0.75 * left_right;
                rounding /= max(left_right.x + left_right.y, 1.0e-5);
                float2 factor = 1.0.xx;
                factor.x -= rounding.x * edge_offset(tc.xy, float2(1.0, 0.0)).g;
                factor.x -= rounding.y * edge_offset(tc.zw, float2(1.0, 1.0)).g;
                factor.y -= rounding.x * edge_offset(tc.xy, float2(-2.0, 0.0)).g;
                factor.y -= rounding.y * edge_offset(tc.zw, float2(-2.0, 1.0)).g;
                weights *= saturate(factor);
            }

            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                float4 weights = 0.0;
                float2 e = edge4(uv).rg;
                const float2 pixcoord = uv * metrics.zw;
                const float4 offset0 = uv.xyxy + metrics.xyxy * float4(-0.25, -0.125, 1.25, -0.125);
                const float4 offset1 = uv.xyxy + metrics.xyxy * float4(-0.125, -0.25, -0.125, 1.25);
                const float4 offset2 =
                    float4(offset0.x, offset0.z, offset1.y, offset1.w) +
                    metrics.xxyy * float4(-64.0, 64.0, -64.0, 64.0);

                if(e.g > 0.0) {
                    weights.rg = calculate_diag_weights(uv, e);
                    if(weights.r + weights.g < 1.0e-5) {
                        float3 coords;
                        coords.x = search_x_left(offset0.xy, offset2.x);
                        coords.y = offset1.y;
                        float e1 = edge4(coords.xy).r;
                        coords.z = search_x_right(offset0.zw, offset2.y);
                        float2 d = abs(round(metrics.zz * float2(coords.x, coords.z) - pixcoord.xx));
                        float e2 = edge_offset(coords.zy, float2(1.0, 0.0)).r;
                        weights.rg = area(sqrt(d), e1, e2, subsample_indices.y);
                        coords.y = uv.y;
                        detect_horizontal_corner(weights.rg, coords.xyzy, d);
                    }
                    else {
                        e.r = 0.0;
                    }
                }

                if(e.r > 0.0) {
                    float3 coords;
                    coords.y = search_y_up(offset1.xy, offset2.z);
                    coords.x = offset0.x;
                    float e1 = edge4(coords.xy).g;
                    coords.z = search_y_down(offset1.zw, offset2.w);
                    float2 d = abs(round(metrics.ww * float2(coords.y, coords.z) - pixcoord.yy));
                    float e2 = edge_offset(coords.xz, float2(0.0, 1.0)).g;
                    weights.ba = area(sqrt(d), e1, e2, subsample_indices.x);
                    coords.x = uv.x;
                    detect_vertical_corner(weights.ba, coords.xyxz, d);
                }

                return weights;
            }
        )HLSL";

        // Reference directional neighborhood blend. Chimera's existing sharpening and
        // color/post effects are retained after the SMAA blend; sharpening is strongly
        // suppressed on active antialiased edges so it does not recreate stair steps.
        static constexpr const char *NEIGHBORHOOD_SHADER = R"HLSL(
            sampler2D frame_sampler : register(s0);
            sampler2D weight_sampler : register(s1);
            sampler2D edge_sampler : register(s2);
            float4 frame_options : register(c0);
            float4 color_options : register(c1);

            float luma(float3 color) {
                return dot(color, float3(0.299, 0.587, 0.114));
            }

            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                const float2 t = frame_options.xy;
                const float4 center_sample = tex2D(frame_sampler, uv);

                const float4 offset = uv.xyxy + t.xyxy * float4(1.0, 0.0, 0.0, 1.0);
                float4 a;
                a.x = tex2D(weight_sampler, offset.xy).a;
                a.y = tex2D(weight_sampler, offset.zw).g;
                a.wz = tex2D(weight_sampler, uv).xz;

                float4 filtered = center_sample;
                const float weight_sum = dot(a, 1.0.xxxx);
                if(weight_sum >= 1.0e-5) {
                    const bool horizontal = max(a.x, a.z) > max(a.y, a.w);
                    float4 blending_offset = float4(0.0, a.y, 0.0, a.w);
                    float2 blending_weight = a.yw;
                    if(horizontal) {
                        blending_offset = float4(a.x, 0.0, a.z, 0.0);
                        blending_weight = a.xz;
                    }
                    blending_weight /= max(dot(blending_weight, 1.0.xx), 1.0e-5);
                    const float4 blending_coord =
                        uv.xyxy + blending_offset * float4(t.xy, -t.xy);
                    filtered =
                        blending_weight.x * tex2D(frame_sampler, blending_coord.xy) +
                        blending_weight.y * tex2D(frame_sampler, blending_coord.zw);
                    filtered.a = center_sample.a;
                }

                float3 color = filtered.rgb;
                const float3 left = tex2D(frame_sampler, uv + float2(-t.x, 0.0)).rgb;
                const float3 right = tex2D(frame_sampler, uv + float2(t.x, 0.0)).rgb;
                const float3 up = tex2D(frame_sampler, uv + float2(0.0, -t.y)).rgb;
                const float3 down = tex2D(frame_sampler, uv + float2(0.0, t.y)).rgb;
                const float3 cross_average = 0.25 * (left + right + up + down);
                const float3 center = center_sample.rgb;
                const float local_range = max(
                    max(abs(luma(center) - luma(left)), abs(luma(center) - luma(right))),
                    max(abs(luma(center) - luma(up)), abs(luma(center) - luma(down)))
                );
                const float2 edge = tex2D(edge_sampler, uv).rg;
                const float edge_activity = saturate(
                    max(edge.r, edge.g) * 0.65 + max(max(a.x, a.y), max(a.z, a.w)) * 1.35
                );
                const float sharpen_gain = saturate(frame_options.w) * 2.5 * (1.0 - 0.92 * edge_activity);
                const float detail_limit = 0.03 + saturate(local_range) * 0.20;
                const float3 sharp_detail = clamp(color - cross_average, -detail_limit, detail_limit);
                color = saturate(color + sharp_detail * sharpen_gain);

                const float grey = luma(color);
                float3 corrected = lerp(float3(grey, grey, grey), color, color_options.z);
                corrected = (corrected - 0.5) * color_options.y + 0.5;
                corrected *= color_options.x;
                color = lerp(color, saturate(corrected), saturate(color_options.w));

                return float4(color, center_sample.a);
            }
        )HLSL";

        // T2X resolve keeps the reference point-sampled 50/50 blend, with an optional
        // current-frame neighborhood clamp for the previous sample. Halo exposes no
        // velocity buffer here, so this conservative clamp rejects only history values
        // that are outside the current local color envelope. Set
        // smaa_t2x_history_clamp=0 for the exact reference non-reprojection resolve.
        static constexpr const char *RESOLVE_SHADER = R"HLSL(
            sampler2D current_sampler : register(s0);
            sampler2D previous_sampler : register(s1);
            float4 frame_options : register(c0); // inv width, inv height, clamp enabled, unused
            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                const float4 current = tex2D(current_sampler, uv);
                float4 previous = tex2D(previous_sampler, uv);

                if(frame_options.z > 0.5) {
                    const float2 t = frame_options.xy;
                    const float3 left = tex2D(current_sampler, uv + float2(-t.x, 0.0)).rgb;
                    const float3 right = tex2D(current_sampler, uv + float2(t.x, 0.0)).rgb;
                    const float3 up = tex2D(current_sampler, uv + float2(0.0, -t.y)).rgb;
                    const float3 down = tex2D(current_sampler, uv + float2(0.0, t.y)).rgb;

                    float3 neighborhood_min = min(current.rgb, min(min(left, right), min(up, down)));
                    float3 neighborhood_max = max(current.rgb, max(max(left, right), max(up, down)));
                    const float3 local_span = neighborhood_max - neighborhood_min;
                    const float3 margin = float3(0.012, 0.012, 0.012) + local_span * 0.08;
                    previous.rgb = clamp(previous.rgb, neighborhood_min - margin, neighborhood_max + margin);
                }

                return lerp(current, previous, 0.5);
            }
        )HLSL";

        static constexpr const char *COPY_SHADER = R"HLSL(
            sampler2D frame_sampler : register(s0);
            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                return tex2D(frame_sampler, uv);
            }
        )HLSL";

        inline bool create_area_texture(IDirect3DDevice9 *device, IDirect3DTexture9 **texture) noexcept {
            if(!device || !texture) {
                return false;
            }
            *texture = nullptr;
            HRESULT hr = IDirect3DDevice9_CreateTexture(
                device,
                static_cast<UINT>(Lut::AREA_WIDTH),
                static_cast<UINT>(Lut::AREA_HEIGHT),
                1,
                0,
                D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED,
                texture,
                nullptr
            );
            if(FAILED(hr) || !*texture) {
                return false;
            }

            D3DLOCKED_RECT locked {};
            hr = IDirect3DTexture9_LockRect(*texture, 0, &locked, nullptr, 0);
            if(FAILED(hr)) {
                release_com(*texture);
                return false;
            }

            const auto &data = Lut::area_data();
            for(std::size_t y = 0; y < Lut::AREA_HEIGHT; y++) {
                auto *row = reinterpret_cast<DWORD *>(
                    reinterpret_cast<std::uint8_t *>(locked.pBits) + y * static_cast<std::size_t>(locked.Pitch)
                );
                for(std::size_t x = 0; x < Lut::AREA_WIDTH; x++) {
                    const std::size_t i = (y * Lut::AREA_WIDTH + x) * Lut::AREA_CHANNELS;
                    row[x] = D3DCOLOR_ARGB(255, data[i], data[i + 1], 0);
                }
            }
            IDirect3DTexture9_UnlockRect(*texture, 0);
            return true;
        }

        inline bool create_search_texture(IDirect3DDevice9 *device, IDirect3DTexture9 **texture) noexcept {
            if(!device || !texture) {
                return false;
            }
            *texture = nullptr;
            HRESULT hr = IDirect3DDevice9_CreateTexture(
                device,
                static_cast<UINT>(Lut::SEARCH_WIDTH),
                static_cast<UINT>(Lut::SEARCH_HEIGHT),
                1,
                0,
                D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED,
                texture,
                nullptr
            );
            if(FAILED(hr) || !*texture) {
                return false;
            }

            D3DLOCKED_RECT locked {};
            hr = IDirect3DTexture9_LockRect(*texture, 0, &locked, nullptr, 0);
            if(FAILED(hr)) {
                release_com(*texture);
                return false;
            }

            const auto &data = Lut::search_data();
            for(std::size_t y = 0; y < Lut::SEARCH_HEIGHT; y++) {
                auto *row = reinterpret_cast<DWORD *>(
                    reinterpret_cast<std::uint8_t *>(locked.pBits) + y * static_cast<std::size_t>(locked.Pitch)
                );
                for(std::size_t x = 0; x < Lut::SEARCH_WIDTH; x++) {
                    const std::uint8_t value = data[y * Lut::SEARCH_WIDTH + x];
                    row[x] = D3DCOLOR_ARGB(255, value, 0, 0);
                }
            }
            IDirect3DTexture9_UnlockRect(*texture, 0);
            return true;
        }

        inline bool create_color_render_target(
            IDirect3DDevice9 *device,
            UINT width,
            UINT height,
            D3DFORMAT format,
            IDirect3DTexture9 **texture,
            IDirect3DSurface9 **surface
        ) noexcept {
            if(!device || !texture || !surface || width == 0 || height == 0 || format == D3DFMT_UNKNOWN) {
                return false;
            }
            *texture = nullptr;
            *surface = nullptr;
            HRESULT hr = IDirect3DDevice9_CreateTexture(
                device,
                width,
                height,
                1,
                D3DUSAGE_RENDERTARGET,
                format,
                D3DPOOL_DEFAULT,
                texture,
                nullptr
            );
            if(FAILED(hr) || !*texture) {
                return false;
            }
            hr = IDirect3DTexture9_GetSurfaceLevel(*texture, 0, surface);
            if(FAILED(hr) || !*surface) {
                release_com(*texture);
                return false;
            }
            return true;
        }

        inline bool ensure_resources(
            IDirect3DDevice9 *device,
            UINT width,
            UINT height,
            D3DFORMAT color_format
        ) noexcept {
            auto &s = state();
            const bool temporal = temporal_requested();
            const bool common_matches =
                s.device == device && s.width == width && s.height == height &&
                s.color_format == color_format &&
                s.edges_texture && s.edges_surface && s.weights_texture && s.weights_surface &&
                s.area_texture && s.search_texture &&
                s.edge_shader && s.weight_shader && s.neighborhood_shader;
            const bool temporal_matches = !temporal || (
                s.current_texture && s.current_surface && s.history_texture && s.history_surface &&
                s.resolve_shader && s.copy_shader
            );
            if(common_matches && temporal_matches) {
                return true;
            }

            // Recreating D3D resources can happen on the first T2x frame. Preserve
            // the already-selected camera sample so the weight pass uses the matching
            // official subsample indices instead of silently reverting to SMAA 1x.
            const bool pending_jitter = s.jitter_applied;
            const std::uint32_t pending_temporal_sample = s.temporal_sample;
            release_resources();
            s.jitter_applied = pending_jitter;
            s.temporal_sample = pending_temporal_sample;
            s.device = device;
            s.width = width;
            s.height = height;
            s.color_format = color_format;
            if(const auto *ini = get_chimera().get_ini()) {
                s.history_clamp = ini->get_value_bool("graphics.smaa_t2x_history_clamp").value_or(true);
            }
            else {
                s.history_clamp = true;
            }

            if(!create_shader(device, EDGE_SHADER, &s.edge_shader) ||
               !create_shader(device, WEIGHT_SHADER, &s.weight_shader) ||
               !create_shader(device, NEIGHBORHOOD_SHADER, &s.neighborhood_shader) ||
               !create_render_target(device, width, height, &s.edges_texture, &s.edges_surface) ||
               !create_render_target(device, width, height, &s.weights_texture, &s.weights_surface) ||
               !create_area_texture(device, &s.area_texture) ||
               !create_search_texture(device, &s.search_texture)) {
                release_resources();
                return false;
            }

            if(temporal) {
                if(!create_shader(device, RESOLVE_SHADER, &s.resolve_shader) ||
                   !create_shader(device, COPY_SHADER, &s.copy_shader) ||
                   !create_color_render_target(device, width, height, color_format, &s.current_texture, &s.current_surface) ||
                   !create_color_render_target(device, width, height, color_format, &s.history_texture, &s.history_surface)) {
                    release_resources();
                    return false;
                }
            }
            s.tracked_resources = temporal ? 15U : 9U;
            GraphicsRuntimeMetrics::resources_created(s.tracked_resources);
            GraphicsRuntimeMetrics::subsystem_recovered(GraphicsRuntimeMetrics::Subsystem::SMAA);
            return true;
        }

        inline void set_common_state(IDirect3DDevice9 *device) noexcept {
            IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_SRGBWRITEENABLE, FALSE);
            IDirect3DDevice9_SetVertexShader(device, nullptr);
            IDirect3DDevice9_SetFVF(device, EnhancedGraphics::SCREEN_VERTEX_FVF);
            for(DWORD sampler = 0; sampler < 3; sampler++) {
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_SRGBTEXTURE, FALSE);
            }
        }

        inline bool draw_quad(
            IDirect3DDevice9 *device,
            IDirect3DSurface9 *target,
            IDirect3DPixelShader9 *shader,
            IDirect3DBaseTexture9 *texture0,
            IDirect3DBaseTexture9 *texture1,
            IDirect3DBaseTexture9 *texture2,
            UINT width,
            UINT height,
            float option_z,
            float option_w,
            const float *subsample_indices = nullptr,
            DWORD point_sampler_mask = 0
        ) noexcept {
            if(!device || !target || !shader || !texture0 || width == 0 || height == 0) {
                return false;
            }

            // Never leave a texture bound while making it a render target in a later pass.
            for(DWORD sampler = 0; sampler < 3; sampler++) {
                IDirect3DDevice9_SetTexture(device, sampler, nullptr);
            }

            if(FAILED(IDirect3DDevice9_SetRenderTarget(device, 0, target))) {
                return false;
            }

            D3DVIEWPORT9 viewport {};
            viewport.X = 0;
            viewport.Y = 0;
            viewport.Width = width;
            viewport.Height = height;
            viewport.MinZ = 0.0f;
            viewport.MaxZ = 1.0f;
            if(FAILED(IDirect3DDevice9_SetViewport(device, &viewport))) {
                return false;
            }

            set_common_state(device);
            for(DWORD sampler = 0; sampler < 3; sampler++) {
                if((point_sampler_mask & (1U << sampler)) != 0) {
                    IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                    IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                }
            }

            IDirect3DDevice9_SetPixelShader(device, shader);
            IDirect3DDevice9_SetTexture(device, 0, texture0);
            IDirect3DDevice9_SetTexture(device, 1, texture1);
            IDirect3DDevice9_SetTexture(device, 2, texture2);

            const float constants[4] = {
                1.0f / static_cast<float>(width),
                1.0f / static_cast<float>(height),
                option_z,
                option_w
            };
            if(FAILED(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, constants, 1))) {
                return false;
            }
            if(subsample_indices && FAILED(IDirect3DDevice9_SetPixelShaderConstantF(device, 1, subsample_indices, 1))) {
                return false;
            }

            const float right = static_cast<float>(width) - 0.5f;
            const float bottom = static_cast<float>(height) - 0.5f;
            const EnhancedGraphics::ScreenVertex vertices[4] = {
                {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
                {right, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
                {-0.5f, bottom, 0.0f, 1.0f, 0.0f, 1.0f},
                {right, bottom, 0.0f, 1.0f, 1.0f, 1.0f}
            };
            const bool ok = SUCCEEDED(IDirect3DDevice9_DrawPrimitiveUP(
                device,
                D3DPT_TRIANGLESTRIP,
                2,
                vertices,
                sizeof(EnhancedGraphics::ScreenVertex)
            ));

            for(DWORD sampler = 0; sampler < 3; sampler++) {
                IDirect3DDevice9_SetTexture(device, sampler, nullptr);
            }
            return ok;
        }

        inline void subsample_indices_for_current_frame(float indices[4]) noexcept {
            if(!temporal_requested() || !state().jitter_applied) {
                indices[0] = indices[1] = indices[2] = indices[3] = 0.0f;
                return;
            }
            if((state().temporal_sample & 1U) == 0U) {
                // Official SMAA T2x sample 0: jitter (0.25, -0.25).
                indices[0] = 1.0f;
                indices[1] = 1.0f;
                indices[2] = 1.0f;
                indices[3] = 0.0f;
            }
            else {
                // Official SMAA T2x sample 1: jitter (-0.25, 0.25).
                indices[0] = 2.0f;
                indices[1] = 2.0f;
                indices[2] = 2.0f;
                indices[3] = 0.0f;
            }
        }

        inline bool render_spatial_passes(
            IDirect3DDevice9 *device,
            IDirect3DSurface9 *output_target,
            UINT width,
            UINT height
        ) noexcept {
            auto &s = state();
            auto &graphics = EnhancedGraphics::state();

            float subsample_indices[4] {};
            subsample_indices_for_current_frame(subsample_indices);

            bool ok = draw_quad(
                device,
                s.edges_surface,
                s.edge_shader,
                reinterpret_cast<IDirect3DBaseTexture9 *>(graphics.frame_texture),
                nullptr,
                nullptr,
                width,
                height,
                static_cast<float>(width),
                static_cast<float>(height)
            );

            if(ok) {
                // The official DX9 SMAA sampler uses point filtering for SearchTex.
                ok = draw_quad(
                    device,
                    s.weights_surface,
                    s.weight_shader,
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.edges_texture),
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.area_texture),
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.search_texture),
                    width,
                    height,
                    static_cast<float>(width),
                    static_cast<float>(height),
                    subsample_indices,
                    1U << 2
                );
            }

            if(ok) {
                ok = draw_quad(
                    device,
                    output_target,
                    s.neighborhood_shader,
                    reinterpret_cast<IDirect3DBaseTexture9 *>(graphics.frame_texture),
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.weights_texture),
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.edges_texture),
                    width,
                    height,
                    1.0f,
                    graphics.settings.sharpening
                );
            }
            return ok;
        }

        inline bool render_t2x_resolve(
            IDirect3DDevice9 *device,
            IDirect3DSurface9 *scene_target,
            UINT width,
            UINT height
        ) noexcept {
            auto &s = state();
            if(!s.current_texture || !s.current_surface || !s.history_texture || !s.history_surface ||
               !s.resolve_shader || !s.copy_shader) {
                return false;
            }

            if(!render_spatial_passes(device, s.current_surface, width, height)) {
                s.history_valid = false;
                return false;
            }

            bool ok = true;
            if(s.history_valid) {
                // Official non-reprojection SMAAResolvePS: point-sampled 50/50 blend.
                ok = draw_quad(
                    device,
                    scene_target,
                    s.resolve_shader,
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.current_texture),
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.history_texture),
                    nullptr,
                    width,
                    height,
                    s.history_clamp ? 1.0f : 0.0f,
                    0.0f,
                    nullptr,
                    (1U << 0) | (1U << 1)
                );
            }
            else {
                // There is no previous temporal sample on the first frame after reset.
                ok = draw_quad(
                    device,
                    scene_target,
                    s.copy_shader,
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.current_texture),
                    nullptr,
                    nullptr,
                    width,
                    height,
                    0.0f,
                    0.0f,
                    nullptr,
                    1U << 0
                );
            }

            if(ok) {
                // History stores the previous *jittered SMAA output*, not the resolved
                // image. This matches the reference temporal resolve contract.
                ok = draw_quad(
                    device,
                    s.history_surface,
                    s.copy_shader,
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.current_texture),
                    nullptr,
                    nullptr,
                    width,
                    height,
                    0.0f,
                    0.0f,
                    nullptr,
                    1U << 0
                );
            }

            if(ok) {
                s.history_valid = true;
            }
            else {
                s.history_valid = false;
            }
            return ok;
        }

        inline bool render_frame(
            IDirect3DDevice9 *device,
            IDirect3DSurface9 *scene_target,
            UINT width,
            UINT height
        ) noexcept {
            if(temporal_requested()) {
                // Never feed T2x subsample indices or stale temporal history to a frame
                // for which the camera jitter could not be applied. Fall back to the
                // exact SMAA 1x spatial path for that frame and restart history cleanly.
                if(!state().jitter_applied) {
                    state().history_valid = false;
                    return render_spatial_passes(device, scene_target, width, height);
                }
                return render_t2x_resolve(device, scene_target, width, height);
            }
            state().jitter_applied = false;
            return render_spatial_passes(device, scene_target, width, height);
        }

        // Apply the exact two camera jitter positions specified by the reference SMAA
        // T2x table. Halo has already built the normal primary frustum when this is called
        // from the existing rasterizer_window_begin hook; we shift its frustum bounds by
        // the requested fraction of one viewport pixel and ask Halo to rebuild it.
        inline void apply_temporal_jitter() noexcept {
            auto &s = state();
            auto &graphics = EnhancedGraphics::state();
            if(!temporal_requested() || s.runtime_disabled || s.jitter_applied ||
               !graphics.settings.enabled || graphics.runtime_disabled ||
               !graphics.settings.smaa_exclude_hud ||
               !global_window_parameters ||
               global_window_parameters->render_target != RENDER_TARGET_RENDER_PRIMARY) {
                return;
            }

            auto &window = *global_window_parameters;
            const int viewport_width = static_cast<int>(window.camera.viewport_bounds.right) -
                                       static_cast<int>(window.camera.viewport_bounds.left);
            const int viewport_height = static_cast<int>(window.camera.viewport_bounds.bottom) -
                                        static_cast<int>(window.camera.viewport_bounds.top);
            if(viewport_width <= 0 || viewport_height <= 0) {
                return;
            }

            Bounds2D bounds = window.frustum.frustum_bounds;
            const float span_x = bounds.right - bounds.left;
            const float span_y = bounds.top - bounds.bottom;
            if(!std::isfinite(span_x) || !std::isfinite(span_y) ||
               std::fabs(span_x) < 1.0e-6f || std::fabs(span_y) < 1.0e-6f) {
                return;
            }

            const bool second_sample = (s.temporal_sample & 1U) != 0U;
            const float jitter_x = second_sample ? -0.25f : 0.25f;
            const float jitter_y = second_sample ? 0.25f : -0.25f;

            // Same projection-window shift used by the reference SMAA demo camera.
            const float dx = -(jitter_x * span_x / static_cast<float>(viewport_width));
            const float dy = -(jitter_y * span_y / static_cast<float>(viewport_height));
            bounds.left += dx;
            bounds.right += dx;
            bounds.top += dy;
            bounds.bottom += dy;

            render_camera_build_frustum(&window.camera, &bounds, &window.frustum, true);
            s.jitter_applied = true;
        }

        inline void on_reset(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *) noexcept {
            GraphicsRuntimeMetrics::subsystem_reset(GraphicsRuntimeMetrics::Subsystem::SMAA);
            release_resources();
            GraphicsRuntimeMetrics::write_log();
        }

        inline void on_pre_hud() noexcept {
            auto &s = state();
            auto &graphics = EnhancedGraphics::state();

            // The world has already been rendered with the selected T2x camera sample
            // when this callback runs. Advance the sample exactly once on every exit,
            // even if capture/copy/AA fails, so a transient D3D failure cannot leave the
            // camera permanently biased to one jitter. Failed frames invalidate history.
            struct TemporalFrameGuard {
                State &state;
                bool active;
                bool keep_history = false;

                ~TemporalFrameGuard() noexcept {
                    if(!active) {
                        return;
                    }
                    if(!keep_history) {
                        state.history_valid = false;
                    }
                    state.temporal_sample ^= 1U;
                    state.jitter_applied = false;
                }
            } temporal_frame_guard {s, temporal_requested() && s.jitter_applied};

            if(s.runtime_disabled) {
                EnhancedGraphics::on_pre_hud();
                return;
            }
            if(!requested() || s.processing ||
               !graphics.settings.enabled || graphics.runtime_disabled ||
               !global_d3d9_device || !*global_d3d9_device) {
                s.jitter_applied = false;
                return;
            }

            auto *device = *global_d3d9_device;
            s.processing = true;

            IDirect3DStateBlock9 *state_block = nullptr;
            IDirect3DSurface9 *scene_target = nullptr;
            D3DVIEWPORT9 old_viewport {};
            RECT old_scissor {};
            if(!EnhancedGraphics::capture_state(device, &state_block, &scene_target, old_viewport, old_scissor)) {
                s.processing = false;
                s.jitter_applied = false;
                return;
            }

            D3DSURFACE_DESC description {};
            if(FAILED(IDirect3DSurface9_GetDesc(scene_target, &description)) ||
               description.Width == 0 || description.Height == 0 || description.Format == D3DFMT_UNKNOWN) {
                EnhancedGraphics::restore_state(device, state_block, scene_target, old_viewport, old_scissor);
                s.processing = false;
                s.jitter_applied = false;
                return;
            }

            if(!EnhancedGraphics::ensure_resources(device, description)) {
                EnhancedGraphics::restore_state(device, state_block, scene_target, old_viewport, old_scissor);
                s.processing = false;
                s.jitter_applied = false;
                EnhancedGraphics::disable_for_session(
                    "Chimera Graphics disabled: shared pre-HUD post-process resources could not be created."
                );
                return;
            }

            if(!ensure_resources(device, description.Width, description.Height, description.Format)) {
                EnhancedGraphics::restore_state(device, state_block, scene_target, old_viewport, old_scissor);
                s.processing = false;
                s.jitter_applied = false;
                disable_for_session(
                    "Chimera Graphics SMAA disabled: official SMAA resources could not be created."
                );
                EnhancedGraphics::on_pre_hud();
                return;
            }

            if(FAILED(IDirect3DDevice9_EndScene(device))) {
                EnhancedGraphics::restore_state(device, state_block, scene_target, old_viewport, old_scissor);
                s.processing = false;
                s.jitter_applied = false;
                return;
            }

            const bool copied = SUCCEEDED(IDirect3DDevice9_StretchRect(
                device,
                scene_target,
                nullptr,
                graphics.frame_surface,
                nullptr,
                D3DTEXF_NONE
            ));

            if(FAILED(IDirect3DDevice9_BeginScene(device))) {
                release_com(scene_target);
                release_com(state_block);
                s.processing = false;
                s.jitter_applied = false;
                disable_for_session("Chimera Graphics SMAA disabled: D3D9 could not resume Halo's scene.");
                EnhancedGraphics::disable_for_session(
                    "Chimera Graphics disabled: D3D9 could not resume Halo's scene after SMAA processing."
                );
                return;
            }

            bool frame_succeeded = false;
            if(!copied) {
                s.history_valid = false;
                s.jitter_applied = false;
                report_failure_once("Chimera Graphics SMAA: active world target could not be copied.");
            }
            else {
                frame_succeeded = render_frame(device, scene_target, description.Width, description.Height);
                if(!frame_succeeded) {
                    report_failure_once("Chimera Graphics SMAA: one of the AA passes failed; frame was left unmodified.");
                }
            }
            temporal_frame_guard.keep_history = frame_succeeded;

            EnhancedGraphics::restore_state(device, state_block, scene_target, old_viewport, old_scissor);
            s.processing = false;
        }

        inline bool install_pre_hud_hook() noexcept {
            static Hook hook;
            if(hook.address && hook.hook && !hook.original_bytes.empty()) {
                return true;
            }
            auto *call_site = EnhancedGraphics::validated_pre_hud_call_site();
            if(!call_site) {
                return false;
            }
            write_jmp_call(call_site, hook, reinterpret_cast<const void *>(on_pre_hud));
            return hook.address == call_site && hook.hook && !hook.original_bytes.empty();
        }

        inline void set_up() noexcept {
            if(!requested()) {
                return;
            }
            auto &graphics = EnhancedGraphics::state();
            if(!graphics.settings.enabled || graphics.runtime_disabled) {
                return;
            }

            // Keep EnhancedGraphics' existing strict-pre-HUD dispatch behavior without
            // selecting its FXAA shader as the requested AA algorithm.
            graphics.settings.fxaa = true;

            if(!d3d9_device_caps || d3d9_device_caps->PixelShaderVersion < 0xffff0300) {
                console_error("Chimera Graphics: official SMAA requires ps_3_0; FXAA fallback remains available.");
                if(graphics.settings.smaa_exclude_hud && !EnhancedGraphics::install_pre_hud_hook()) {
                    EnhancedGraphics::disable_for_session(
                        "Chimera Graphics SMAA fallback disabled: strict pre-HUD hook could not be installed."
                    );
                }
                return;
            }

            if(!graphics.settings.smaa_exclude_hud) {
                console_error("Chimera Graphics: official SMAA requires smaa_exclude_hud=1; full-frame fallback is active.");
                return;
            }

            if(!install_pre_hud_hook()) {
                disable_for_session("Chimera Graphics SMAA disabled: strict pre-HUD hook could not be installed.");
                return;
            }

            add_d3d9_reset_event(on_reset, EVENT_PRIORITY_BEFORE);
            add_game_exit_event(release_resources, EVENT_PRIORITY_BEFORE);
        }
    }
}

#endif
