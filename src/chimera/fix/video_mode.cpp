// SPDX-License-Identifier: GPL-3.0-only

#include "video_mode.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../halo_data/game_variables.hpp"
#include "../output/output.hpp"
#include "../signature/signature.hpp"
#include "../signature/hook.hpp"
#include "../event/frame.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstring>
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

    static std::uint32_t requested_graphics_msaa_samples() noexcept {
        const auto *ini = get_chimera().get_ini();
        if(!ini || !ini->get_value_bool("graphics.enabled").value_or(false)) {
            return 0;
        }

        const char *anti_aliasing = ini->get_value("graphics.anti_aliasing");
        if(!anti_aliasing) {
            return 0;
        }

        if(std::strcmp(anti_aliasing, "msaa8") == 0 ||
           std::strcmp(anti_aliasing, "MSAA8") == 0 ||
           std::strcmp(anti_aliasing, "msaa8x") == 0 ||
           std::strcmp(anti_aliasing, "MSAA8X") == 0) {
            return 8;
        }

        if(std::strcmp(anti_aliasing, "msaa4") == 0 ||
           std::strcmp(anti_aliasing, "MSAA4") == 0 ||
           std::strcmp(anti_aliasing, "msaa4x") == 0 ||
           std::strcmp(anti_aliasing, "MSAA4X") == 0 ||
           std::strcmp(anti_aliasing, "msaa") == 0 ||
           std::strcmp(anti_aliasing, "MSAA") == 0) {
            return 4;
        }

        if(std::strcmp(anti_aliasing, "msaa2") == 0 ||
           std::strcmp(anti_aliasing, "MSAA2") == 0 ||
           std::strcmp(anti_aliasing, "msaa2x") == 0 ||
           std::strcmp(anti_aliasing, "MSAA2X") == 0) {
            return 2;
        }

        return 0;
    }

    static D3DMULTISAMPLE_TYPE d3d9_multisample_type(std::uint32_t samples) noexcept {
        switch(samples) {
            case 2:
                return D3DMULTISAMPLE_2_SAMPLES;
            case 4:
                return D3DMULTISAMPLE_4_SAMPLES;
            case 8:
                return D3DMULTISAMPLE_8_SAMPLES;
            default:
                return D3DMULTISAMPLE_NONE;
        }
    }

    static std::uint32_t choose_supported_msaa_samples(
        const D3DPRESENT_PARAMETERS &params,
        std::uint32_t requested_samples
    ) noexcept {
        using Direct3DCreate9Proc = IDirect3D9 *(WINAPI *)(UINT);

        HMODULE d3d9_module = GetModuleHandleA("d3d9.dll");
        if(!d3d9_module) {
            return 0;
        }

        FARPROC create_d3d9_address = GetProcAddress(d3d9_module, "Direct3DCreate9");
        if(!create_d3d9_address) {
            return 0;
        }

        Direct3DCreate9Proc create_d3d9 = nullptr;
        static_assert(sizeof(create_d3d9) == sizeof(create_d3d9_address),
                      "Unexpected Direct3DCreate9 function-pointer size");
        std::memcpy(&create_d3d9, &create_d3d9_address, sizeof(create_d3d9));

        IDirect3D9 *d3d9 = create_d3d9(D3D_SDK_VERSION);
        if(!d3d9) {
            return 0;
        }

        D3DFORMAT back_buffer_format = params.BackBufferFormat;
        if(back_buffer_format == D3DFMT_UNKNOWN) {
            D3DDISPLAYMODE display_mode {};
            if(SUCCEEDED(IDirect3D9_GetAdapterDisplayMode(d3d9, D3DADAPTER_DEFAULT, &display_mode))) {
                back_buffer_format = display_mode.Format;
            }
        }

        auto supports_samples = [&](std::uint32_t samples) noexcept {
            if(back_buffer_format == D3DFMT_UNKNOWN) {
                return false;
            }

            const auto type = d3d9_multisample_type(samples);
            DWORD color_quality_levels = 0;
            if(FAILED(IDirect3D9_CheckDeviceMultiSampleType(
                d3d9,
                D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL,
                back_buffer_format,
                params.Windowed,
                type,
                &color_quality_levels
            )) || color_quality_levels == 0) {
                return false;
            }

            if(params.EnableAutoDepthStencil && params.AutoDepthStencilFormat != D3DFMT_UNKNOWN) {
                DWORD depth_quality_levels = 0;
                if(FAILED(IDirect3D9_CheckDeviceMultiSampleType(
                    d3d9,
                    D3DADAPTER_DEFAULT,
                    D3DDEVTYPE_HAL,
                    params.AutoDepthStencilFormat,
                    params.Windowed,
                    type,
                    &depth_quality_levels
                )) || depth_quality_levels == 0) {
                    return false;
                }
            }

            return true;
        };

        std::uint32_t selected_samples = 0;
        if(supports_samples(requested_samples)) {
            selected_samples = requested_samples;
        }
        else if(requested_samples >= 8 && supports_samples(4)) {
            selected_samples = 4;
        }
        else if(requested_samples >= 4 && supports_samples(2)) {
            selected_samples = 2;
        }

        IDirect3D9_Release(d3d9);
        return selected_samples;
    }

    static void apply_graphics_msaa(D3DPRESENT_PARAMETERS *params) noexcept {
        if(!params) {
            return;
        }

        const std::uint32_t requested_samples = requested_graphics_msaa_samples();
        if(requested_samples == 0) {
            return;
        }

        // A lockable swap-chain backbuffer and D3D9 multisampling are mutually
        // incompatible. Do not silently remove the lockable flag because another
        // renderer component may depend on it.
        std::uint32_t selected_samples = 0;
        if((params->Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) == 0) {
            selected_samples = choose_supported_msaa_samples(*params, requested_samples);
        }

        params->MultiSampleQuality = 0;
        if(selected_samples > 0) {
            // D3D9 requires DISCARD swap effect for multisampled swap chains.
            params->SwapEffect = D3DSWAPEFFECT_DISCARD;
            params->MultiSampleType = d3d9_multisample_type(selected_samples);
        }
        else {
            params->MultiSampleType = D3DMULTISAMPLE_NONE;
        }

        static std::uint32_t last_reported_request = std::numeric_limits<std::uint32_t>::max();
        static std::uint32_t last_reported_selection = std::numeric_limits<std::uint32_t>::max();
        if(last_reported_request == requested_samples && last_reported_selection == selected_samples) {
            return;
        }
        last_reported_request = requested_samples;
        last_reported_selection = selected_samples;

        if(selected_samples == requested_samples) {
            console_output("Chimera Graphics: native D3D9 MSAA %ux enabled.", selected_samples);
        }
        else if(selected_samples > 0) {
            console_warning(
                "Chimera Graphics: MSAA %ux is unsupported for this mode; using native MSAA %ux instead.",
                requested_samples,
                selected_samples
            );
        }
        else if((params->Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) != 0) {
            console_warning(
                "Chimera Graphics: native MSAA was not enabled because Halo requested a lockable backbuffer."
            );
        }
        else {
            console_warning(
                "Chimera Graphics: native MSAA %ux is unsupported for this D3D9 mode; no FXAA fallback was enabled.",
                requested_samples
            );
        }
    }

    extern "C" {
        void on_windowed_check_force_windowed() noexcept;
        void on_set_present_params_asm() noexcept;
        std::uint32_t force_windowed_mode = 0;
    }

    static void install_present_parameter_hooks() noexcept {
        static Hook present_hook_1, present_hook_2;
        static bool installed = false;
        if(installed) {
            return;
        }

        auto &chimera = get_chimera();
        auto *present_sig_1 = chimera.get_signature("presentation_interval_1_sig").data() + 2;
        auto *present_sig_2 = chimera.get_signature("presentation_interval_2_sig").data();

        write_jmp_call(present_sig_1, present_hook_1, nullptr, reinterpret_cast<const void *>(on_set_present_params_asm));
        write_jmp_call(present_sig_2, present_hook_2, nullptr, reinterpret_cast<const void *>(on_set_present_params_asm));
        installed = true;
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
        if(ini->get_value_bool("video_mode.enabled").value_or(false)) {
            std::uint32_t refresh_rate = 0;
            parse_uint32_setting(ini->get_value("video_mode.refresh_rate"), refresh_rate);
            params->FullScreen_RefreshRateInHz = params->Windowed ? 0 : refresh_rate;
            params->PresentationInterval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

            if(params->Windowed) {
                add_preframe_event(set_borderless_window);
            }
        }

        // Apply true multisampling at the swap-chain/device level. This is intentionally
        // separate from Chimera's post-process shader: selecting msaa2/msaa4/msaa8 never
        // enables the old FXAA path.
        apply_graphics_msaa(params);
    }

    void set_up_video_mode() noexcept {
        auto &chimera = get_chimera();
        bool pc = chimera.feature_present("client_resolution_pc");
        bool demo = chimera.feature_present("client_resolution_demo");
        auto *ini = chimera.get_ini();

        // The presentation-parameter signatures are shared by all supported client
        // executables, including Custom Edition. Install these hooks for MSAA even when
        // Chimera's resolution override is unavailable or disabled.
        if(requested_graphics_msaa_samples() > 0) {
            install_present_parameter_hooks();
        }

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
        static Hook window_hook;
        auto *windowed_sig = chimera.get_signature("windowed_sig").data();
        write_jmp_call(windowed_sig, window_hook, reinterpret_cast<const void *>(on_windowed_check_force_windowed), nullptr, false);

        // The same present-parameter hook handles refresh/vsync and optional MSAA.
        install_present_parameter_hooks();
        force_windowed_mode = ini->get_value_bool("video_mode.windowed").value_or(false);
    }
}
