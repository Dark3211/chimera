// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_MODEL_SHADER_COMPAT_HPP
#define CHIMERA_D3D9_MODEL_SHADER_COMPAT_HPP

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../event/d3d9_end_scene.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace D3D9ModelShaderCompat {
        constexpr std::size_t DEVICE_SET_VERTEX_SHADER = 92;

        using SetVertexShaderFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, IDirect3DVertexShader9 *
        );

        enum class Slot : std::size_t {
            MODEL = 0,
            FOGGED,
            FIXED_FUNCTION,
            ACTIVE_CAMO,
            ACTIVE_CAMO_FF,
            FOG_SCREEN,
            SHADOW,
            ZBUFFER,
            COUNT
        };

        struct Replacement {
            VertexShaderIndex stock_index;
            Slot slot;
            const char *name;
            std::uint32_t bit;
        };

        static constexpr Replacement replacements[] = {
            {VSH_MODEL_FOGGED,              Slot::FOGGED,          "VSH_MODEL_FOGGED",              1u << 0},
            {VSH_MODEL,                     Slot::MODEL,           "VSH_MODEL",                     1u << 1},
            {VSH_MODEL_FF,                  Slot::FIXED_FUNCTION,  "VSH_MODEL_FF",                  1u << 2},
            {VSH_MODEL_FAST,                Slot::MODEL,           "VSH_MODEL_FAST",                1u << 3},
            {VSH_MODEL_ACTIVE_CAMOUFLAGE,   Slot::ACTIVE_CAMO,     "VSH_MODEL_ACTIVE_CAMOUFLAGE",   1u << 4},
            {VSH_MODEL_ACTIVE_CAMOUFLAGE_FF,Slot::ACTIVE_CAMO_FF,  "VSH_MODEL_ACTIVE_CAMOUFLAGE_FF",1u << 5},
            {VSH_MODEL_FOG_SCREEN,          Slot::FOG_SCREEN,      "VSH_MODEL_FOG_SCREEN",          1u << 6},
            {VSH_MODEL_SHADOW,              Slot::SHADOW,          "VSH_MODEL_SHADOW",              1u << 7},
            {VSH_MODEL_ZBUFFER,             Slot::ZBUFFER,         "VSH_MODEL_ZBUFFER",             1u << 8},
        };

        static SetVertexShaderFunction original_set_vertex_shader = nullptr;
        static IDirect3DDevice9 *installed_device = nullptr;
        static IDirect3DDevice9 *shader_set_device = nullptr;
        static IDirect3DVertexShader9 *shader_set[static_cast<std::size_t>(Slot::COUNT)] = {};
        static bool queued_announced = false;
        static bool installed_announced = false;
        static bool end_scene_retry_registered = false;
        static std::uint32_t replacement_hit_mask = 0;

        static constexpr const char *compat_hlsl = R"HLSL(
float4x4 c_world_view_projection : register(c0);
float4 c_eye_position : register(c4);
float4 c_eye_forward : register(c5);
float4 c_planar_fog_gradient1 : register(c6);
float4 c_planar_fog_gradient2 : register(c7);
float4 c_planar_fog_gradient_unused : register(c8);
float4 c_fog_densities : register(c9);
float4 c_fog_screen_gradient : register(c10);
float4 c_base_map_xform_x : register(c11);
float4 c_base_map_xform_y : register(c12);
float4 c_shared_13 : register(c13);
float4 c_shared_14 : register(c14);
float4 c_shared_15 : register(c15);
float4 c_shared_16 : register(c16);
float4 c_eye_xform_x : register(c27);
float4 c_eye_xform_y : register(c28);
float4x3 c_node_matrices[22] : register(c29);

struct VS_INPUT {
    float4 Position : POSITION0;
    float3 Normal : NORMAL0;
    float3 BiNormal : BINORMAL0;
    float3 Tangent : TANGENT0;
    float4 TexCoord0 : TEXCOORD0;
    float2 BlendIndices : BLENDINDICES0;
    float2 BlendWeights : BLENDWEIGHT0;
};

