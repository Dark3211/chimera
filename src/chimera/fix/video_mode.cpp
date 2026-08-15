// SPDX-License-Identifier: GPL-3.0-only

#include "video_mode.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../halo_data/game_variables.hpp"
#include "../signature/signature.hpp"
#include "../signature/hook.hpp"
#include "../event/frame.hpp"

#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <limits>

namespace Chimera {
    static bool vsync = false;

    static bool parse_uint32_setting(const char *value, std::uint32_t &result) noexcept {
        if(!value || !*value || *value == '-') {
            return false;
        }
        errno = 0;
        char *end = nullptr;
        auto parsed = std::strtoull(value, &end, 10);
        if(errno == ERANGE || end == value || !end || *end != 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        result = static_cast<std::uint32_t>(parsed);
        return true;
    }

    extern "C" {
        void on_windowed_check_force_windowed() noexcept;
        void on_set_present_params_asm() noexcept;
        std::uint32_t force_windowed_mode = 0;
    }

    static void set_borderless_window() noexcept {
        HWND window;
        HMONITOR monitor;
        MONITORINFO monitor_info;

        if(get_chimera().get_ini()->get_value_bool("video_mode.borderless").value_or(false)) {
            // Get our window
            window = GetActiveWindow();
            if(!window) {
                goto cleanup;
            }

            // If the window isn't the foreground window, wait until it is before doing the thing.
            if(window != GetForegroundWindow()) {
                return;
            }

            // Query monitor information
            monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
            monitor_info.cbSize = sizeof(monitor_info);
            if(!GetMonitorInfo(monitor, &monitor_info)) {
                goto cleanup;
            }

            // Work our magic!
            ShowWindow(window, SW_HIDE);
            SetWindowLong(window, GWL_STYLE, 0);
            SetWindowPos(window, NULL, 0, 0, monitor_info.rcMonitor.right, monitor_info.rcMonitor.bottom, 0);
        }

        cleanup:
        remove_preframe_event(set_borderless_window);
    }

    extern "C" void now_set_borderless_windowed_mode() noexcept {
        add_preframe_event(set_borderless_window);
    }

    extern "C" void override_d3d_present_parameters(D3DPRESENT_PARAMETERS *params) noexcept {
        if(!params) {
            return;
        }
        auto *ini = get_chimera().get_ini();
        std::uint32_t refresh_rate = 0;
        parse_uint32_setting(ini->get_value("video_mode.refresh_rate"), refresh_rate);
        params->FullScreen_RefreshRateInHz = params->Windowed ? 0 : refresh_rate;
        params->PresentationInterval = vsync ? D3DPRESENT_DONOTWAIT : 0x80000000;

        if(params->Windowed) {
            add_preframe_event(set_borderless_window);
        }
    }

    void set_up_video_mode() noexcept {
        auto &chimera = get_chimera();
        bool pc = chimera.feature_present("client_resolution_pc");
        bool demo = chimera.feature_present("client_resolution_demo");

        if(!pc && !demo) {
            return;
        }

        // This is from HAC2 so the in-game resolution picker doesn't limit values.
        SigByte bypass_res_limit[] = { 0xEB, 0x54 };
        SigByte bypass_fullscreen_width_limit[] = { 0x77, 0x06 };
        SigByte bypass_fullscreen_height_limit[] = { 0x76, 0x25 };
        write_code_s(chimera.get_signature("resolution_limit_sig").data(), bypass_res_limit);
        write_code_s(chimera.get_signature("resolution_width_limit_sig").data(), bypass_fullscreen_width_limit);
        write_code_s(chimera.get_signature("resolution_height_limit_sig").data(), bypass_fullscreen_height_limit);

        std::uint32_t default_width = 800;
        std::uint32_t default_height = 600;
        std::uint32_t default_refresh_rate = 60;

        auto *ini = chimera.get_ini();
        if(!ini->get_value_bool("video_mode.enabled").value_or(false)) {
            return;
        }

        auto read_dimension = [](const char *value, int metric, std::uint32_t &destination) noexcept {
            if(!value) {
                return;
            }
            if(std::strcmp(value, "auto") == 0 || std::strcmp(value, "0") == 0) {
                auto system_value = GetSystemMetrics(metric);
                if(system_value > 0) {
                    destination = static_cast<std::uint32_t>(system_value);
                }
                return;
            }
            std::uint32_t parsed = 0;
            if(parse_uint32_setting(value, parsed) && parsed > 0) {
                destination = parsed;
            }
        };

        read_dimension(ini->get_value("video_mode.width"), SM_CXSCREEN, default_width);
        read_dimension(ini->get_value("video_mode.height"), SM_CYSCREEN, default_height);

        std::uint32_t parsed_refresh_rate = 0;
        if(parse_uint32_setting(ini->get_value("video_mode.refresh_rate"), parsed_refresh_rate)) {
            default_refresh_rate = parsed_refresh_rate;
        }
        vsync = ini->get_value_bool("video_mode.vsync").value_or(vsync);

        // Don't fallback the resolution to 800x600
        auto fallback_resolution_sig = chimera.get_signature("fallback_resolution_sig").data();
        static const constexpr SigByte remove_fallback_resolution[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        write_code_s(fallback_resolution_sig, remove_fallback_resolution);

        if(pc) {
            auto *default_res = chimera.get_signature("default_resolution_pc_sig").data();
            overwrite(default_res + 4, default_width);
            overwrite(default_res + 12, default_height);
            overwrite(default_res + 20, default_refresh_rate);
        }
        else if(demo) {
            auto *default_res = chimera.get_signature("default_resolution_demo_sig").data();
            overwrite(default_res + 1, default_width);
            overwrite(default_res + 28, default_height);
            overwrite(default_res + 6, default_refresh_rate);

            // Prevent the LAA patch from overruling this
            auto *default_res_override = chimera.get_signature("default_resolution_override_demo_sig").data();
            overwrite(default_res_override + 10, static_cast<std::uint8_t>(0xEB));
        }

        // Disable Halo's loading of the profile data
        overwrite(chimera.get_signature("load_profile_resolution_sig").data(), static_cast<std::uint8_t>(0xEB));

        // Also, windowed mode
        static Hook window_hook, present_hook_1, present_hook_2;
        auto *windowed_sig = chimera.get_signature("windowed_sig").data();
        auto *present_sig_1 = chimera.get_signature("presentation_interval_1_sig").data() + 2;
        auto *present_sig_2 = chimera.get_signature("presentation_interval_2_sig").data();

        write_jmp_call(windowed_sig, window_hook, reinterpret_cast<const void *>(on_windowed_check_force_windowed), nullptr, false);
        write_jmp_call(present_sig_1, present_hook_1, nullptr, reinterpret_cast<const void *>(on_set_present_params_asm));
        write_jmp_call(present_sig_2, present_hook_2, nullptr, reinterpret_cast<const void *>(on_set_present_params_asm));
        force_windowed_mode = ini->get_value_bool("video_mode.windowed").value_or(false);
    }
}
