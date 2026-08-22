// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_TRANSPARENT_SHADER_COMPAT_HPP
#define CHIMERA_D3D9_TRANSPARENT_SHADER_COMPAT_HPP

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "rasterizer_vertex_shaders.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace D3D9TransparentShaderCompat {
        enum GenericMode {
            GENERIC_MODE_STOCK = 0,
            GENERIC_MODE_EXACT_VS2,
            GENERIC_MODE_LEGACY_VS3
        };

        static IDirect3DVertexShader9 *stock_generic_m = nullptr;
        static IDirect3DVertexShader9 *stock_plasma_m = nullptr;
        static IDirect3DVertexShader9 *exact_generic_m = nullptr;
        static IDirect3DVertexShader9 *exact_plasma_m = nullptr;
        static IDirect3DVertexShader9 *legacy_generic_m = nullptr;
        static IDirect3DDevice9 *compiled_device = nullptr;
        static GenericMode generic_mode = GENERIC_MODE_STOCK;
        static bool plasma_m_active = false;

        // Reconstructed directly from the stock VSH_TRANSPARENT_GENERIC_M
        // disassembly dumped by Halo CE. The goal is not to redesign the effect:
        // it preserves the stock output contract while moving the legacy VS1.1
        // dynamic-addressing path to an explicit HLSL VS2 shader for D3D9On12.
        static constexpr const char *generic_m_hlsl = R"HLSL(
float4x4 c_world_view_projection : register(c0);
float4 c_eye_forward : register(c5);
float4 c_fog_gradient0 : register(c6);
float4 c_fog_gradient1 : register(c7);
float4 c_fog_gradient2 : register(c8);
float4 c_fog_densities : register(c9);
float4 c_generic_alpha : register(c12);
float4 c_tex0_x : register(c13);
float4 c_tex0_y : register(c14);
float4 c_tex1_x : register(c15);
float4 c_tex1_y : register(c16);
float4 c_tex2_x : register(c17);
float4 c_tex2_y : register(c18);
float4 c_tex3_x : register(c19);
float4 c_tex3_y : register(c20);
float4x3 c_node_matrices[22] : register(c29);

struct GENERIC_INPUT {
    float4 Position : POSITION0;
    float3 Normal : NORMAL0;
    float4 TexCoord : TEXCOORD0;
    float2 BlendIndices : BLENDINDICES0;
    float2 BlendWeights : BLENDWEIGHT0;
};

struct GENERIC_OUTPUT {
    float4 Position : POSITION0;
    float4 D0 : COLOR0;
    float4 D1 : COLOR1;
    float4 T0 : TEXCOORD0;
    float4 T1 : TEXCOORD1;
    float4 T2 : TEXCOORD2;
    float4 T3 : TEXCOORD3;
};

float4x3 GetWorldMatrix(GENERIC_INPUT IN) {
    // Stock VS1.1 computes constant offsets as
    // blend_index * c9.w + c5.w. c9.w is the three-register matrix stride;
    // HLSL array addressing supplies that stride, leaving the stock +0.5
    // conversion convention represented by c5.w.
    float2 Indices = IN.BlendIndices + c_eye_forward.ww;
    int NodeIndex0 = (int)Indices.x;
    int NodeIndex1 = (int)Indices.y;
    float4x3 WorldMatrix = c_node_matrices[NodeIndex0] * IN.BlendWeights.x;
    WorldMatrix += c_node_matrices[NodeIndex1] * IN.BlendWeights.y;
    return WorldMatrix;
}

