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
#include "../event/interface_render.hpp"

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

        enum class PassKind : std::uint8_t {
            NORMAL,
            EDGE,
            WEIGHTS
        };

        struct SmaaVertex {
            float x;
            float y;
            float z;
            float rhw;
            float u;
            float v;
            float data0[4];
            float data1[4];
            float data2[4];
            float data3[4];
        };

        static constexpr DWORD SMAA_VERTEX_FVF =
            D3DFVF_XYZRHW |
            D3DFVF_TEX5 |
            D3DFVF_TEXCOORDSIZE2(0) |
            D3DFVF_TEXCOORDSIZE4(1) |
            D3DFVF_TEXCOORDSIZE4(2) |
            D3DFVF_TEXCOORDSIZE4(3) |
            D3DFVF_TEXCOORDSIZE4(4);

        struct State {
            IDirect3DDevice9 *device = nullptr;

            IDirect3DTexture9 *edges_texture = nullptr;
            IDirect3DSurface9 *edges_surface = nullptr;
            IDirect3DTexture9 *weights_texture = nullptr;
            IDirect3DSurface9 *weights_surface = nullptr;
            IDirect3DTexture9 *area_texture = nullptr;
            IDirect3DTexture9 *search_texture = nullptr;
            IDirect3DSurface9 *stencil_surface = nullptr;

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
            IDirect3DVertexBuffer9 *vertex_buffer = nullptr;
            IDirect3DStateBlock9 *common_state_block = nullptr;
            IDirect3DStateBlock9 *restore_state_block = nullptr;
            IDirect3DSurface9 *cached_scene_surface = nullptr;
            IDirect3DTexture9 *cached_scene_texture = nullptr;

            UINT width = 0;
            UINT height = 0;
            D3DFORMAT color_format = D3DFMT_UNKNOWN;
            bool runtime_disabled = false;
            bool failure_reported = false;
            bool processing = false;
            bool history_valid = false;
            bool jitter_applied = false;
            bool temporal_mode = false;
            bool cached_scene_checked = false;
            std::uint32_t temporal_sample = 0;
            std::uint32_t tracked_resources = 0;
            DWORD active_point_sampler_mask = 0;
            DWORD stencil_ref = 1;
            DWORD stencil_ref_mask = 1;
            bool stencil_clear_required = true;
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
            release_com(s.cached_scene_texture);
            release_com(s.cached_scene_surface);
            s.cached_scene_checked = false;
            release_com(s.restore_state_block);
            release_com(s.common_state_block);
            release_com(s.vertex_buffer);
            release_com(s.copy_shader);
            release_com(s.resolve_shader);
            release_com(s.neighborhood_shader);
            release_com(s.weight_shader);
            release_com(s.edge_shader);
            release_com(s.history_surface);
            release_com(s.history_texture);
            release_com(s.current_surface);
            release_com(s.current_texture);
            release_com(s.stencil_surface);
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
            s.stencil_ref = 1;
            s.stencil_ref_mask = 1;
            s.stencil_clear_required = true;
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

        inline bool create_stencil_surface(
            IDirect3DDevice9 *device,
            UINT width,
            UINT height,
            IDirect3DSurface9 **surface,
            DWORD &stencil_ref_mask
        ) noexcept {
            if(!device || !surface || width == 0 || height == 0) {
                return false;
            }

            *surface = nullptr;
            stencil_ref_mask = 1;

            struct StencilFormat {
                D3DFORMAT format;
                DWORD mask;
            };
            constexpr StencilFormat formats[] = {
                {D3DFMT_D24S8, 0xFFU},
                {D3DFMT_D24X4S4, 0x0FU},
                {D3DFMT_D15S1, 0x01U}
            };

            for(const auto &candidate : formats) {
                if(SUCCEEDED(IDirect3DDevice9_CreateDepthStencilSurface(
                    device,
                    width,
                    height,
                    candidate.format,
                    D3DMULTISAMPLE_NONE,
                    0,
                    TRUE,
                    surface,
                    nullptr
                )) && *surface) {
                    stencil_ref_mask = candidate.mask;
                    return true;
                }
                release_com(*surface);
            }
            return false;
        }

        // Reference SMAA Ultra luma edge detection. As in the official implementation,
        // pixels without an edge are discarded so the stencil pass can mark only work
        // that must enter the expensive blending-weight shader.
        static constexpr const char *EDGE_SHADER = R"HLSL(
            sampler2D frame_sampler : register(s0);

            float luma(float3 c) {
                return dot(c, float3(0.2126, 0.7152, 0.0722));
            }

            float4 main(
                float2 uv : TEXCOORD0,
                float4 offset0 : TEXCOORD1,
                float4 offset1 : TEXCOORD2,
                float4 offset2 : TEXCOORD3
            ) : COLOR0 {
                const float L = luma(tex2D(frame_sampler, uv).rgb);
                const float Lleft = luma(tex2D(frame_sampler, offset0.xy).rgb);
                const float Ltop = luma(tex2D(frame_sampler, offset0.zw).rgb);

                float4 delta = 0.0;
                delta.xy = abs(L.xx - float2(Lleft, Ltop));
                float2 edges = step(0.05.xx, delta.xy);
                if(dot(edges, 1.0.xx) == 0.0) {
                    discard;
                }

                const float Lright = luma(tex2D(frame_sampler, offset1.xy).rgb);
                const float Lbottom = luma(tex2D(frame_sampler, offset1.zw).rgb);
                delta.zw = abs(L.xx - float2(Lright, Lbottom));
                float2 max_delta = max(delta.xy, delta.zw);

                const float Lleftleft = luma(tex2D(frame_sampler, offset2.xy).rgb);
                const float Ltoptop = luma(tex2D(frame_sampler, offset2.zw).rgb);
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

                [branch]
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

                [branch]
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

            float4 main(
                float2 uv : TEXCOORD0,
                float4 pixcoord_input : TEXCOORD1,
                float4 offset0 : TEXCOORD2,
                float4 offset1 : TEXCOORD3,
                float4 offset2 : TEXCOORD4
            ) : COLOR0 {
                float4 weights = 0.0;
                float2 e = edge4(uv).rg;
                const float2 pixcoord = pixcoord_input.xy;

                [branch]
                if(e.g > 0.0) {
                    weights.rg = calculate_diag_weights(uv, e);
                    [branch]
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

                [branch]
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

            float4 main(float2 uv : TEXCOORD0, float4 offset : TEXCOORD1) : COLOR0 {
                const float2 t = frame_options.xy;
                const float4 center_sample = tex2D(frame_sampler, uv);

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

        // Official non-reprojection SMAA T2x resolve: point-sampled 50/50 blend.
        static constexpr const char *RESOLVE_SHADER = R"HLSL(
            sampler2D current_sampler : register(s0);
            sampler2D previous_sampler : register(s1);
            sampler2D edges_sampler : register(s2);
            float4 metrics : register(c0); // inv width, inv height, unused, unused

            float4 sample_lod0(sampler2D texture_sampler, float2 uv) {
                return tex2Dlod(texture_sampler, float4(uv, 0.0, 0.0));
            }

            float4 main(float2 uv : TEXCOORD0) : COLOR0 {
                const float4 current = sample_lod0(current_sampler, uv);
                float4 previous = sample_lod0(previous_sampler, uv);
                const float2 edges = sample_lod0(edges_sampler, uv).rg;
                const float3 temporal_delta = abs(current.rgb - previous.rgb);
                const float max_temporal_delta =
                    max(temporal_delta.r, max(temporal_delta.g, temporal_delta.b));

                // The old T2x path stabilized history with a 5-tap neighborhood clamp.
                // Run the extra taps only around current SMAA edges or where current and
                // previous disagree enough to indicate a shifted subpixel edge/detail.
                [branch]
                if(max(edges.r, edges.g) > 0.0 || max_temporal_delta > 0.02) {
                    const float2 t = metrics.xy;
                    const float3 left =
                        sample_lod0(current_sampler, uv + float2(-t.x, 0.0)).rgb;
                    const float3 right =
                        sample_lod0(current_sampler, uv + float2(t.x, 0.0)).rgb;
                    const float3 up =
                        sample_lod0(current_sampler, uv + float2(0.0, -t.y)).rgb;
                    const float3 down =
                        sample_lod0(current_sampler, uv + float2(0.0, t.y)).rgb;

                    const float3 neighborhood_min =
                        min(current.rgb, min(min(left, right), min(up, down)));
                    const float3 neighborhood_max =
                        max(current.rgb, max(max(left, right), max(up, down)));
                    const float3 local_span = neighborhood_max - neighborhood_min;
                    const float3 margin =
                        float3(0.012, 0.012, 0.012) + local_span * 0.08;
                    previous.rgb = clamp(
                        previous.rgb,
                        neighborhood_min - margin,
                        neighborhood_max + margin
                    );
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

        inline bool create_fullscreen_vertex_buffer(
            IDirect3DDevice9 *device,
            UINT width,
            UINT height,
            IDirect3DVertexBuffer9 **buffer
        ) noexcept {
            if(!device || !buffer || width == 0 || height == 0) {
                return false;
            }

            constexpr UINT vertices_per_pass = 3;
            constexpr UINT vertex_count = vertices_per_pass * 3;
            const UINT buffer_size =
                static_cast<UINT>(sizeof(SmaaVertex) * vertex_count);

            *buffer = nullptr;
            if(FAILED(IDirect3DDevice9_CreateVertexBuffer(
                device,
                buffer_size,
                D3DUSAGE_WRITEONLY,
                SMAA_VERTEX_FVF,
                D3DPOOL_DEFAULT,
                buffer,
                nullptr
            )) || !*buffer) {
                return false;
            }

            SmaaVertex *vertices = nullptr;
            if(FAILED(IDirect3DVertexBuffer9_Lock(
                *buffer,
                0,
                buffer_size,
                reinterpret_cast<void **>(&vertices),
                0
            )) || !vertices) {
                release_com(*buffer);
                return false;
            }

            const float inv_width = 1.0f / static_cast<float>(width);
            const float inv_height = 1.0f / static_cast<float>(height);

            // Official SMAA recommendation: one oversized fullscreen triangle.
            // Within the viewport, position and UV interpolation match a 0..1 quad.
            const float far_right = 2.0f * static_cast<float>(width) - 0.5f;
            const float far_bottom = 2.0f * static_cast<float>(height) - 0.5f;
            const float screen_x[3] = {-0.5f, far_right, -0.5f};
            const float screen_y[3] = {-0.5f, -0.5f, far_bottom};
            const float tex_u[3] = {0.0f, 2.0f, 0.0f};
            const float tex_v[3] = {0.0f, 0.0f, 2.0f};

            auto fill_pass = [&](PassKind pass_kind, SmaaVertex *pass_vertices) noexcept {
                for(std::size_t i = 0; i < 3; i++) {
                    auto &vertex = pass_vertices[i];
                    const float u = tex_u[i];
                    const float v = tex_v[i];

                    vertex = {};
                    vertex.x = screen_x[i];
                    vertex.y = screen_y[i];
                    vertex.z = 0.0f;
                    vertex.rhw = 1.0f;
                    vertex.u = u;
                    vertex.v = v;

                    if(pass_kind == PassKind::EDGE) {
                        vertex.data0[0] = u - inv_width;
                        vertex.data0[1] = v;
                        vertex.data0[2] = u;
                        vertex.data0[3] = v - inv_height;
                        vertex.data1[0] = u + inv_width;
                        vertex.data1[1] = v;
                        vertex.data1[2] = u;
                        vertex.data1[3] = v + inv_height;
                        vertex.data2[0] = u - 2.0f * inv_width;
                        vertex.data2[1] = v;
                        vertex.data2[2] = u;
                        vertex.data2[3] = v - 2.0f * inv_height;
                    }
                    else if(pass_kind == PassKind::WEIGHTS) {
                        vertex.data0[0] = u * static_cast<float>(width);
                        vertex.data0[1] = v * static_cast<float>(height);
                        vertex.data1[0] = u - 0.25f * inv_width;
                        vertex.data1[1] = v - 0.125f * inv_height;
                        vertex.data1[2] = u + 1.25f * inv_width;
                        vertex.data1[3] = v - 0.125f * inv_height;
                        vertex.data2[0] = u - 0.125f * inv_width;
                        vertex.data2[1] = v - 0.25f * inv_height;
                        vertex.data2[2] = u - 0.125f * inv_width;
                        vertex.data2[3] = v + 1.25f * inv_height;
                        vertex.data3[0] = vertex.data1[0] - 64.0f * inv_width;
                        vertex.data3[1] = vertex.data1[2] + 64.0f * inv_width;
                        vertex.data3[2] = vertex.data2[1] - 64.0f * inv_height;
                        vertex.data3[3] = vertex.data2[3] + 64.0f * inv_height;
                    }
                    else {
                        vertex.data0[0] = u + inv_width;
                        vertex.data0[1] = v;
                        vertex.data0[2] = u;
                        vertex.data0[3] = v + inv_height;
                    }
                }
            };

            fill_pass(PassKind::NORMAL, vertices);
            fill_pass(PassKind::EDGE, vertices + vertices_per_pass);
            fill_pass(PassKind::WEIGHTS, vertices + 2 * vertices_per_pass);

            IDirect3DVertexBuffer9_Unlock(*buffer);
            return true;
        }

        inline void record_common_state(
            IDirect3DDevice9 *device,
            const State &s
        ) noexcept {
            IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_SRGBWRITEENABLE, FALSE);
            IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILENABLE, FALSE);

            IDirect3DDevice9_SetVertexShader(device, nullptr);
            IDirect3DDevice9_SetVertexDeclaration(device, nullptr);
            IDirect3DDevice9_SetFVF(device, SMAA_VERTEX_FVF);
            IDirect3DDevice9_SetStreamSource(
                device,
                0,
                s.vertex_buffer,
                0,
                sizeof(SmaaVertex)
            );

            for(DWORD sampler = 0; sampler < 3; sampler++) {
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_SRGBTEXTURE, FALSE);
            }
        }

        inline bool create_smaa_state_blocks(
            IDirect3DDevice9 *device
        ) noexcept {
            auto &s = state();
            release_com(s.common_state_block);
            release_com(s.restore_state_block);

            if(FAILED(IDirect3DDevice9_BeginStateBlock(device))) {
                return false;
            }
            record_common_state(device, s);
            if(FAILED(IDirect3DDevice9_EndStateBlock(device, &s.common_state_block)) ||
               !s.common_state_block) {
                release_com(s.common_state_block);
                return false;
            }

            if(FAILED(IDirect3DDevice9_BeginStateBlock(device))) {
                release_com(s.common_state_block);
                return false;
            }

            // Record exactly the state family SMAA can modify. Capture() on this block
            // updates only these values instead of walking the whole D3D9 state machine.
            record_common_state(device, s);
            IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
            IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILREF, 1);
            IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILMASK, 0xFFFFFFFFU);
            IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILWRITEMASK, 0xFFFFFFFFU);
            IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
            IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
            IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);

            IDirect3DDevice9_SetPixelShader(device, s.edge_shader);
            IDirect3DDevice9_SetTexture(
                device,
                0,
                reinterpret_cast<IDirect3DBaseTexture9 *>(s.edges_texture)
            );
            IDirect3DDevice9_SetTexture(
                device,
                1,
                reinterpret_cast<IDirect3DBaseTexture9 *>(s.area_texture)
            );
            IDirect3DDevice9_SetTexture(
                device,
                2,
                reinterpret_cast<IDirect3DBaseTexture9 *>(s.search_texture)
            );

            const float zero_constants[8] {};
            IDirect3DDevice9_SetPixelShaderConstantF(device, 0, zero_constants, 2);

            if(FAILED(IDirect3DDevice9_EndStateBlock(device, &s.restore_state_block)) ||
               !s.restore_state_block) {
                release_com(s.restore_state_block);
                release_com(s.common_state_block);
                return false;
            }
            return true;
        }

        struct CapturedState {
            IDirect3DStateBlock9 *fallback_state_block = nullptr;
            IDirect3DSurface9 *render_target = nullptr;
            D3DVIEWPORT9 viewport {};
            RECT scissor {};
            bool lean = false;
        };

        inline bool capture_smaa_state(
            IDirect3DDevice9 *device,
            IDirect3DSurface9 *render_target,
            CapturedState &captured
        ) noexcept {
            if(!device || !render_target) {
                return false;
            }

            captured.render_target = render_target;
            auto &s = state();
            if(s.restore_state_block &&
               SUCCEEDED(IDirect3DStateBlock9_Capture(s.restore_state_block))) {
                captured.lean = true;
                if(FAILED(IDirect3DDevice9_GetViewport(device, &captured.viewport))) {
                    captured.render_target = nullptr;
                    captured.lean = false;
                    return false;
                }
                if(FAILED(IDirect3DDevice9_GetScissorRect(device, &captured.scissor))) {
                    captured.scissor.left = 0;
                    captured.scissor.top = 0;
                    captured.scissor.right = static_cast<LONG>(s.width);
                    captured.scissor.bottom = static_cast<LONG>(s.height);
                }
                return true;
            }

            // Compatibility fallback: reuse EnhancedGraphics' cached D3DSBT_ALL
            // block, but keep ownership of the render-target reference we already have.
            auto &graphics = EnhancedGraphics::state();
            if(!graphics.capture_state_block || graphics.capture_state_block_device != device) {
                EnhancedGraphics::release_com(graphics.capture_state_block);
                graphics.capture_state_block_device = nullptr;
                if(FAILED(IDirect3DDevice9_CreateStateBlock(
                    device,
                    D3DSBT_ALL,
                    &graphics.capture_state_block
                )) || !graphics.capture_state_block) {
                    captured.render_target = nullptr;
                    return false;
                }
                graphics.capture_state_block_device = device;
            }

            if(FAILED(IDirect3DStateBlock9_Capture(graphics.capture_state_block))) {
                captured.render_target = nullptr;
                return false;
            }

            captured.fallback_state_block = graphics.capture_state_block;
            captured.fallback_state_block->AddRef();
            captured.lean = false;

            if(FAILED(IDirect3DDevice9_GetViewport(device, &captured.viewport))) {
                release_com(captured.fallback_state_block);
                captured.render_target = nullptr;
                return false;
            }
            if(FAILED(IDirect3DDevice9_GetScissorRect(device, &captured.scissor))) {
                captured.scissor.left = 0;
                captured.scissor.top = 0;
                captured.scissor.right = static_cast<LONG>(s.width);
                captured.scissor.bottom = static_cast<LONG>(s.height);
            }
            return true;
        }

        inline void restore_smaa_state(
            IDirect3DDevice9 *device,
            CapturedState &captured
        ) noexcept {
            if(captured.lean) {
                auto &s = state();
                if(s.restore_state_block) {
                    IDirect3DStateBlock9_Apply(s.restore_state_block);
                }
                if(captured.render_target) {
                    IDirect3DDevice9_SetRenderTarget(device, 0, captured.render_target);
                }
                IDirect3DDevice9_SetViewport(device, &captured.viewport);
                IDirect3DDevice9_SetScissorRect(device, &captured.scissor);
                release_com(captured.render_target);
                captured.lean = false;
                return;
            }

            EnhancedGraphics::restore_state(
                device,
                captured.fallback_state_block,
                captured.render_target,
                captured.viewport,
                captured.scissor
            );
            captured.fallback_state_block = nullptr;
            captured.render_target = nullptr;
        }

        inline void discard_smaa_state(CapturedState &captured) noexcept {
            release_com(captured.render_target);
            release_com(captured.fallback_state_block);
            captured.lean = false;
        }

        inline bool ensure_resources(
            IDirect3DDevice9 *device,
            UINT width,
            UINT height,
            D3DFORMAT color_format,
            bool temporal
        ) noexcept {
            auto &s = state();
            const bool common_matches =
                s.device == device && s.width == width && s.height == height &&
                s.color_format == color_format &&
                s.edges_texture && s.edges_surface && s.weights_texture && s.weights_surface &&
                s.area_texture && s.search_texture && s.vertex_buffer &&
                s.common_state_block && s.restore_state_block &&
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

            if(!create_shader(device, EDGE_SHADER, &s.edge_shader) ||
               !create_shader(device, WEIGHT_SHADER, &s.weight_shader) ||
               !create_shader(device, NEIGHBORHOOD_SHADER, &s.neighborhood_shader) ||
               !create_render_target(device, width, height, &s.edges_texture, &s.edges_surface) ||
               !create_render_target(device, width, height, &s.weights_texture, &s.weights_surface) ||
               !create_area_texture(device, &s.area_texture) ||
               !create_search_texture(device, &s.search_texture) ||
               !create_fullscreen_vertex_buffer(device, width, height, &s.vertex_buffer)) {
                release_resources();
                return false;
            }

            // Stencil is an optimization only. If a particular D3D9 implementation
            // cannot provide a compatible stencil format, SMAA keeps its functional
            // full-screen fallback instead of disabling anti-aliasing.
            create_stencil_surface(
                device,
                width,
                height,
                &s.stencil_surface,
                s.stencil_ref_mask
            );
            s.stencil_ref = 1;
            s.stencil_clear_required = true;

            if(temporal) {
                if(!create_shader(device, RESOLVE_SHADER, &s.resolve_shader) ||
                   !create_shader(device, COPY_SHADER, &s.copy_shader) ||
                   !create_color_render_target(device, width, height, color_format, &s.current_texture, &s.current_surface) ||
                   !create_color_render_target(device, width, height, color_format, &s.history_texture, &s.history_surface)) {
                    release_resources();
                    return false;
                }
            }
            if(!create_smaa_state_blocks(device)) {
                release_resources();
                return false;
            }
            s.tracked_resources = (temporal ? 18U : 12U) + (s.stencil_surface ? 1U : 0U);
            GraphicsRuntimeMetrics::resources_created(s.tracked_resources);
            GraphicsRuntimeMetrics::subsystem_recovered(GraphicsRuntimeMetrics::Subsystem::SMAA);
            return true;
        }

        inline void begin_pass_sequence(IDirect3DDevice9 *device) noexcept {
            auto &s = state();

            if(s.common_state_block &&
               SUCCEEDED(IDirect3DStateBlock9_Apply(s.common_state_block))) {
                s.active_point_sampler_mask = 0;
                return;
            }

            // Compatibility fallback for wrappers/drivers that reject a custom block.
            record_common_state(device, s);
            s.active_point_sampler_mask = 0;
        }

        inline void set_point_sampler_mask(IDirect3DDevice9 *device, DWORD point_sampler_mask) noexcept {
            auto &s = state();
            const DWORD changed = s.active_point_sampler_mask ^ point_sampler_mask;
            if(changed == 0) {
                return;
            }

            for(DWORD sampler = 0; sampler < 3; sampler++) {
                const DWORD bit = 1U << sampler;
                if((changed & bit) == 0) {
                    continue;
                }
                const DWORD filter = (point_sampler_mask & bit) != 0 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_MINFILTER, filter);
                IDirect3DDevice9_SetSamplerState(device, sampler, D3DSAMP_MAGFILTER, filter);
            }
            s.active_point_sampler_mask = point_sampler_mask;
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
            DWORD point_sampler_mask = 0,
            PassKind pass_kind = PassKind::NORMAL,
            bool use_stencil = false
        ) noexcept {
            if(!device || !target || !shader || !texture0 || width == 0 || height == 0) {
                return false;
            }

            if(FAILED(IDirect3DDevice9_SetRenderTarget(device, 0, target))) {
                return false;
            }

            if(pass_kind == PassKind::EDGE || pass_kind == PassKind::WEIGHTS) {
                DWORD clear_flags = D3DCLEAR_TARGET;
                if(pass_kind == PassKind::EDGE && use_stencil && state().stencil_clear_required) {
                    clear_flags |= D3DCLEAR_STENCIL;
                }
                if(FAILED(IDirect3DDevice9_Clear(
                    device,
                    0,
                    nullptr,
                    clear_flags,
                    0,
                    1.0f,
                    0
                ))) {
                    return false;
                }
            }

            if(use_stencil && pass_kind == PassKind::EDGE) {
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILENABLE, TRUE);
                const auto &s = state();
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILREF, s.stencil_ref);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILMASK, s.stencil_ref_mask);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILWRITEMASK, s.stencil_ref_mask);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
            }
            else if(use_stencil && pass_kind == PassKind::WEIGHTS) {
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILENABLE, TRUE);
                const auto &s = state();
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILREF, s.stencil_ref);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILMASK, s.stencil_ref_mask);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILWRITEMASK, 0U);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
            }
            else {
                IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILENABLE, FALSE);
            }

            set_point_sampler_mask(device, point_sampler_mask);

            IDirect3DDevice9_SetPixelShader(device, shader);
            IDirect3DDevice9_SetTexture(device, 0, texture0);
            if(texture1) {
                IDirect3DDevice9_SetTexture(device, 1, texture1);
            }
            if(texture2) {
                IDirect3DDevice9_SetTexture(device, 2, texture2);
            }

            // Only the blending-weight and neighborhood shaders consume c0.
            // Edge offsets are precomputed in the vertex buffer; resolve/copy have no
            // shader constants. Avoid redundant constant uploads on those passes.
            if(pass_kind == PassKind::WEIGHTS ||
               shader == state().neighborhood_shader ||
               shader == state().resolve_shader) {
                const float constants[4] = {
                    1.0f / static_cast<float>(width),
                    1.0f / static_cast<float>(height),
                    option_z,
                    option_w
                };
                if(FAILED(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, constants, 1))) {
                    return false;
                }
            }
            if(subsample_indices && FAILED(IDirect3DDevice9_SetPixelShaderConstantF(device, 1, subsample_indices, 1))) {
                return false;
            }

            UINT start_vertex = 0;
            if(pass_kind == PassKind::EDGE) {
                start_vertex = 3;
            }
            else if(pass_kind == PassKind::WEIGHTS) {
                start_vertex = 6;
            }

            return SUCCEEDED(IDirect3DDevice9_DrawPrimitive(
                device,
                D3DPT_TRIANGLELIST,
                start_vertex,
                1
            ));
        }

        inline void subsample_indices_for_current_frame(float indices[4], bool temporal) noexcept {
            if(!temporal || !state().jitter_applied) {
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
            IDirect3DBaseTexture9 *source_texture,
            IDirect3DSurface9 *output_target,
            UINT width,
            UINT height,
            bool temporal
        ) noexcept {
            auto &s = state();
            auto &graphics = EnhancedGraphics::state();
            if(!source_texture) {
                return false;
            }

            float subsample_indices[4] {};
            subsample_indices_for_current_frame(subsample_indices, temporal);

            IDirect3DSurface9 *original_depth_stencil = nullptr;
            bool use_stencil = false;
            if(s.stencil_surface) {
                const HRESULT depth_result =
                    IDirect3DDevice9_GetDepthStencilSurface(device, &original_depth_stencil);
                const bool can_restore_depth =
                    SUCCEEDED(depth_result) || depth_result == D3DERR_NOTFOUND;
                if(can_restore_depth &&
                   SUCCEEDED(IDirect3DDevice9_SetDepthStencilSurface(device, s.stencil_surface))) {
                    use_stencil = true;
                }
            }

            bool ok = draw_quad(
                device,
                s.edges_surface,
                s.edge_shader,
                source_texture,
                nullptr,
                nullptr,
                width,
                height,
                static_cast<float>(width),
                static_cast<float>(height),
                nullptr,
                0,
                PassKind::EDGE,
                use_stencil
            );

            if(ok) {
                // The official DX9 SMAA path uses the stencil generated by edge
                // detection so this expensive search shader runs only on edge pixels.
                // SearchTex itself remains point-filtered as required by the reference.
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
                    1U << 2,
                    PassKind::WEIGHTS,
                    use_stencil
                );
            }

            if(use_stencil) {
                IDirect3DDevice9_SetDepthStencilSurface(device, original_depth_stencil);

                if(s.stencil_ref_mask <= 1U) {
                    // A one-bit stencil has no spare generation values.
                    s.stencil_ref = 1U;
                    s.stencil_clear_required = true;
                }
                else if(s.stencil_ref >= s.stencil_ref_mask) {
                    // Old generations can match again after wrapping, so clear once.
                    s.stencil_ref = 1U;
                    s.stencil_clear_required = true;
                }
                else {
                    s.stencil_ref++;
                    s.stencil_clear_required = false;
                }
            }
            release_com(original_depth_stencil);

            if(ok) {
                ok = draw_quad(
                    device,
                    output_target,
                    s.neighborhood_shader,
                    source_texture,
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
            IDirect3DBaseTexture9 *source_texture,
            IDirect3DSurface9 *scene_target,
            UINT width,
            UINT height
        ) noexcept {
            auto &s = state();
            if(!source_texture ||
               !s.current_texture || !s.current_surface || !s.history_texture || !s.history_surface ||
               !s.resolve_shader || !s.copy_shader) {
                return false;
            }

            if(!render_spatial_passes(device, source_texture, s.current_surface, width, height, true)) {
                s.history_valid = false;
                return false;
            }

            // The direct T2x fast path can sample the same texture whose surface becomes
            // RT0 for the resolve. D3D9 requires it to be unbound from the sampler first.
            IDirect3DDevice9_SetTexture(device, 0, nullptr);

            bool ok = true;
            if(s.history_valid) {
                // Official non-reprojection SMAAResolvePS: point-sampled 50/50 blend.
                ok = draw_quad(
                    device,
                    scene_target,
                    s.resolve_shader,
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.current_texture),
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.history_texture),
                    reinterpret_cast<IDirect3DBaseTexture9 *>(s.edges_texture),
                    width,
                    height,
                    0.0f,
                    0.0f,
                    nullptr,
                    (1U << 0) | (1U << 1) | (1U << 2)
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
                // Ping-pong the two spatial targets. The freshly rendered current image
                // becomes next frame's history, removing the former full-screen history
                // copy pass while keeping the exact same T2x samples and 50/50 resolve.
                auto *texture_swap = s.current_texture;
                s.current_texture = s.history_texture;
                s.history_texture = texture_swap;

                auto *surface_swap = s.current_surface;
                s.current_surface = s.history_surface;
                s.history_surface = surface_swap;

                s.history_valid = true;
            }
            else {
                s.history_valid = false;
            }
            return ok;
        }

        inline bool render_frame(
            IDirect3DDevice9 *device,
            IDirect3DBaseTexture9 *source_texture,
            IDirect3DSurface9 *scene_target,
            UINT width,
            UINT height,
            bool temporal
        ) noexcept {
            auto &s = state();
            begin_pass_sequence(device);

            if(temporal) {
                // Never feed T2x subsample indices or stale temporal history to a frame
                // for which the camera jitter could not be applied. Fall back to the
                // exact SMAA 1x spatial path for that frame and restart history cleanly.
                if(!s.jitter_applied) {
                    s.history_valid = false;
                    return render_spatial_passes(
                        device,
                        source_texture,
                        scene_target,
                        width,
                        height,
                        true
                    );
                }
                return render_t2x_resolve(
                    device,
                    source_texture,
                    scene_target,
                    width,
                    height
                );
            }
            s.jitter_applied = false;
            return render_spatial_passes(
                device,
                source_texture,
                scene_target,
                width,
                height,
                false
            );
        }

        // Apply the exact two camera jitter positions specified by the reference SMAA
        // T2x table. Halo has already built the normal primary frustum when this is called
        // from the existing rasterizer_window_begin hook; we shift its frustum bounds by
        // the requested fraction of one viewport pixel and ask Halo to rebuild it.
        inline void apply_temporal_jitter() noexcept {
            auto &s = state();
            auto &graphics = EnhancedGraphics::state();
            if(!s.temporal_mode || s.runtime_disabled || s.jitter_applied ||
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

        inline IDirect3DTexture9 *surface_texture(IDirect3DSurface9 *surface) noexcept {
            if(!surface) {
                return nullptr;
            }

            // Keep the IID local so MinGW does not require the external dxguid symbol
            // IID_IDirect3DTexture9 at link time.
            static const IID texture_iid = {
                0x85c31227,
                0x3de5,
                0x4f00,
                {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}
            };

            IDirect3DTexture9 *texture = nullptr;
            if(FAILED(IDirect3DSurface9_GetContainer(
                surface,
                texture_iid,
                reinterpret_cast<void **>(&texture)
            ))) {
                return nullptr;
            }
            return texture;
        }

        inline IDirect3DTexture9 *cached_surface_texture(IDirect3DSurface9 *surface) noexcept {
            if(!surface) {
                return nullptr;
            }

            auto &s = state();
            if(s.cached_scene_checked && s.cached_scene_surface == surface) {
                return s.cached_scene_texture;
            }

            release_com(s.cached_scene_texture);
            release_com(s.cached_scene_surface);
            s.cached_scene_checked = false;

            surface->AddRef();
            s.cached_scene_surface = surface;
            s.cached_scene_texture = surface_texture(surface);
            s.cached_scene_checked = true;
            return s.cached_scene_texture;
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
            };

            const bool temporal = s.temporal_mode;
            TemporalFrameGuard temporal_frame_guard {s, temporal && s.jitter_applied};

            if(s.runtime_disabled) {
                EnhancedGraphics::on_pre_hud();
                return;
            }
            if(s.processing ||
               !graphics.settings.enabled || graphics.runtime_disabled ||
               !global_d3d9_device || !*global_d3d9_device) {
                s.jitter_applied = false;
                return;
            }

            auto *device = *global_d3d9_device;
            s.processing = true;

            IDirect3DSurface9 *scene_target = nullptr;
            if(FAILED(IDirect3DDevice9_GetRenderTarget(device, 0, &scene_target)) || !scene_target) {
                s.processing = false;
                s.jitter_applied = false;
                return;
            }

            D3DSURFACE_DESC description {};
            if(s.cached_scene_checked &&
               s.cached_scene_surface == scene_target &&
               s.width != 0 && s.height != 0 && s.color_format != D3DFMT_UNKNOWN) {
                description.Width = s.width;
                description.Height = s.height;
                description.Format = s.color_format;
            }
            else if(FAILED(IDirect3DSurface9_GetDesc(scene_target, &description)) ||
                    description.Width == 0 || description.Height == 0 ||
                    description.Format == D3DFMT_UNKNOWN) {
                release_com(scene_target);
                s.processing = false;
                s.jitter_applied = false;
                return;
            }

            if(!ensure_resources(device, description.Width, description.Height, description.Format, temporal)) {
                release_com(scene_target);
                s.processing = false;
                s.jitter_applied = false;
                disable_for_session(
                    "Chimera Graphics SMAA disabled: official SMAA resources could not be created."
                );
                EnhancedGraphics::on_pre_hud();
                return;
            }

            CapturedState captured_state {};
            if(!capture_smaa_state(device, scene_target, captured_state)) {
                release_com(scene_target);
                s.processing = false;
                s.jitter_applied = false;
                return;
            }

            // HAC2-style resource reuse: resolve the texture container once for a given
            // Halo render surface and keep the COM objects alive until reset/recreation.
            // This removes GetContainer/AddRef/Release churn from the per-frame T2x path.
            IDirect3DTexture9 *direct_scene_texture = nullptr;
            if(temporal && s.jitter_applied) {
                direct_scene_texture = cached_surface_texture(scene_target);
            }
            const bool direct_t2x = direct_scene_texture != nullptr;

            if(!direct_t2x && !EnhancedGraphics::ensure_resources(device, description)) {
                restore_smaa_state(device, captured_state);
                s.processing = false;
                s.jitter_applied = false;
                EnhancedGraphics::disable_for_session(
                    "Chimera Graphics disabled: shared pre-HUD post-process resources could not be created."
                );
                return;
            }

            bool copied = true;
            IDirect3DBaseTexture9 *source_texture =
                reinterpret_cast<IDirect3DBaseTexture9 *>(direct_scene_texture);

            if(!direct_t2x) {
                if(FAILED(IDirect3DDevice9_EndScene(device))) {
                    restore_smaa_state(device, captured_state);
                    s.processing = false;
                    s.jitter_applied = false;
                    return;
                }

                copied = SUCCEEDED(IDirect3DDevice9_StretchRect(
                    device,
                    scene_target,
                    nullptr,
                    graphics.frame_surface,
                    nullptr,
                    D3DTEXF_NONE
                ));

                if(FAILED(IDirect3DDevice9_BeginScene(device))) {
                    discard_smaa_state(captured_state);
                    s.processing = false;
                    s.jitter_applied = false;
                    disable_for_session("Chimera Graphics SMAA disabled: D3D9 could not resume Halo's scene.");
                    EnhancedGraphics::disable_for_session(
                        "Chimera Graphics disabled: D3D9 could not resume Halo's scene after SMAA processing."
                    );
                    return;
                }

                source_texture =
                    reinterpret_cast<IDirect3DBaseTexture9 *>(graphics.frame_texture);
            }

            bool frame_succeeded = false;
            if(!copied) {
                s.history_valid = false;
                s.jitter_applied = false;
                report_failure_once("Chimera Graphics SMAA: active world target could not be copied.");
            }
            else {
                frame_succeeded = render_frame(
                    device,
                    source_texture,
                    scene_target,
                    description.Width,
                    description.Height,
                    temporal
                );
                if(!frame_succeeded) {
                    report_failure_once("Chimera Graphics SMAA: one of the AA passes failed; frame was left unmodified.");
                }
            }
            temporal_frame_guard.keep_history = frame_succeeded;

            restore_smaa_state(device, captured_state);
            s.processing = false;
        }

        inline void on_hud_render_event(bool after) noexcept {
            if(!after) {
                on_pre_hud();
            }
        }

        inline bool install_pre_hud_hook() noexcept {
            auto *call_site = EnhancedGraphics::validated_pre_hud_call_site();
            if(!call_site || hud_render_event_call_site() != call_site) {
                return false;
            }

            return add_hud_render_event(on_hud_render_event, EVENT_PRIORITY_BEFORE);
        }

        inline void set_up() noexcept {
            if(!requested()) {
                return;
            }
            auto &s = state();
            s.temporal_mode = temporal_requested();

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
