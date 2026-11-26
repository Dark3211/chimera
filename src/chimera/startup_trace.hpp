// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_STARTUP_TRACE_HPP
#define CHIMERA_STARTUP_TRACE_HPP

namespace Chimera {
    class Ini;

    /** Start the opt-in startup trace and install D3D9/display-mode diagnostics. */
    void startup_trace_initialize(const Ini *ini) noexcept;

    /** Write a timestamped startup diagnostic line when tracing is enabled. */
    void startup_trace_log(const char *format, ...) noexcept;
    void startup_trace_log_window(const char *label, void *window) noexcept;
}

#endif
