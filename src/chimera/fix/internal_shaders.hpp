// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_INTERNAL_SHADERS
#define CHIMERA_INTERNAL_SHADERS

namespace Chimera {
    /**
     * True when Custom Edition is using the validated internal D3D9On12
     * vertex-shader collection selected by video_mode.d3d_backend=9on12.
     */
    bool using_internal_d3d9on12_vertex_shader_collection() noexcept;

    /**
     * Replace shaders loaded from games default shader collection files with fixed ones
     * located internally within strings.dll
     */
    void set_up_internal_shaders() noexcept;
}

#endif
