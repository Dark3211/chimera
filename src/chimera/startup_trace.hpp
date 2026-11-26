// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_STARTUP_TRACE_HPP
#define CHIMERA_STARTUP_TRACE_HPP

#include <d3d9.h>

namespace Chimera {
    class Ini;

    /** Start the opt-in startup trace and install D3D9/display-mode diagnostics. */
    void startup_trace_initialize(const Ini *ini) noexcept;

    /** Write a timestamped startup diagnostic line when tracing is enabled. */
    void startup_trace_log(const char *format, ...) noexcept;
    void startup_trace_log_window(const char *label, void *window) noexcept;

    /** Called immediately before Halo's native Reset call; captures its first result and future Present calls. */
    void startup_trace_on_reset_call(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *params) noexcept;
}

#endif
