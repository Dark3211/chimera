// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_INTERPOLATE_PARTICLE_HPP
#define CHIMERA_INTERPOLATE_PARTICLE_HPP

namespace Chimera {
    /**
     * Register the temporary particle interpolation diagnostic command.
     */
    void set_up_particle_interpolation_diagnostic() noexcept;

    /**
     * Apply a pending diagnostic state change after the current frame has restored
     * any temporarily interpolated particle positions.
     */
    void apply_particle_interpolation_state_change() noexcept;

    /**
     * Interpolate particles.
     */
    void interpolate_particle() noexcept;

    /**
     * Uninterpolate particles.
     */
    void interpolate_particle_after() noexcept;

    /**
     * Clear the buffers. This should be done if changing the interpolation setting.
     */
    void interpolate_particle_clear() noexcept;

    /**
     * Set the tick flag, swapping buffers for the next tick.
     */
    void interpolate_particle_on_tick() noexcept;
}

#endif
