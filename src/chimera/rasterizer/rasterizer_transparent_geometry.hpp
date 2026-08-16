// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_TRANSPARENT_GEOMETRY_HPP
#define CHIMERA_TRANSPARENT_GEOMETRY_HPP

#include "rasterizer.hpp"

namespace Chimera {

    /**
    * Get primary vertex type from a transparent geometry group.
    */
    short rasterizer_transparent_geometry_get_primary_vertex_type(TransparentGeometryGroup *group) noexcept;

    /**
    * Register the temporary transparent-geometry guard diagnostics command.
    */
    void set_up_transparent_geometry_guard_diagnostics() noexcept;

    /**
    * Validate a transparent geometry group before Halo submits its draw call.
    * This is exported with C linkage because the x86 trampoline calls it directly.
    */
    extern "C" bool rasterizer_validate_transparent_geometry_group(TransparentGeometryGroup *group) noexcept;

}

#endif