float4x3 GetWorldMatrix(VS_INPUT IN) {
    // Halo stores two node selectors in BLENDINDICES. The stock shader adds
    // c5.w (0.5) before using them for relative constant addressing.
    float2 Indices = IN.BlendIndices + c_eye_forward.ww;
    int NodeIndex0 = (int)Indices.x;
    int NodeIndex1 = (int)Indices.y;
    float4x3 WorldMatrix = c_node_matrices[NodeIndex0] * IN.BlendWeights.x;
    WorldMatrix += c_node_matrices[NodeIndex1] * IN.BlendWeights.y;
    return WorldMatrix;
}

float3 GetWorldPosition(VS_INPUT IN, float4x3 WorldMatrix) {
    return mul(IN.Position, WorldMatrix);
}

float4 MulScreenProjection(float4 Position) {
    // SCREENPROJ occupies c13-c16. c15 is also the model normal-scale register
    // in ordinary model passes, so keep the physical registers explicit here.
    return float4(
        dot(Position, c_shared_13),
        dot(Position, c_shared_14),
        dot(Position, c_shared_15),
        dot(Position, c_shared_16)
    );
}

float CalculatePlanarFog(float3 WorldPosition) {
    float PlanarFogVertexDensity = dot(WorldPosition, c_planar_fog_gradient1.xyz) + c_planar_fog_gradient1.w;
    float PlanarFogEyeDistance = dot(WorldPosition, c_planar_fog_gradient2.xyz) + c_planar_fog_gradient2.w;
    float2 FogDensity = 1.0f - float2(PlanarFogVertexDensity, PlanarFogEyeDistance);
    FogDensity = max(FogDensity, 0.0f);
    FogDensity *= FogDensity;
    FogDensity = min(FogDensity, 1.0f);
    FogDensity.x = min(FogDensity.x + FogDensity.y, 1.0f);
    FogDensity = 1.0f - FogDensity;
    FogDensity *= FogDensity;
    FogDensity.y -= FogDensity.x;
    return (c_fog_densities.y * FogDensity.y + FogDensity.x) * c_fog_densities.z;
}

struct MODEL_OUTPUT {
    float4 Position : POSITION0;
    float Fog : FOG;
    float3 Position3D : TEXCOORD0;
    float4 DiffuseMultiUV : TEXCOORD1;
    float4 DetailUV : TEXCOORD2;
    float3x3 TBNTranspose : TEXCOORD3;
    float4 NormalDetailUV : TEXCOORD6;
};

