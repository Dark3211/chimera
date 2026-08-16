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
    * Install temporary diagnostics for the environment transparent renderer path.
    */
    void set_up_environment_transparent_diagnostic() noexcept;

}

#endif
