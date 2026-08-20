// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_BACKEND_HPP
#define CHIMERA_D3D9_BACKEND_HPP

namespace Chimera {
    /**
     * Install the optional Direct3D 9 on 12 device creation hook.
     *
     * The hook is only installed when video_mode.d3d_backend is set to
     * "9on12". If setup fails, Halo keeps using its original D3D9 entrypoint.
     */
    void set_up_d3d9_backend() noexcept;

    /**
     * Report whether the requested backend activated or fell back to D3D9.
     * This should be called after Chimera console output has been enabled.
     */
    void report_d3d9_backend_status() noexcept;
}

#endif