MODEL_OUTPUT BuildModelOutput(VS_INPUT IN, bool Fogged) {
    MODEL_OUTPUT OUT = (MODEL_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrix(IN);
    float4 WorldPosition;
    WorldPosition.xyz = GetWorldPosition(IN, WorldMatrix);
    WorldPosition.w = IN.TexCoord0.w;

    OUT.Position3D = WorldPosition.xyz;

    float3x3 WorldToTangentSpace;
    WorldToTangentSpace[0] = mul(IN.Tangent, (float3x3)WorldMatrix);
    WorldToTangentSpace[1] = mul(IN.BiNormal, (float3x3)WorldMatrix);
    WorldToTangentSpace[2] = mul(IN.Normal, (float3x3)WorldMatrix) * c_fog_screen_gradient.w;
    OUT.TBNTranspose = transpose(WorldToTangentSpace);

    OUT.Position = mul(WorldPosition, c_world_view_projection);
    OUT.DiffuseMultiUV.x = dot(IN.TexCoord0, c_base_map_xform_x);
    OUT.DiffuseMultiUV.y = dot(IN.TexCoord0, c_base_map_xform_y);
    OUT.DiffuseMultiUV.zw = OUT.DiffuseMultiUV.xy;
    OUT.DetailUV.xy = OUT.DiffuseMultiUV.xy * c_fog_screen_gradient.xy;
    OUT.DetailUV.z = 0.0f;
    OUT.DetailUV.w = c_base_map_xform_y.z;
    OUT.NormalDetailUV.xy = OUT.DiffuseMultiUV.xy * c_shared_15.xy;
    OUT.NormalDetailUV.zw = OUT.DiffuseMultiUV.xy * c_shared_15.zw;

    if(Fogged) {
        float FogDensity = CalculatePlanarFog(WorldPosition.xyz);
        OUT.DetailUV.z = FogDensity;
        OUT.Fog = IN.TexCoord0.w - FogDensity;
    }
    return OUT;
}

MODEL_OUTPUT main_model(VS_INPUT IN) {
    return BuildModelOutput(IN, false);
}

MODEL_OUTPUT main_fogged(VS_INPUT IN) {
    return BuildModelOutput(IN, true);
}

struct SIMPLE_OUTPUT {
    float4 Position : POSITION0;
    float2 T0 : TEXCOORD0;
};

SIMPLE_OUTPUT main_ff(VS_INPUT IN) {
    SIMPLE_OUTPUT OUT = (SIMPLE_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrix(IN);
    float4 WorldPosition = float4(GetWorldPosition(IN, WorldMatrix), 1.0f);
    OUT.Position = mul(WorldPosition, c_world_view_projection);
    OUT.T0 = IN.TexCoord0.xy;
    return OUT;
}

SIMPLE_OUTPUT main_zbuffer(VS_INPUT IN) {
    SIMPLE_OUTPUT OUT = (SIMPLE_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrix(IN);
    float4 WorldPosition = float4(GetWorldPosition(IN, WorldMatrix), 1.0f);
    OUT.Position = mul(WorldPosition, c_world_view_projection);
    OUT.T0.x = dot(IN.TexCoord0, c_base_map_xform_x);
    OUT.T0.y = dot(IN.TexCoord0, c_base_map_xform_y);
    return OUT;
}

SIMPLE_OUTPUT main_shadow(VS_INPUT IN) {
    SIMPLE_OUTPUT OUT = (SIMPLE_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrix(IN);
    float4 WorldPosition = float4(GetWorldPosition(IN, WorldMatrix), 1.0f);
    OUT.Position = MulScreenProjection(WorldPosition);
    OUT.T0.x = dot(IN.TexCoord0, c_base_map_xform_x);
    OUT.T0.y = dot(IN.TexCoord0, c_base_map_xform_y);
    return OUT;
}

struct FOG_SCREEN_OUTPUT {
    float4 Position : POSITION0;
    float2 T0 : TEXCOORD0;
    float2 T1 : TEXCOORD1;
    float Fog : FOG;
};

FOG_SCREEN_OUTPUT main_fog_screen(VS_INPUT IN) {
    FOG_SCREEN_OUTPUT OUT = (FOG_SCREEN_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrix(IN);
    float4 WorldPosition = float4(GetWorldPosition(IN, WorldMatrix), 1.0f);
    OUT.Position = mul(WorldPosition, c_world_view_projection);
    OUT.Fog = dot(WorldPosition, c_fog_screen_gradient);
    OUT.T0 = float2(OUT.Fog, 0.0f);
    OUT.T1.x = dot(IN.TexCoord0, c_base_map_xform_x);
    OUT.T1.y = dot(IN.TexCoord0, c_base_map_xform_y);
    return OUT;
}

struct CAMO_OUTPUT {
    float4 Position : POSITION0;
    float4 Diffuse : COLOR0;
    float3 T0 : TEXCOORD0;
    float3 T1 : TEXCOORD1;
    float3 T2 : TEXCOORD2;
};

CAMO_OUTPUT main_active_camo(VS_INPUT IN) {
    CAMO_OUTPUT OUT = (CAMO_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrix(IN);
    float3 WorldPosition3 = GetWorldPosition(IN, WorldMatrix);
    float4 WorldPosition = float4(WorldPosition3, 1.0f);
    float3 WorldNormal = normalize(mul(IN.Normal, (float3x3)WorldMatrix));

    float3 EyeVector = c_eye_position.xyz - WorldPosition3;
    float EyeDistance = dot(EyeVector, c_eye_forward.xyz);
    float CamoFactor = saturate((1.0f / (EyeDistance * EyeDistance)) * c_fog_screen_gradient.y);

    OUT.Diffuse.xyz = c_base_map_xform_y.xyz;
    OUT.Diffuse.w = CamoFactor;
    OUT.Position = mul(WorldPosition, c_world_view_projection);
    OUT.T0.x = dot(WorldNormal, c_eye_xform_x.xyz);
    OUT.T0.y = dot(WorldNormal, c_eye_xform_y.xyz);
    OUT.T0.z = dot(WorldNormal, c_eye_forward.xyz);
    OUT.T1.xy = float2(CamoFactor * c_fog_screen_gradient.x, 0.0f);
    OUT.T2.xy = float2(0.0f, CamoFactor * c_fog_screen_gradient.x);

    float2 Projected = OUT.Position.xy / OUT.Position.w;
    Projected = (Projected + float2(1.0f, -1.0f)) * 0.5f;
    Projected.y *= -1.0f;
    OUT.T1.z = c_fog_screen_gradient.z * Projected.x;
    OUT.T2.z = c_fog_screen_gradient.w * Projected.y;
    return OUT;
}

struct CAMO_FF_OUTPUT {
    float4 Position : POSITION0;
    float4 Diffuse : COLOR0;
    float2 T0 : TEXCOORD0;
};

CAMO_FF_OUTPUT main_active_camo_ff(VS_INPUT IN) {
    CAMO_FF_OUTPUT OUT = (CAMO_FF_OUTPUT)0;
    float4x3 WorldMatrix = GetWorldMatrix(IN);
    float3 WorldPosition3 = GetWorldPosition(IN, WorldMatrix);
    float4 WorldPosition = float4(WorldPosition3, 1.0f);
    float3 WorldNormal = normalize(mul(IN.Normal, (float3x3)WorldMatrix));

    float3 EyeVector = c_eye_position.xyz - WorldPosition3;
    float EyeDistance = dot(EyeVector, c_eye_forward.xyz);
    float CamoFactor = saturate((1.0f / (EyeDistance * EyeDistance)) * c_fog_screen_gradient.y);
    OUT.Diffuse.xyz = c_base_map_xform_y.xyz;
    OUT.Diffuse.w = CamoFactor;
    OUT.Position = mul(WorldPosition, c_world_view_projection);

    float2 Projected = OUT.Position.xy / OUT.Position.w;
    Projected = (Projected + float2(1.0f, -1.0f)) * 0.5f;
    Projected.y *= -1.0f;
    OUT.T0.x = c_fog_screen_gradient.z * Projected.x;
    OUT.T0.y = c_fog_screen_gradient.w * Projected.y;
    return OUT;
}
)HLSL";

        static bool d3d9on12_requested() noexcept {
            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            return backend
                && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
        }

        static bool enabled() noexcept {
            if(!d3d9on12_requested()) {
                return false;
            }
            auto *value = get_chimera().get_ini()->get_value("video_mode.d3d_model_shader_compat");
            return value && (_stricmp(value, "vs2_family") == 0 || _stricmp(value, "vs2") == 0);
        }

        static const char *entry_for_slot(Slot slot) noexcept {
            switch(slot) {
                case Slot::MODEL: return "main_model";
                case Slot::FOGGED: return "main_fogged";
                case Slot::FIXED_FUNCTION: return "main_ff";
                case Slot::ACTIVE_CAMO: return "main_active_camo";
                case Slot::ACTIVE_CAMO_FF: return "main_active_camo_ff";
                case Slot::FOG_SCREEN: return "main_fog_screen";
                case Slot::SHADOW: return "main_shadow";
                case Slot::ZBUFFER: return "main_zbuffer";
                case Slot::COUNT: return nullptr;
            }
            return nullptr;
        }

        static void release_shader_set() noexcept {
            for(auto &shader : shader_set) {
                if(shader) {
                    shader->Release();
                    shader = nullptr;
                }
            }
            shader_set_device = nullptr;
            replacement_hit_mask = 0;
        }

        static bool compile_slot(IDirect3DDevice9 *device, Slot slot) noexcept {
            const char *entry = entry_for_slot(slot);
            if(!entry) {
                return false;
            }

            ID3DBlob *bytecode = nullptr;
            ID3DBlob *errors = nullptr;
            const HRESULT compile_result = D3DCompile(
                compat_hlsl,
                std::strlen(compat_hlsl),
                "chimera_d3d9_model_shader_compat",
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
                    console_output(
                        "D3D9 model VS2 compatibility compile failed (%s): %s",
                        entry,
                        static_cast<const char *>(errors->GetBufferPointer())
                    );
                }
                else {
                    console_output(
                        "D3D9 model VS2 compatibility compile failed (%s), HRESULT=0x%08lX.",
                        entry,
                        static_cast<unsigned long>(compile_result)
                    );
                }
                if(errors) errors->Release();
                if(bytecode) bytecode->Release();
                return false;
            }

            IDirect3DVertexShader9 *shader = nullptr;
            const HRESULT create_result = device->CreateVertexShader(
                static_cast<const DWORD *>(bytecode->GetBufferPointer()),
                &shader
            );
            if(errors) errors->Release();
            bytecode->Release();

            if(FAILED(create_result) || !shader) {
                console_output(
                    "D3D9 model VS2 compatibility CreateVertexShader failed (%s), HRESULT=0x%08lX.",
                    entry,
                    static_cast<unsigned long>(create_result)
                );
                if(shader) shader->Release();
                return false;
            }

            shader_set[static_cast<std::size_t>(slot)] = shader;
            return true;
        }

        static bool create_shader_set(IDirect3DDevice9 *device) noexcept {
            if(shader_set_device == device && shader_set[static_cast<std::size_t>(Slot::MODEL)]) {
                return true;
            }

            release_shader_set();
            bool any = false;
            for(std::size_t i = 0; i < static_cast<std::size_t>(Slot::COUNT); i++) {
                const Slot slot = static_cast<Slot>(i);
                if(compile_slot(device, slot)) {
                    any = true;
                }
            }
            if(!any) {
                release_shader_set();
                return false;
            }
            shader_set_device = device;
            console_output("D3D9 backend: compiled VS2 compatibility shaders for the Halo model family.");
            return true;
        }

        static HRESULT STDMETHODCALLTYPE set_vertex_shader_hook(
            IDirect3DDevice9 *device,
            IDirect3DVertexShader9 *shader
        ) {
            if(!original_set_vertex_shader) {
                return D3DERR_INVALIDCALL;
            }

            if(enabled() && vertex_shaders && shader_set_device == device) {
                for(const auto &replacement : replacements) {
                    if(vertex_shaders[replacement.stock_index].shader
                        && shader == vertex_shaders[replacement.stock_index].shader) {
                        auto *compat = shader_set[static_cast<std::size_t>(replacement.slot)];
                        if(compat) {
                            if(!(replacement_hit_mask & replacement.bit)) {
                                replacement_hit_mask |= replacement.bit;
                                console_output(
                                    "D3D9 model VS2 compatibility: replacing %s.",
                                    replacement.name
                                );
                            }
                            shader = compat;
                        }
                        break;
                    }
                }
            }

            return original_set_vertex_shader(device, shader);
        }

        static bool install(IDirect3DDevice9 *device) noexcept {
            if(!device || !enabled()) {
                return false;
            }
            if(!create_shader_set(device)) {
                return false;
            }

            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) {
                return false;
            }

            auto *entry = &vtable[DEVICE_SET_VERTEX_SHADER];
            const ULONG_PTR replacement = reinterpret_cast<ULONG_PTR>(set_vertex_shader_hook);
            if(*entry == replacement && installed_device == device && original_set_vertex_shader) {
                return true;
            }

            // Do not stack this compatibility hook on top of the earlier diagnostic
            // SetVertexShader hook. Run production-path tests with d3d_model_shader_test=off.
            original_set_vertex_shader = reinterpret_cast<SetVertexShaderFunction>(*entry);
            if(!original_set_vertex_shader) {
                return false;
            }

            DWORD old_protection = 0;
            if(!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE, &old_protection)) {
                original_set_vertex_shader = nullptr;
                return false;
            }

            *entry = replacement;
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(*entry), old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(*entry));
            installed_device = device;

            if(!installed_announced) {
                console_output("D3D9 backend: Halo model-family VS2 compatibility path enabled on D3D9On12.");
                installed_announced = true;
            }
            return true;
        }

        static void on_end_scene(IDirect3DDevice9 *device) noexcept {
            if(enabled()) {
                install(device);
            }
        }

        static void set_up() noexcept {
            if(!enabled()) {
                return;
            }

            if(!queued_announced) {
                console_output("D3D9 backend: model-family VS2 compatibility requested; waiting for live D3D9 device.");
                queued_announced = true;
            }

            if(!end_scene_retry_registered) {
                add_d3d9_end_scene_event(on_end_scene);
                end_scene_retry_registered = true;
            }

            if(global_d3d9_device && *global_d3d9_device) {
                install(*global_d3d9_device);
            }
        }
    }

    inline void set_up_d3d9_model_shader_compat() noexcept {
        D3D9ModelShaderCompat::set_up();
    }
}

#endif
