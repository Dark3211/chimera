// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_MODERN_SHADER_BANK_HPP
#define CHIMERA_D3D9_MODERN_SHADER_BANK_HPP

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstddef>
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
    namespace D3D9ModernShaderBank {
        static constexpr const char *vertex_shader_names[NUM_OF_VERTEX_SHADERS] = {
            "VSH_CONVOLUTION",
            "VSH_DEBUG",
            "VSH_DECAL",
            "VSH_DETAIL_OBJECT_TYPE0",
            "VSH_DETAIL_OBJECT_TYPE1",
            "VSH_EFFECT",
            "VSH_EFFECT_MULTITEXTURE",
            "VSH_EFFECT_MULTITEXTURE_SCREENSPACE",
            "VSH_EFFECT_ZSPRITE",
            "VSH_ENVIRONMENT_DIFFUSE_LIGHT",
            "VSH_ENVIRONMENT_DIFFUSE_LIGHT_FF",
            "VSH_ENVIRONMENT_FOG",
            "VSH_ENVIRONMENT_FOG_SCREEN",
            "VSH_ENVIRONMENT_LIGHTMAP",
            "VSH_ENVIRONMENT_REFLECTION_BUMPED",
            "VSH_ENVIRONMENT_REFLECTION_FLAT",
            "VSH_ENVIRONMENT_REFLECTION_LIGHTMAP_MASK",
            "VSH_ENVIRONMENT_REFLECTION_MIRROR",
            "VSH_ENVIRONMENT_REFLECTION_RADIOSITY",
            "VSH_ENVIRONMENT_SHADOW",
            "VSH_ENVIRONMENT_SPECULAR_LIGHT",
            "VSH_ENVIRONMENT_SPECULAR_SPOT_LIGHT",
            "VSH_ENVIRONMENT_SPECULAR_LIGHTMAP",
            "VSH_ENVIRONMENT_TEXTURE",
            "VSH_LENS_FLARE",
            "VSH_MODEL_FOGGED",
            "VSH_MODEL",
            "VSH_MODEL_FF",
            "VSH_MODEL_FAST",
            "VSH_MODEL_SCENERY",
            "VSH_MODEL_ACTIVE_CAMOUFLAGE",
            "VSH_MODEL_ACTIVE_CAMOUFLAGE_FF",
            "VSH_MODEL_FOG_SCREEN",
            "VSH_MODEL_SHADOW",
            "VSH_MODEL_ZBUFFER",
            "VSH_SCREEN",
            "VSH_SCREEN2",
            "VSH_TRANSPARENT_GENERIC",
            "VSH_TRANSPARENT_GENERIC_LIT_M",
            "VSH_TRANSPARENT_GENERIC_M",
            "VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED",
            "VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED_M",
            "VSH_TRANSPARENT_GENERIC_REFLECTION",
            "VSH_TRANSPARENT_GENERIC_REFLECTION_M",
            "VSH_TRANSPARENT_GENERIC_SCREENSPACE",
            "VSH_TRANSPARENT_GENERIC_SCREENSPACE_M",
            "VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED",
            "VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M",
            "VSH_TRANSPARENT_GLASS_DIFFUSE_LIGHT",
            "VSH_TRANSPARENT_GLASS_DIFFUSE_LIGHT_M",
            "VSH_TRANSPARENT_GLASS_REFLECTION_BUMPED",
            "VSH_TRANSPARENT_GLASS_REFLECTION_BUMPED_M",
            "VSH_TRANSPARENT_GLASS_REFLECTION_FLAT",
            "VSH_TRANSPARENT_GLASS_REFLECTION_FLAT_M",
            "VSH_TRANSPARENT_GLASS_REFLECTION_MIRROR",
            "VSH_TRANSPARENT_GLASS_TINT",
            "VSH_TRANSPARENT_GLASS_TINT_M",
            "VSH_TRANSPARENT_METER",
            "VSH_TRANSPARENT_METER_M",
            "VSH_TRANSPARENT_PLASMA_M",
            "VSH_TRANSPARENT_WATER_OPACITY",
            "VSH_TRANSPARENT_WATER_OPACITY_M",
            "VSH_TRANSPARENT_WATER_REFLECTION",
            "VSH_TRANSPARENT_WATER_REFLECTION_M"
        };

        static bool d3d9on12_requested() noexcept {
            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            return backend
                && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
        }

        static bool dump_shader(
            std::FILE *output,
            IDirect3DVertexShader9 *shader,
            std::uint16_t index,
            const char *name,
            bool modern
        ) noexcept {
            if(!output || !name) {
                return false;
            }
            if(!shader) {
                std::fprintf(
                    output,
                    "===== %03u %s %s =====\n<shader unavailable>\n===== END =====\n\n",
                    static_cast<unsigned>(index),
                    name,
                    modern ? "MODERN" : "STOCK"
                );
                return false;
            }

            UINT byte_count = 0;
            HRESULT result = shader->GetFunction(nullptr, &byte_count);
            if(FAILED(result) || byte_count == 0) {
                std::fprintf(
                    output,
                    "===== %03u %s %s =====\nGetFunction(size) failed hr=0x%08lX size=%u\n===== END =====\n\n",
                    static_cast<unsigned>(index),
                    name,
                    modern ? "MODERN" : "STOCK",
                    static_cast<unsigned long>(result),
                    byte_count
                );
                return false;
            }

            std::vector<unsigned char> bytecode(byte_count);
            result = shader->GetFunction(bytecode.data(), &byte_count);
            if(FAILED(result)) {
                std::fprintf(
                    output,
                    "===== %03u %s %s =====\nGetFunction(data) failed hr=0x%08lX\n===== END =====\n\n",
                    static_cast<unsigned>(index),
                    name,
                    modern ? "MODERN" : "STOCK",
                    static_cast<unsigned long>(result)
                );
                return false;
            }

            ID3DBlob *disassembly = nullptr;
            result = D3DDisassemble(
                bytecode.data(),
                byte_count,
                0,
                name,
                &disassembly
            );
            if(FAILED(result) || !disassembly) {
                std::fprintf(
                    output,
                    "===== %03u %s %s =====\nD3DDisassemble failed hr=0x%08lX bytes=%u\n===== END =====\n\n",
                    static_cast<unsigned>(index),
                    name,
                    modern ? "MODERN" : "STOCK",
                    static_cast<unsigned long>(result),
                    byte_count
                );
                if(disassembly) disassembly->Release();
                return false;
            }

            std::fprintf(
                output,
                "===== %03u %s %s bytes=%u =====\n",
                static_cast<unsigned>(index),
                name,
                modern ? "MODERN" : "STOCK",
                byte_count
            );
            std::fwrite(disassembly->GetBufferPointer(), 1, disassembly->GetBufferSize(), output);
            std::fprintf(output, "\n===== END %03u %s %s =====\n\n",
                static_cast<unsigned>(index),
                name,
                modern ? "MODERN" : "STOCK");
            disassembly->Release();
            return true;
        }

        static bool dump_all_vertex_shaders() noexcept {
            if(!d3d9on12_requested()) {
                console_error("D3D9 modern bank: dump_all requires video_mode.d3d_backend=9on12.");
                return false;
            }
            if(!vertex_shaders) {
                console_error("D3D9 modern bank: Halo vertex shader table is unavailable.");
                return false;
            }

            std::FILE *output = std::fopen("chimera_d3d9_all_vertex_shaders.asm.log", "w");
            if(!output) {
                console_error("D3D9 modern bank: could not create chimera_d3d9_all_vertex_shaders.asm.log.");
                return false;
            }

            std::fprintf(output, "# Halo CE D3D9 vertex shader inventory for D3D9On12 modernization.\n");
            std::fprintf(output, "# Run with live compatibility replacements disabled when an exact stock dump is required.\n");
            std::fprintf(output, "# NUM_OF_VERTEX_SHADERS=%u existing_modern_bank=%lu\n\n",
                static_cast<unsigned>(NUM_OF_VERTEX_SHADERS),
                static_cast<unsigned long>(rasterizer_modern_vertex_shader_count()));

            unsigned stock_ok = 0;
            unsigned modern_ok = 0;
            for(std::uint16_t i = 0; i < NUM_OF_VERTEX_SHADERS; i++) {
                const char *name = vertex_shader_names[i];
                if(dump_shader(output, vertex_shaders[i].shader, i, name, false)) {
                    stock_ok++;
                }

                IDirect3DVertexShader9 *modern = rasterizer_get_modern_vertex_shader(i);
                if(modern) {
                    if(dump_shader(output, modern, i, name, true)) {
                        modern_ok++;
                    }
                }
                else {
                    std::fprintf(
                        output,
                        "===== %03u %s MODERN =====\n<not converted yet>\n===== END =====\n\n",
                        static_cast<unsigned>(i),
                        name
                    );
                }
            }

            std::fprintf(output, "# SUMMARY stock_dumped=%u/%u modern_dumped=%u/%u\n",
                stock_ok,
                static_cast<unsigned>(NUM_OF_VERTEX_SHADERS),
                modern_ok,
                static_cast<unsigned>(NUM_OF_VERTEX_SHADERS));
            std::fclose(output);

            console_output(
                "D3D9 modern bank: dumped %u/%u stock VS and %u modern VS -> chimera_d3d9_all_vertex_shaders.asm.log",
                stock_ok,
                static_cast<unsigned>(NUM_OF_VERTEX_SHADERS),
                modern_ok
            );
            return stock_ok > 0;
        }

        static void print_status() noexcept {
            const std::size_t modern_count = rasterizer_modern_vertex_shader_count();
            console_output(
                "D3D9 modern vertex bank: %lu/%u VS3 slots currently populated; %lu stock fallbacks remain.",
                static_cast<unsigned long>(modern_count),
                static_cast<unsigned>(NUM_OF_VERTEX_SHADERS),
                static_cast<unsigned long>(NUM_OF_VERTEX_SHADERS - modern_count)
            );
            if(modern_count > 0) {
                for(std::uint16_t i = 0; i < NUM_OF_VERTEX_SHADERS; i++) {
                    if(rasterizer_has_modern_vertex_shader(i)) {
                        console_output("  modern: %03u %s", static_cast<unsigned>(i), vertex_shader_names[i]);
                    }
                }
            }
        }

        static void print_help() noexcept {
            console_output("chimera_d3d9_modern status");
            console_output("chimera_d3d9_modern dump_all");
        }

        static bool command(int argc, const char **argv) noexcept {
            if(!d3d9on12_requested()) {
                console_error("D3D9 modern bank is only available with video_mode.d3d_backend=9on12.");
                return false;
            }
            if(argc == 0 || _stricmp(argv[0], "status") == 0) {
                print_status();
                return true;
            }
            if(_stricmp(argv[0], "dump_all") == 0 || _stricmp(argv[0], "dump") == 0) {
                return dump_all_vertex_shaders();
            }
            if(_stricmp(argv[0], "help") == 0) {
                print_help();
                return true;
            }

            console_error("D3D9 modern bank: unknown action '%s'.", argv[0]);
            print_help();
            return false;
        }
    }
}

#endif
