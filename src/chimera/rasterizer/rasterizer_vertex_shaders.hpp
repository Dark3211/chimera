// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_VERTEX_SHADERS_HPP
#define CHIMERA_VERTEX_SHADERS_HPP

#include <windows.h>
#include <d3dcompiler.h>

#include "rasterizer.hpp"
#include "../halo_data/shader_defs.hpp"

namespace Chimera {

    extern DynamicVertex screen_vertices[4];

    /**
    * MinGW's d3dcompiler headers do not consistently declare D3DAssemble even
    * though the Microsoft compiler DLL exports it. Keep the runtime VS1.1 ->
    * VS3 diagnostic transpiler independent of that header/import-library gap by
    * resolving the export dynamically. The public wrapper intentionally uses
    * the seven-argument form already used by Chimera and supplies a null
    * ID3DInclude to the real eight-argument D3DAssemble export.
    */
    inline HRESULT D3DAssemble(
        LPCVOID source_data,
        SIZE_T source_size,
        LPCSTR source_name,
        const D3D_SHADER_MACRO *defines,
        UINT flags,
        ID3DBlob **shader,
        ID3DBlob **errors
    ) noexcept {
        using D3DAssembleFunction = HRESULT (WINAPI *)(
            LPCVOID,
            SIZE_T,
            LPCSTR,
            const D3D_SHADER_MACRO *,
            ID3DInclude *,
            UINT,
            ID3DBlob **,
            ID3DBlob **
        );

        static D3DAssembleFunction function = []() noexcept -> D3DAssembleFunction {
            const char *compiler_dlls[] = {
                "d3dcompiler_47.dll",
                "d3dcompiler_46.dll",
                "d3dcompiler_43.dll"
            };

            for(const char *dll_name : compiler_dlls) {
                HMODULE module = GetModuleHandleA(dll_name);
                if(!module) {
                    module = LoadLibraryA(dll_name);
                }
                if(!module) {
                    continue;
                }

                auto *address = GetProcAddress(module, "D3DAssemble");
                if(address) {
                    return reinterpret_cast<D3DAssembleFunction>(address);
                }
            }
            return nullptr;
        }();

        if(!function) {
            if(shader) {
                *shader = nullptr;
            }
            if(errors) {
                *errors = nullptr;
            }
            return E_NOTIMPL;
        }

        return function(
            source_data,
            source_size,
            source_name,
            defines,
            nullptr,
            flags,
            shader,
            errors
        );
    }

    /**
    * Functions for getting vertex shaders.
    */
    IDirect3DVertexShader9 *rasterizer_get_vertex_shader(std::uint16_t index) noexcept;
    IDirect3DVertexShader9 *rasterizer_get_vertex_shader_for_permutation(uint16_t vertex_shader_permutation, short vertex_type) noexcept;
    IDirect3DVertexDeclaration9 *rasterizer_get_vertex_declaration(short vertex_type) noexcept;

    /**
    * D3D9On12 modern vertex-shader bank.
    *
    * The bank is indexed by the full Halo VertexShaderIndex enum. Entries that
    * have not been modernized yet remain null and automatically fall back to
    * Halo's stock shader. This lets us migrate the 64 stock vertex-shader paths
    * incrementally without changing working paths or requiring all-or-nothing
    * replacement.
    */
    IDirect3DVertexShader9 *rasterizer_get_modern_vertex_shader(std::uint16_t index) noexcept;
    bool rasterizer_has_modern_vertex_shader(std::uint16_t index) noexcept;
    std::size_t rasterizer_modern_vertex_shader_count() noexcept;

}

#endif
