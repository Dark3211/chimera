// SPDX-License-Identifier: GPL-3.0-only

#include <cctype>
#include <cstring>

#include "../../chimera.hpp"
#include "../../signature/hook.hpp"
#include "../../signature/signature.hpp"
#include "../../halo_data/multiplayer.hpp"
#include "../../halo_data/pause.hpp"
#include "../../event/camera.hpp"
#include "../../event/command.hpp"
#include "../../event/frame.hpp"
#include "../../event/tick.hpp"
#include "../../event/revert.hpp"
#include "../../halo_data/game_engine.hpp"
#include "../../output/output.hpp"

#include "antenna.hpp"
#include "camera.hpp"
#include "flag.hpp"
#include "fp.hpp"
#include "light.hpp"
#include "object.hpp"
#include "particle.hpp"

#include "interpolate.hpp"

namespace Chimera {
    // This is the progress since the last tick (updated every frame).
    float interpolation_tick_progress = 0;

    // This is the assumed tick rate of the first person camera.
    static float *first_person_camera_tick_rate = nullptr;

    // Set for if interpolation is enabled
    bool interpolation_enabled = false;

    static bool interpolation_command_registered = false;
    static bool interpolation_transition_registered = false;
    static bool interpolation_requested_enabled = true;
    static bool interpolation_transition_pending = false;

    static bool interpolation_console_command(const char *command) noexcept {
        if(!command) {
            return true;
        }

        static constexpr char command_name[] = "chimera_interpolate";
        static constexpr std::size_t command_name_length = sizeof(command_name) - 1;

        if(std::strncmp(command, command_name, command_name_length) != 0) {
            return true;
        }

        const char *argument = command + command_name_length;

        // Do not intercept commands that merely start with our command name.
        if(*argument != '\0' && !std::isspace(static_cast<unsigned char>(*argument))) {
            return true;
        }

        while(std::isspace(static_cast<unsigned char>(*argument))) {
            argument++;
        }

        if(*argument == '\0') {
            const bool displayed_state = interpolation_transition_pending ? interpolation_requested_enabled : interpolation_enabled;
            console_output("chimera_interpolate: %s", BOOL_TO_STR(displayed_state));
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
            console_error("chimera_interpolate: expected true or false");
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
            console_error("chimera_interpolate: expected true or false");
            return false;
        }

        interpolation_requested_enabled = new_enabled;
        interpolation_transition_pending = new_enabled != interpolation_enabled;

        console_output("chimera_interpolate: %s", BOOL_TO_STR(new_enabled));
        return false;
    }

    static void on_tick() noexcept {
        // Prevent interpolation when the game is paused
        if(game_paused()) {
            return;
        }

        interpolate_antenna_on_tick();
        interpolate_flag_on_tick();
        interpolate_fp_on_tick();
        interpolate_light_on_tick();
        interpolate_object_on_tick();
        interpolate_camera_on_tick();
        interpolate_particle_on_tick();
        interpolation_tick_progress = 0;
        float current_tick_rate = effective_tick_rate();
        if(*first_person_camera_tick_rate != current_tick_rate) {
            overwrite(first_person_camera_tick_rate, current_tick_rate);
        }
    }

    static void on_preframe() noexcept {
        if(game_paused()) {
            return;
        }

        interpolation_tick_progress = get_tick_progress();

        interpolate_antenna_before();
        interpolate_flag_before();
        interpolate_light_before();
        interpolate_object_before();
        interpolate_particle();
    }

    static void on_frame() noexcept {
        if(game_paused()) {
            return;
        }

        interpolate_antenna_after();
        interpolate_object_after();
        interpolate_particle_after();
    }

    void clear_buffers() noexcept {
        interpolate_object_clear();
        interpolate_particle_clear();
        interpolate_light_clear();
        interpolate_flag_clear();
        interpolate_camera_clear();
        interpolate_fp_clear();
    }

    static void apply_interpolation_state_change() noexcept {
        if(!interpolation_transition_pending) {
            return;
        }

        const bool requested_enabled = interpolation_requested_enabled;
        interpolation_transition_pending = false;

        if(requested_enabled == interpolation_enabled) {
            return;
        }

        // Run this from an AFTER frame event so the normal interpolation rollback for
        // the current frame has already completed before any hooks/events are removed.
        if(requested_enabled) {
            set_up_interpolation();
        }
        else {
            disable_interpolation();
        }
    }

    void set_up_interpolation() noexcept {
        static auto *fp_interp_ptr = get_chimera().get_signature("fp_interp_sig").data();
        static Hook fp_interp_hook;
        first_person_camera_tick_rate = *reinterpret_cast<float **>(get_chimera().get_signature("fp_cam_tick_rate_sig").data() + 2);

        if(!interpolation_command_registered) {
            add_command_event(interpolation_console_command, EVENT_PRIORITY_BEFORE);
            interpolation_command_registered = true;
        }

        // Keep this event registered even when interpolation itself is disabled. That
        // lets the diagnostic command re-enable interpolation at a safe frame boundary.
        if(!interpolation_transition_registered) {
            add_frame_event(apply_interpolation_state_change, EVENT_PRIORITY_AFTER);
            interpolation_transition_registered = true;
        }

        add_tick_event(on_tick);
        add_preframe_event(on_preframe);
        add_frame_event(on_frame);
        add_precamera_event(interpolate_camera_before);
        add_camera_event(interpolate_camera_after);
        write_jmp_call(fp_interp_ptr, fp_interp_hook, reinterpret_cast<const void *>(interpolate_fp_before), reinterpret_cast<const void *>(interpolate_fp_after));

        // Block built-in fp camera interpolation. Let Chimera do it instead.
        overwrite(get_chimera().get_signature("camera_interpolation_sig").data() + 0xF, static_cast<unsigned char>(0xEB));

        //Clear interpolation buffers on major game state changes to prevent funny things from happening
        add_revert_event(clear_buffers);
        interpolation_enabled = true;
        interpolation_requested_enabled = true;
    }

    void disable_interpolation() noexcept {
        get_chimera().get_signature("fp_interp_sig").rollback();
        get_chimera().get_signature("camera_interpolation_sig").rollback();
        remove_tick_event(on_tick);
        remove_preframe_event(on_preframe);
        remove_frame_event(on_frame);
        remove_precamera_event(interpolate_camera_before);
        remove_camera_event(interpolate_camera_after);
        remove_revert_event(clear_buffers);
        clear_buffers();
        interpolation_enabled = false;
        interpolation_requested_enabled = false;
    }
}