float GenericAttenuation(float4 WorldPosition) {
    float Fog1 = dot(WorldPosition, c_fog_gradient1);
    float Fog2 = dot(WorldPosition, c_fog_gradient2);
    float Plane = dot(WorldPosition, c_fog_gradient0);

    float A = max(1.0f - Fog1, 0.0f);
    float B = max(1.0f - Fog2, 0.0f);
    float P = max(Plane, 0.0f);

    float A2 = min(A * A, 1.0f);
    float B2 = min(B * B, 1.0f);
    P = min(P, 1.0f);

    float Sum = min(A2 + B2, 1.0f);
    float X = 1.0f - Sum;
    float Y = 1.0f - B2;
    X *= X;
    Y = Y * Y - X;

    float Fog = c_fog_densities.y * Y + X;
    float PlaneTerm = P * c_fog_densities.x;
    float FogTerm = Fog * c_fog_densities.z;
    return (1.0f - PlaneTerm) * (1.0f - FogTerm);
}

float2 TransformUV(float2 UV, float4 XformX, float4 XformY) {
    float3 U = float3(UV, 1.0f);
    return float2(
        dot(U, float3(XformX.x, XformX.y, XformX.w)),
        dot(U, float3(XformY.x, XformY.y, XformY.w))
    );
}

GENERIC_OUTPUT main_generic_m(GENERIC_INPUT IN) {
    GENERIC_OUTPUT OUT = (GENERIC_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrix(IN);

    float3 WorldPosition3 = mul(float4(IN.Position.xyz, 1.0f), WorldMatrix);
    float4 WorldPosition = float4(WorldPosition3, 1.0f);
    OUT.Position = mul(WorldPosition, c_world_view_projection);

    OUT.T0.xy = TransformUV(IN.TexCoord.xy, c_tex0_x, c_tex0_y);
    OUT.T0.zw = float2(0.0f, 1.0f);
    OUT.T1.xy = TransformUV(IN.TexCoord.xy, c_tex1_x, c_tex1_y);
    OUT.T1.zw = 0.0f;
    OUT.T2.xy = TransformUV(IN.TexCoord.xy, c_tex2_x, c_tex2_y);
    OUT.T2.zw = 0.0f;
    OUT.T3.xy = TransformUV(IN.TexCoord.xy, c_tex3_x, c_tex3_y);
    OUT.T3.zw = 0.0f;

    float Attenuation = GenericAttenuation(WorldPosition);
    float Alpha = Attenuation * c_generic_alpha.z;
    OUT.D0 = float4(0.0f, 0.0f, 0.0f, Alpha);

    float3 WorldNormal = normalize(mul(IN.Normal, (float3x3)WorldMatrix));
    float Facing = abs(dot(WorldNormal, -c_eye_forward.xyz));
    OUT.D1 = Attenuation * float4(Facing, Facing, Facing, 1.0f - Facing) * c_generic_alpha.z;
    return OUT;
}
)HLSL";

        // Reconstructed directly from VSH_TRANSPARENT_PLASMA_M stock assembly.
        // Important stock details retained here:
        //  * two-node model skinning;
        //  * vertex offset along the skinned normal using c13.z;
        //  * noise coordinates generated from the original unskinned position;
        //  * view-angle colour and stock fog/alpha attenuation.
        static constexpr const char *plasma_m_hlsl = R"HLSL(
float4x4 c_world_view_projection : register(c0);
float4 c_eye_position : register(c4);
float4 c_eye_forward : register(c5);
float4 c_fog_gradient0 : register(c6);
float4 c_fog_gradient1 : register(c7);
float4 c_fog_gradient2 : register(c8);
float4 c_fog_densities : register(c9);
float4 c_plasma_parallel : register(c11);
float4 c_plasma_delta : register(c12);
float4 c_noise0_x : register(c13);
float4 c_noise0_y : register(c14);
float4 c_noise0_z : register(c15);
float4 c_noise1_x : register(c16);
float4 c_noise1_y : register(c17);
float4 c_noise1_z : register(c18);
float4x3 c_node_matrices[22] : register(c29);

struct PLASMA_INPUT {
    float4 Position : POSITION0;
    float3 Normal : NORMAL0;
    float3 BiNormal : BINORMAL0;
    float3 Tangent : TANGENT0;
    float4 TexCoord : TEXCOORD0;
    float2 BlendIndices : BLENDINDICES0;
    float2 BlendWeights : BLENDWEIGHT0;
};

