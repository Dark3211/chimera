// SPDX-License-Identifier: GPL-3.0-only

#include <cctype>
#include <cstring>

#include "../../halo_data/particle.hpp"
#include "../../event/command.hpp"
#include "../../output/output.hpp"

#include "particle.hpp"

namespace Chimera {
    struct InterpolatedParticle {
        bool interpolate;
        Point3D position;
    };

    #define PARTICLE_BUFFER_SIZE 1024
    static InterpolatedParticle particle_buffers[2][PARTICLE_BUFFER_SIZE] = {};

    // These are pointers to each buffer. These swap every tick.
    static auto *current_tick = particle_buffers[0];
    static auto *previous_tick = particle_buffers[1];

    // If true, a tick has passed and it's time to re-copy the particle data.
    static bool tick_passed = false;

    // Temporary diagnostic state. Particle interpolation remains enabled by default.
    static bool particle_interpolation_enabled = true;
    static bool particle_interpolation_requested_enabled = true;
    static bool particle_interpolation_transition_pending = false;
    static bool particle_interpolation_command_registered = false;

    static bool particle_interpolation_console_command(const char *command) noexcept {
        if(!command) {
            return true;
        }

        static constexpr char command_name[] = "chimera_debug_particle_interpolation";
        static constexpr std::size_t command_name_length = sizeof(command_name) - 1;

        if(std::strncmp(command, command_name, command_name_length) != 0) {
            return true;
        }

        const char *argument = command + command_name_length;
        if(*argument != '\0' && !std::isspace(static_cast<unsigned char>(*argument))) {
            return true;
        }

        while(std::isspace(static_cast<unsigned char>(*argument))) {
            argument++;
        }

        if(*argument == '\0') {
            const bool displayed_state = particle_interpolation_transition_pending
                ? particle_interpolation_requested_enabled
                : particle_interpolation_enabled;
            console_output("chimera_debug_particle_interpolation: %s", displayed_state ? "true" : "false");
            return false;
        }

        const char *argument_end = argument;
        while(*argument_end != '\0' && !std::isspace(static_cast<unsigned char>(*argument_end))) {
            argument_end++;
        }

        const std::size_t argument_length = static_cast<std::size_t>(argument_end - argument);
        while(std::isspace(static_cast<unsigned char>(*argument_end))) {
            argument_end++;
        }

        if(*argument_end != '\0') {
            console_error("chimera_debug_particle_interpolation: expected true or false");
            return false;
        }

        bool new_enabled;
        if((argument_length == 4 && std::strncmp(argument, "true", 4) == 0) ||
           (argument_length == 1 && argument[0] == '1')) {
            new_enabled = true;
        }
        else if((argument_length == 5 && std::strncmp(argument, "false", 5) == 0) ||
                (argument_length == 1 && argument[0] == '0')) {
            new_enabled = false;
        }
        else {
            console_error("chimera_debug_particle_interpolation: expected true or false");
            return false;
        }

        particle_interpolation_requested_enabled = new_enabled;
        particle_interpolation_transition_pending = new_enabled != particle_interpolation_enabled;

        console_output("chimera_debug_particle_interpolation: %s", new_enabled ? "true" : "false");
        return false;
    }

    void set_up_particle_interpolation_diagnostic() noexcept {
        if(!particle_interpolation_command_registered) {
            add_command_event(particle_interpolation_console_command, EVENT_PRIORITY_BEFORE);
            particle_interpolation_command_registered = true;
        }
    }

    void apply_particle_interpolation_state_change() noexcept {
        if(!particle_interpolation_transition_pending) {
            return;
        }

        particle_interpolation_transition_pending = false;
        if(particle_interpolation_requested_enabled == particle_interpolation_enabled) {
            return;
        }

        // This is called after interpolate_particle_after(), so no frame can be left
        // with temporary interpolated positions when the diagnostic mode changes.
        particle_interpolation_enabled = particle_interpolation_requested_enabled;
        interpolate_particle_clear();
    }

    void interpolate_particle() noexcept {
        if(!particle_interpolation_enabled) {
            return;
        }

        auto &particle_table = ParticleTable::get_particle_table();
        if(tick_passed) {
            // Swap buffers.
            if(current_tick == particle_buffers[0]) {
                current_tick = particle_buffers[1];
                previous_tick = particle_buffers[0];
            }
            else {
                current_tick = particle_buffers[0];
                previous_tick = particle_buffers[1];
            }

            // Go through each particle, determining if any can be interpolated.
            for(std::size_t i = 0; i < PARTICLE_BUFFER_SIZE; i++) {
                auto *particle = particle_table.get_element(i);
                auto &current_tick_particle = current_tick[i];
                current_tick_particle.interpolate = false;

                if(!particle) {
                    continue;
                }

                // Copy the original particle data.
                current_tick_particle.position = particle->position;

                // I'm not entirely sure what unknown0 does, but it magically determines if I should interpolate the particle.
                current_tick_particle.interpolate = particle->unknown0 & 0xFFFF;
            }

            tick_passed = false;
        }

        // Iterate through each particle.
        for(std::size_t i = 0; i < particle_table.current_size && i < PARTICLE_BUFFER_SIZE; i++) {
            auto *particle = particle_table.get_element(i);
            if(!particle) {
                continue;
            }

            auto &current_tick_particle = current_tick[i];
            auto &previous_tick_particle = previous_tick[i];
            extern float interpolation_tick_progress;

            // Interpolate each particle that can be interpolated.
            if(current_tick_particle.interpolate && previous_tick_particle.interpolate) {
                interpolate_point(previous_tick_particle.position, current_tick_particle.position, particle->position, interpolation_tick_progress);
            }
        }
    }

    void interpolate_particle_after() noexcept {
        if(!particle_interpolation_enabled) {
            return;
        }

        auto &particle_table = ParticleTable::get_particle_table();
        for(std::size_t i = 0; i < particle_table.current_size && i < PARTICLE_BUFFER_SIZE; i++) {
            auto *particle = particle_table.get_element(i);
            if(!particle) {
                continue;
            }

            auto &current_tick_particle = current_tick[i];
            auto &previous_tick_particle = previous_tick[i];

            // Restore each position.
            if(current_tick_particle.interpolate && previous_tick_particle.interpolate) {
                particle->position = current_tick_particle.position;
            }
        }
    }

    void interpolate_particle_clear() noexcept {
        std::memset(particle_buffers, 0, sizeof(particle_buffers));
        current_tick = particle_buffers[0];
        previous_tick = particle_buffers[1];
        tick_passed = false;
    }

    void interpolate_particle_on_tick() noexcept {
        if(particle_interpolation_enabled) {
            tick_passed = true;
        }
    }
}
