// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_VERTEX_SHADERS_HPP
#define CHIMERA_VERTEX_SHADERS_HPP

#include "rasterizer.hpp"
#include "../halo_data/shader_defs.hpp"

namespace Chimera {

    extern DynamicVertex screen_vertices[4];

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