struct PLASMA_OUTPUT {
    float4 Position : POSITION0;
    float4 D0 : COLOR0;
    float3 T0 : TEXCOORD0;
    float3 T1 : TEXCOORD1;
};

float4x3 GetWorldMatrix(PLASMA_INPUT IN) {
    float2 Indices = IN.BlendIndices + c_eye_forward.ww;
    int NodeIndex0 = (int)Indices.x;
    int NodeIndex1 = (int)Indices.y;
    float4x3 WorldMatrix = c_node_matrices[NodeIndex0] * IN.BlendWeights.x;
    WorldMatrix += c_node_matrices[NodeIndex1] * IN.BlendWeights.y;
    return WorldMatrix;
}

float PlasmaAttenuation(float4 OffsetWorldPosition, float2 ClampZW) {
    float Low = ClampZW.x;
    float High = ClampZW.y;

    float A = max(High - dot(OffsetWorldPosition, c_fog_gradient1), Low);
    float B = max(High - dot(OffsetWorldPosition, c_fog_gradient2), Low);
    float P = max(dot(OffsetWorldPosition, c_fog_gradient0), Low);

    A = min(A * A, High);
    B = min(B * B, High);
    P = min(P, High);

    float X = High - min(A + B, High);
    float Y = High - B;
    X *= X;
    Y = Y * Y - X;

    float Fog = (c_fog_densities.y * Y + X) * c_fog_densities.z;
    float Plane = P * c_fog_densities.x;
    return (High - Plane) * (High - Fog);
}

