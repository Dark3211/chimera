// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_INTERNAL_SHADERS
#define CHIMERA_INTERNAL_SHADERS

namespace Chimera {
    /**
     * True when Custom Edition on D3D9On12 is using Chimera's validated
     * internal hybrid vertex-shader collection instead of the legacy VSH set.
     */
    bool using_internal_d3d9on12_vertex_shader_collection() noexcept;

    /**
     * Replace shaders loaded from games default shader collection files with fixed ones
     * located internally within strings.dll
     */
    void set_up_internal_shaders() noexcept;
}

#endif
