// SPDX-License-Identifier: GPL-3.0-only

#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../event/frame.hpp"
#include "../event/tick.hpp"

namespace Chimera {
    static Signature *auto_center_signature = nullptr;

    // Apply the mod, disabling auto centering.
    static void apply_mod() noexcept {
        overwrite(auto_center_signature->data(), static_cast<std::uint16_t>(0x9090));
    }

    // This is the number of frames that occurred this tick.
    static std::size_t frames = 0;
    static bool auto_center_frame_active = false;

    // Disable auto centering once a second frame occurs.
    static void auto_center_frame() noexcept {
        if(!auto_center_frame_active) {
            return;
        }

        if(++frames == 2) {
            apply_mod();
            auto_center_frame_active = false;
        }
    }

    // Re-enable auto centering, ensuring that the camera movement only occurs once per tick. Set frame counter to 0.
    static void auto_center_tick() noexcept {
        auto_center_signature->rollback();
        frames = 0;
        auto_center_frame_active = true;
    }

    void set_up_auto_center_fix(bool disabled) noexcept {
        if(!auto_center_signature) {
            auto_center_signature = &get_chimera().get_signature("auto_center_sig");
        }

        if(disabled) {
            auto_center_frame_active = false;
            apply_mod();
            remove_pretick_event(auto_center_tick);
            remove_frame_event(auto_center_frame);
        }
        else {
            add_pretick_event(auto_center_tick);
            add_frame_event(auto_center_frame);
        }
    }
}