PLASMA_OUTPUT main_plasma_m(PLASMA_INPUT IN) {
    PLASMA_OUTPUT OUT = (PLASMA_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrix(IN);

    float3 WorldPosition = mul(float4(IN.Position.xyz, 1.0f), WorldMatrix);
    float3 WorldNormal = normalize(mul(IN.Normal, (float3x3)WorldMatrix));
    float3 EyeVector = normalize(c_eye_position.xyz - WorldPosition);

    float4 OffsetWorldPosition = float4(
        WorldPosition + WorldNormal * c_noise0_x.z,
        IN.TexCoord.w
    );
    OUT.Position = mul(OffsetWorldPosition, c_world_view_projection);

    // The stock shader intentionally derives both noise volumes from v0,
    // before the normal offset and before node skinning affect the position.
    OUT.T0 = float3(
        dot(IN.Position, c_noise0_x),
        dot(IN.Position, c_noise0_y),
        dot(IN.Position, c_noise0_z)
    );
    OUT.T1 = float3(
        dot(IN.Position, c_noise1_x),
        dot(IN.Position, c_noise1_y),
        dot(IN.Position, c_noise1_z)
    );

    float Facing = dot(EyeVector, WorldNormal);
    float4 PlasmaColor = c_plasma_parallel + Facing * c_plasma_delta;
    float Attenuation = PlasmaAttenuation(
        OffsetWorldPosition,
        float2(IN.TexCoord.z, IN.TexCoord.w)
    );
    OUT.D0 = float4(PlasmaColor.xyz, PlasmaColor.w * Attenuation);
    return OUT;
}
)HLSL";

        static bool d3d9on12_requested() noexcept {
            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            return backend
                && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
        }

        static IDirect3DDevice9 *live_device() noexcept {
            return global_d3d9_device ? *global_d3d9_device : nullptr;
        }

        static const char *generic_mode_name() noexcept {
            switch(generic_mode) {
                case GENERIC_MODE_EXACT_VS2: return "exact-vs2";
                case GENERIC_MODE_LEGACY_VS3: return "legacy-vs3";
                default: return "stock";
            }
        }

        static void release_owned_refs() noexcept {
            if(exact_generic_m) {
                exact_generic_m->Release();
                exact_generic_m = nullptr;
            }
            if(exact_plasma_m) {
                exact_plasma_m->Release();
                exact_plasma_m = nullptr;
            }
            if(legacy_generic_m) {
                legacy_generic_m->Release();
                legacy_generic_m = nullptr;
            }
            if(stock_generic_m) {
                stock_generic_m->Release();
                stock_generic_m = nullptr;
            }
            if(stock_plasma_m) {
                stock_plasma_m->Release();
                stock_plasma_m = nullptr;
            }
            compiled_device = nullptr;
        }

        static bool capture_stock_shaders() noexcept {
            if(!d3d9on12_requested() || !vertex_shaders) {
                return false;
            }
            if(!stock_generic_m) {
                stock_generic_m = vertex_shaders[VSH_TRANSPARENT_GENERIC_M].shader;
                if(!stock_generic_m) {
                    console_error("D3D9 transparent compat: stock VSH_TRANSPARENT_GENERIC_M is unavailable.");
                    return false;
                }
                stock_generic_m->AddRef();
            }
            if(!stock_plasma_m) {
                stock_plasma_m = vertex_shaders[VSH_TRANSPARENT_PLASMA_M].shader;
                if(!stock_plasma_m) {
                    console_error("D3D9 transparent compat: stock VSH_TRANSPARENT_PLASMA_M is unavailable.");
                    return false;
                }
                stock_plasma_m->AddRef();
            }
            return true;
        }

        static bool compile_shader(
            IDirect3DDevice9 *device,
            const char *source,
            const char *source_name,
            const char *entry,
            IDirect3DVertexShader9 **out
        ) noexcept {
            if(!device || !source || !entry || !out) {
                return false;
            }
            *out = nullptr;

            ID3DBlob *bytecode = nullptr;
            ID3DBlob *errors = nullptr;
            const HRESULT compile_result = D3DCompile(
                source,
                std::strlen(source),
                source_name,
                nullptr,
                nullptr,
                entry,
                "vs_2_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0,
                &bytecode,
                &errors
            );

            if(FAILED(compile_result) || !bytecode) {
                if(errors && errors->GetBufferPointer()) {
                    console_error(
                        "D3D9 transparent compat compile failed (%s): %s",
                        entry,
                        static_cast<const char *>(errors->GetBufferPointer())
                    );
                }
                else {
                    console_error(
                        "D3D9 transparent compat compile failed (%s), HRESULT=0x%08lX.",
                        entry,
                        static_cast<unsigned long>(compile_result)
                    );
                }
                if(errors) errors->Release();
                if(bytecode) bytecode->Release();
                return false;
            }

            const HRESULT create_result = device->CreateVertexShader(
                static_cast<const DWORD *>(bytecode->GetBufferPointer()),
                out
            );
            if(errors) errors->Release();
            bytecode->Release();

            if(FAILED(create_result) || !*out) {
                if(*out) {
                    (*out)->Release();
                    *out = nullptr;
                }
                console_error(
                    "D3D9 transparent compat CreateVertexShader failed (%s), HRESULT=0x%08lX.",
                    entry,
                    static_cast<unsigned long>(create_result)
                );
                return false;
            }
            return true;
        }

        static bool prepare_exact_shaders() noexcept {
            IDirect3DDevice9 *device = live_device();
            if(!device || !capture_stock_shaders()) {
                return false;
            }
            if(compiled_device == device && exact_generic_m && exact_plasma_m) {
                return true;
            }
            if(compiled_device && compiled_device != device) {
                console_error("D3D9 transparent compat: D3D9 device changed; restart the map before changing compatibility modes.");
                return false;
            }

            if(!exact_generic_m
                && !compile_shader(device, generic_m_hlsl, "chimera_d3d9_generic_m_exact", "main_generic_m", &exact_generic_m)) {
                return false;
            }
            if(!exact_plasma_m
                && !compile_shader(device, plasma_m_hlsl, "chimera_d3d9_plasma_m_exact", "main_plasma_m", &exact_plasma_m)) {
                if(exact_generic_m) {
                    exact_generic_m->Release();
                    exact_generic_m = nullptr;
                }
                return false;
            }
            compiled_device = device;
            console_output("D3D9 transparent compat: exact GENERIC_M/PLASMA_M VS2 shaders compiled.");
            return true;
        }

        static bool restore_generic_m() noexcept {
            if(generic_mode == GENERIC_MODE_STOCK) {
                return true;
            }
            if(vertex_shaders && stock_generic_m) {
                auto &slot = vertex_shaders[VSH_TRANSPARENT_GENERIC_M].shader;
                IDirect3DVertexShader9 *active = generic_mode == GENERIC_MODE_EXACT_VS2
                    ? exact_generic_m
                    : legacy_generic_m;
                if(slot == active) {
                    slot = stock_generic_m;
                }
                else if(slot != stock_generic_m) {
                    console_output("D3D9 transparent compat: GENERIC_M slot changed externally; leaving it untouched.");
                }
            }
            generic_mode = GENERIC_MODE_STOCK;
            console_output("D3D9 transparent compat: VSH_TRANSPARENT_GENERIC_M restored to stock.");
            return true;
        }

        static bool restore_plasma_m() noexcept {
            if(!plasma_m_active) {
                return true;
            }
            if(vertex_shaders && stock_plasma_m) {
                auto &slot = vertex_shaders[VSH_TRANSPARENT_PLASMA_M].shader;
                if(slot == exact_plasma_m) {
                    slot = stock_plasma_m;
                }
                else if(slot != stock_plasma_m) {
                    console_output("D3D9 transparent compat: PLASMA_M slot changed externally; leaving it untouched.");
                }
            }
            plasma_m_active = false;
            console_output("D3D9 transparent compat: VSH_TRANSPARENT_PLASMA_M restored to stock.");
            return true;
        }

        static bool enable_generic_m_exact() noexcept {
            if(generic_mode == GENERIC_MODE_EXACT_VS2) {
                console_output("D3D9 transparent compat: GENERIC_M exact VS2 is already enabled.");
                return true;
            }
            if(!prepare_exact_shaders()) {
                return false;
            }
            restore_generic_m();

            auto &slot = vertex_shaders[VSH_TRANSPARENT_GENERIC_M].shader;
            if(slot != stock_generic_m) {
                console_error("D3D9 transparent compat: GENERIC_M slot is not stock; refusing unsafe replacement.");
                return false;
            }
            slot = exact_generic_m;
            generic_mode = GENERIC_MODE_EXACT_VS2;
            console_output("D3D9 transparent compat: VSH_TRANSPARENT_GENERIC_M -> exact stock-equivalent VS2 shader.");
            return true;
        }

        static bool enable_generic_m_legacy_vs3() noexcept {
            if(generic_mode == GENERIC_MODE_LEGACY_VS3) {
                console_output("D3D9 transparent compat: GENERIC_M legacy VS3 is already enabled.");
                return true;
            }
            if(!capture_stock_shaders()) {
                return false;
            }

            IDirect3DVertexShader9 *candidate = rasterizer_get_vertex_shader(VSH_TRANSPARENT_GENERIC_M);
            if(!candidate || candidate == stock_generic_m) {
                console_error("D3D9 transparent compat: no distinct legacy Chimera GENERIC_M VS3 shader is available.");
                return false;
            }
            if(!legacy_generic_m) {
                legacy_generic_m = candidate;
                legacy_generic_m->AddRef();
            }
            restore_generic_m();

            auto &slot = vertex_shaders[VSH_TRANSPARENT_GENERIC_M].shader;
            if(slot != stock_generic_m) {
                console_error("D3D9 transparent compat: GENERIC_M slot is not stock; refusing unsafe replacement.");
                return false;
            }
            slot = legacy_generic_m;
            generic_mode = GENERIC_MODE_LEGACY_VS3;
            console_output("D3D9 transparent compat: VSH_TRANSPARENT_GENERIC_M -> legacy Chimera VS3 shader.");
            return true;
        }

        static bool enable_plasma_m_exact() noexcept {
            if(plasma_m_active) {
                console_output("D3D9 transparent compat: PLASMA_M exact VS2 is already enabled.");
                return true;
            }
            if(!prepare_exact_shaders()) {
                return false;
            }

            auto &slot = vertex_shaders[VSH_TRANSPARENT_PLASMA_M].shader;
            if(slot != stock_plasma_m) {
                console_error("D3D9 transparent compat: PLASMA_M slot is not stock; refusing unsafe replacement.");
                return false;
            }
            slot = exact_plasma_m;
            plasma_m_active = true;
            console_output("D3D9 transparent compat: VSH_TRANSPARENT_PLASMA_M -> exact stock-equivalent VS2 shader.");
            return true;
        }

        static bool dump_shader(std::FILE *output, IDirect3DVertexShader9 *shader, const char *name) noexcept {
            if(!output || !shader || !name) {
                return false;
            }

            UINT byte_count = 0;
            HRESULT result = shader->GetFunction(nullptr, &byte_count);
            if(FAILED(result) || byte_count == 0) {
                std::fprintf(output, "===== %s =====\nGetFunction(size) failed hr=0x%08lX size=%u\n\n",
                    name, static_cast<unsigned long>(result), byte_count);
                return false;
            }

            std::vector<unsigned char> bytecode(byte_count);
            result = shader->GetFunction(bytecode.data(), &byte_count);
            if(FAILED(result)) {
                std::fprintf(output, "===== %s =====\nGetFunction(data) failed hr=0x%08lX\n\n",
                    name, static_cast<unsigned long>(result));
                return false;
            }

            ID3DBlob *disassembly = nullptr;
            result = D3DDisassemble(bytecode.data(), byte_count, 0, name, &disassembly);
            if(FAILED(result) || !disassembly) {
                std::fprintf(output, "===== %s =====\nD3DDisassemble failed hr=0x%08lX bytes=%u\n\n",
                    name, static_cast<unsigned long>(result), byte_count);
                if(disassembly) disassembly->Release();
                return false;
            }

            std::fprintf(output, "===== %s =====\n", name);
            std::fwrite(disassembly->GetBufferPointer(), 1, disassembly->GetBufferSize(), output);
            std::fprintf(output, "\n===== END %s =====\n\n", name);
            disassembly->Release();
            return true;
        }

        static bool dump_shaders() noexcept {
            if(!capture_stock_shaders()) {
                console_error("D3D9 transparent compat: stock shaders are not available yet.");
                return false;
            }

            // Compile our exact candidates as part of the dump when possible so
            // the resulting file can compare stock VS1.1 and compatibility VS2.
            const bool exact_ready = prepare_exact_shaders();
            IDirect3DVertexShader9 *legacy = rasterizer_get_vertex_shader(VSH_TRANSPARENT_GENERIC_M);

            std::FILE *output = std::fopen("chimera_d3d9_transparent_vs.asm.log", "w");
            if(!output) {
                console_error("D3D9 transparent compat: could not create chimera_d3d9_transparent_vs.asm.log.");
                return false;
            }

            bool ok = true;
            ok = dump_shader(output, stock_generic_m, "VSH_TRANSPARENT_GENERIC_M_STOCK") && ok;
            if(exact_ready) {
                ok = dump_shader(output, exact_generic_m, "VSH_TRANSPARENT_GENERIC_M_EXACT_VS2") && ok;
            }
            if(legacy && legacy != stock_generic_m && legacy != exact_generic_m) {
                ok = dump_shader(output, legacy, "VSH_TRANSPARENT_GENERIC_M_LEGACY_CHIMERA_VS3") && ok;
            }
            ok = dump_shader(output, stock_plasma_m, "VSH_TRANSPARENT_PLASMA_M_STOCK") && ok;
            if(exact_ready) {
                ok = dump_shader(output, exact_plasma_m, "VSH_TRANSPARENT_PLASMA_M_EXACT_VS2") && ok;
            }
            std::fclose(output);

            if(ok) {
                console_output("D3D9 transparent compat: stock/compat shader assembly -> chimera_d3d9_transparent_vs.asm.log");
                return true;
            }
            console_error("D3D9 transparent compat: shader dump was incomplete; inspect chimera_d3d9_transparent_vs.asm.log.");
            return false;
        }

        static void print_status() noexcept {
            console_output(
                "D3D9 transparent compat: generic_m=%s plasma_m=%s",
                generic_mode_name(),
                plasma_m_active ? "exact-vs2" : "stock"
            );
            console_output(
                "D3D9 transparent compat caps: VS=0x%08lX PS=0x%08lX",
                d3d9_device_caps ? static_cast<unsigned long>(d3d9_device_caps->VertexShaderVersion) : 0UL,
                d3d9_device_caps ? static_cast<unsigned long>(d3d9_device_caps->PixelShaderVersion) : 0UL
            );
        }

        static void print_help() noexcept {
            console_output("chimera_d3d9_compat status");
            console_output("chimera_d3d9_compat generic_m on|exact|vs3|off");
            console_output("chimera_d3d9_compat plasma_m on|exact|off");
            console_output("chimera_d3d9_compat dump");
        }

        static bool command(int argc, const char **argv) noexcept {
            if(!d3d9on12_requested()) {
                console_error("D3D9 transparent compat is only available with video_mode.d3d_backend=9on12.");
                return false;
            }

            if(argc == 0 || _stricmp(argv[0], "status") == 0) {
                print_status();
                return true;
            }
            if(_stricmp(argv[0], "help") == 0) {
                print_help();
                return true;
            }
            if(_stricmp(argv[0], "dump") == 0) {
                return dump_shaders();
            }
            if(_stricmp(argv[0], "generic_m") == 0) {
                if(argc < 2) {
                    console_error("D3D9 transparent compat: use generic_m on|exact|vs3|off.");
                    return false;
                }
                if(_stricmp(argv[1], "on") == 0
                    || _stricmp(argv[1], "exact") == 0
                    || _stricmp(argv[1], "compat") == 0) {
                    return enable_generic_m_exact();
                }
                if(_stricmp(argv[1], "vs3") == 0 || _stricmp(argv[1], "legacy") == 0) {
                    return enable_generic_m_legacy_vs3();
                }
                if(_stricmp(argv[1], "off") == 0 || _stricmp(argv[1], "stock") == 0) {
                    return restore_generic_m();
                }
                console_error("D3D9 transparent compat: use generic_m on|exact|vs3|off.");
                return false;
            }
            if(_stricmp(argv[0], "plasma_m") == 0 || _stricmp(argv[0], "plasma") == 0) {
                if(argc < 2) {
                    console_error("D3D9 transparent compat: use plasma_m on|exact|off.");
                    return false;
                }
                if(_stricmp(argv[1], "on") == 0
                    || _stricmp(argv[1], "exact") == 0
                    || _stricmp(argv[1], "compat") == 0) {
                    return enable_plasma_m_exact();
                }
                if(_stricmp(argv[1], "off") == 0 || _stricmp(argv[1], "stock") == 0) {
                    return restore_plasma_m();
                }
                console_error("D3D9 transparent compat: use plasma_m on|exact|off.");
                return false;
            }

            console_error("D3D9 transparent compat: unknown action '%s'.", argv[0]);
            print_help();
            return false;
        }

        static void on_game_exit() noexcept {
            restore_plasma_m();
            restore_generic_m();
            release_owned_refs();
        }
    }
}

#endif
