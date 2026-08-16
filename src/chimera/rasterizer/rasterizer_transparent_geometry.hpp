// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_TRANSPARENT_GEOMETRY_HPP
#define CHIMERA_TRANSPARENT_GEOMETRY_HPP

#include "rasterizer.hpp"

namespace Chimera {

    /**
    * Register the temporary transparent-generic diagnostic command.
    */
    void set_up_transparent_generic_diagnostic() noexcept;

    /**
    * Get primary vertex type from a transparent geometry group.
    */
    short rasterizer_transparent_geometry_get_primary_vertex_type(TransparentGeometryGroup *group) noexcept;

}

#endif
