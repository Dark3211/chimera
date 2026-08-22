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
        static IDirect3DVertexShader9 *stock_generic_m = nullptr;
        static IDirect3DVertexShader9 *compat_generic_m = nullptr;
        static bool generic_m_active = false;

        static bool d3d9on12_requested() noexcept {
            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            return backend
                && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
        }

        static void release_owned_refs() noexcept {
            if(compat_generic_m) {
                compat_generic_m->Release();
                compat_generic_m = nullptr;
            }
            if(stock_generic_m) {
                stock_generic_m->Release();
                stock_generic_m = nullptr;
            }
        }

        static bool restore_generic_m() noexcept {
            if(!generic_m_active) {
                return true;
            }
            if(vertex_shaders && stock_generic_m) {
                auto &slot = vertex_shaders[VSH_TRANSPARENT_GENERIC_M].shader;
                if(slot == compat_generic_m) {
                    slot = stock_generic_m;
                }
                else if(slot != stock_generic_m) {
                    console_output(
                        "D3D9 transparent compat: generic_m shader slot changed externally; leaving it untouched."
                    );
                }
            }
            generic_m_active = false;
            console_output("D3D9 transparent compat: VSH_TRANSPARENT_GENERIC_M restored to stock.");
            return true;
        }

        static bool prepare_generic_m() noexcept {
            if(!d3d9on12_requested() || !vertex_shaders) {
                return false;
            }

            if(stock_generic_m && compat_generic_m) {
                return true;
            }

            IDirect3DVertexShader9 *stock = vertex_shaders[VSH_TRANSPARENT_GENERIC_M].shader;
            if(!stock) {
                console_error("D3D9 transparent compat: stock VSH_TRANSPARENT_GENERIC_M is unavailable.");
                return false;
            }

            // Ask Chimera for the already-existing modern transparent-generic VS.
            // This path creates/returns the VS3 version when the device exposes
            // shader-model 3 support; otherwise it falls back to the stock shader.
            IDirect3DVertexShader9 *candidate = rasterizer_get_vertex_shader(VSH_TRANSPARENT_GENERIC_M);
            if(!candidate || candidate == stock) {
                console_error(
                    "D3D9 transparent compat: no distinct modern GENERIC_M shader is available (VS caps=0x%08lX PS caps=0x%08lX).",
                    d3d9_device_caps ? static_cast<unsigned long>(d3d9_device_caps->VertexShaderVersion) : 0UL,
                    d3d9_device_caps ? static_cast<unsigned long>(d3d9_device_caps->PixelShaderVersion) : 0UL
                );
                return false;
            }

            stock->AddRef();
            candidate->AddRef();
            stock_generic_m = stock;
            compat_generic_m = candidate;
            return true;
        }

        static bool enable_generic_m() noexcept {
            if(generic_m_active) {
                console_output("D3D9 transparent compat: GENERIC_M compatibility is already enabled.");
                return true;
            }
            if(!prepare_generic_m()) {
                return false;
            }

            auto &slot = vertex_shaders[VSH_TRANSPARENT_GENERIC_M].shader;
            if(slot != stock_generic_m) {
                console_error(
                    "D3D9 transparent compat: GENERIC_M slot is not the captured stock shader; refusing unsafe replacement."
                );
                return false;
            }

            slot = compat_generic_m;
            generic_m_active = true;
            console_output(
                "D3D9 transparent compat: VSH_TRANSPARENT_GENERIC_M -> Chimera VS3 compatibility shader."
            );
            return true;
        }

        static bool dump_one_shader(
            std::FILE *output,
            VertexShaderIndex index,
            const char *name
        ) noexcept {
            if(!output || !vertex_shaders || index < 0 || index >= NUM_OF_VERTEX_SHADERS) {
                return false;
            }

            IDirect3DVertexShader9 *shader = vertex_shaders[index].shader;
            if(index == VSH_TRANSPARENT_GENERIC_M && stock_generic_m) {
                shader = stock_generic_m;
            }
            if(!shader) {
                std::fprintf(output, "===== %s =====\n<shader unavailable>\n\n", name);
                return false;
            }

            UINT byte_count = 0;
            HRESULT result = shader->GetFunction(nullptr, &byte_count);
            if(FAILED(result) || byte_count == 0) {
                std::fprintf(
                    output,
                    "===== %s =====\nGetFunction(size) failed hr=0x%08lX size=%u\n\n",
                    name,
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
                    "===== %s =====\nGetFunction(data) failed hr=0x%08lX\n\n",
                    name,
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
                    "===== %s =====\nD3DDisassemble failed hr=0x%08lX bytes=%u\n\n",
                    name,
                    static_cast<unsigned long>(result),
                    byte_count
                );
                if(disassembly) disassembly->Release();
                return false;
            }

            std::fprintf(output, "===== %s =====\n", name);
            std::fwrite(disassembly->GetBufferPointer(), 1, disassembly->GetBufferSize(), output);
            std::fprintf(output, "\n===== END %s =====\n\n", name);
            disassembly->Release();
            return true;
        }

        static bool dump_stock_shaders() noexcept {
            if(!d3d9on12_requested() || !vertex_shaders) {
                console_error("D3D9 transparent compat: stock shaders are not available yet.");
                return false;
            }

            std::FILE *output = std::fopen("chimera_d3d9_transparent_vs.asm.log", "w");
            if(!output) {
                console_error("D3D9 transparent compat: could not create chimera_d3d9_transparent_vs.asm.log.");
                return false;
            }

            const bool generic_ok = dump_one_shader(
                output,
                VSH_TRANSPARENT_GENERIC_M,
                "VSH_TRANSPARENT_GENERIC_M_STOCK"
            );
            const bool plasma_ok = dump_one_shader(
                output,
                VSH_TRANSPARENT_PLASMA_M,
                "VSH_TRANSPARENT_PLASMA_M_STOCK"
            );
            std::fclose(output);

            if(generic_ok && plasma_ok) {
                console_output(
                    "D3D9 transparent compat: stock GENERIC_M/PLASMA_M assembly -> chimera_d3d9_transparent_vs.asm.log"
                );
                return true;
            }
            console_error(
                "D3D9 transparent compat: shader dump was incomplete; inspect chimera_d3d9_transparent_vs.asm.log."
            );
            return false;
        }

        static void print_status() noexcept {
            console_output(
                "D3D9 transparent compat: generic_m=%s plasma_m=pending-exact-rebuild",
                generic_m_active ? "compat" : "stock"
            );
            console_output(
                "D3D9 transparent compat caps: VS=0x%08lX PS=0x%08lX",
                d3d9_device_caps ? static_cast<unsigned long>(d3d9_device_caps->VertexShaderVersion) : 0UL,
                d3d9_device_caps ? static_cast<unsigned long>(d3d9_device_caps->PixelShaderVersion) : 0UL
            );
        }

        static void print_help() noexcept {
            console_output("chimera_d3d9_compat status");
            console_output("chimera_d3d9_compat generic_m on");
            console_output("chimera_d3d9_compat generic_m off");
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
                return dump_stock_shaders();
            }
            if(_stricmp(argv[0], "generic_m") == 0) {
                if(argc < 2) {
                    console_error("D3D9 transparent compat: use generic_m on|off.");
                    return false;
                }
                if(_stricmp(argv[1], "on") == 0 || _stricmp(argv[1], "compat") == 0) {
                    return enable_generic_m();
                }
                if(_stricmp(argv[1], "off") == 0 || _stricmp(argv[1], "stock") == 0) {
                    return restore_generic_m();
                }
                console_error("D3D9 transparent compat: use generic_m on|off.");
                return false;
            }

            console_error("D3D9 transparent compat: unknown action '%s'.", argv[0]);
            print_help();
            return false;
        }

        static void on_game_exit() noexcept {
            restore_generic_m();
            release_owned_refs();
        }
    }
}

#endif
