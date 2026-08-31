// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_RASTERIZER_GRAPHICS_DIAGNOSTICS_HPP
#define CHIMERA_RASTERIZER_GRAPHICS_DIAGNOSTICS_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <windows.h>
#include <d3d9.h>

#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../event/frame.hpp"
#include "../halo_data/game_engine.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace GraphicsDiagnostics {
        struct BenchmarkState {
            static constexpr std::size_t CAPACITY = 16384;
            bool enabled = false;
            bool csv = true;
            double interval_seconds = 5.0;
            LARGE_INTEGER frequency {};
            LARGE_INTEGER last_frame {};
            LARGE_INTEGER window_start {};
            std::array<double, CAPACITY> frame_ms {};
            std::size_t count = 0;
            double sum_ms = 0.0;
        };

        inline BenchmarkState &benchmark_state() noexcept {
            static BenchmarkState state;
            return state;
        }

        inline const char *engine_name() noexcept {
            switch(game_engine()) {
                case GameEngine::GAME_ENGINE_CUSTOM_EDITION:
                    return "Halo Custom Edition";
                case GameEngine::GAME_ENGINE_RETAIL:
                    return "Halo PC Retail";
                case GameEngine::GAME_ENGINE_DEMO:
                    return "Halo Trial";
            }
            return "Unknown";
        }

        inline const char *anti_aliasing_name() noexcept {
            const auto *ini = get_chimera().get_ini();
            if(!ini) {
                return "off";
            }
            const char *aa = ini->get_value("graphics.anti_aliasing");
            return aa ? aa : "off";
        }

        inline std::size_t percentile_index(std::size_t count, double percentile) noexcept {
            if(count == 0) {
                return 0;
            }
            const double scaled = percentile * static_cast<double>(count - 1);
            const std::size_t index = static_cast<std::size_t>(scaled + 0.5);
            return index < count ? index : count - 1;
        }

        inline void write_benchmark_csv(
            std::size_t samples,
            double average_fps,
            double one_percent_low,
            double average_ms,
            double p95_ms,
            double p99_ms,
            double minimum_ms,
            double maximum_ms
        ) noexcept {
            auto &state = benchmark_state();
            if(!state.csv) {
                return;
            }

            bool write_header = true;
            {
                std::ifstream existing("chimera_graphics_benchmark.csv", std::ios::binary);
                if(existing.good() && existing.peek() != std::char_traits<char>::eof()) {
                    write_header = false;
                }
            }

            std::ofstream csv("chimera_graphics_benchmark.csv", std::ios::app);
            if(!csv.good()) {
                return;
            }
            if(write_header) {
                csv << "engine,anti_aliasing,samples,avg_fps,one_percent_low_fps,avg_ms,p95_ms,p99_ms,min_ms,max_ms\n";
            }
            csv << engine_name() << ','
                << anti_aliasing_name() << ','
                << samples << ','
                << std::fixed << std::setprecision(3)
                << average_fps << ','
                << one_percent_low << ','
                << average_ms << ','
                << p95_ms << ','
                << p99_ms << ','
                << minimum_ms << ','
                << maximum_ms << '\n';
        }

        inline void report_benchmark_window() noexcept {
            auto &state = benchmark_state();
            if(state.count == 0 || state.sum_ms <= 0.0) {
                state.count = 0;
                state.sum_ms = 0.0;
                return;
            }

            std::sort(state.frame_ms.begin(), state.frame_ms.begin() + state.count);
            const double average_ms = state.sum_ms / static_cast<double>(state.count);
            const double p95_ms = state.frame_ms[percentile_index(state.count, 0.95)];
            const double p99_ms = state.frame_ms[percentile_index(state.count, 0.99)];
            const double minimum_ms = state.frame_ms[0];
            const double maximum_ms = state.frame_ms[state.count - 1];
            const double average_fps = average_ms > 0.0 ? 1000.0 / average_ms : 0.0;
            const double one_percent_low = p99_ms > 0.0 ? 1000.0 / p99_ms : 0.0;

            console_output(
                "Chimera benchmark: %.1f FPS avg | %.3f ms avg | P95 %.3f ms | P99 %.3f ms | 1%% low %.1f FPS (%zu frames)",
                average_fps,
                average_ms,
                p95_ms,
                p99_ms,
                one_percent_low,
                state.count
            );

            write_benchmark_csv(
                state.count,
                average_fps,
                one_percent_low,
                average_ms,
                p95_ms,
                p99_ms,
                minimum_ms,
                maximum_ms
            );

            state.count = 0;
            state.sum_ms = 0.0;
        }

        inline void benchmark_frame() noexcept {
            auto &state = benchmark_state();
            if(!state.enabled || state.frequency.QuadPart <= 0) {
                return;
            }

            LARGE_INTEGER now {};
            if(!QueryPerformanceCounter(&now)) {
                return;
            }

            if(state.last_frame.QuadPart == 0) {
                state.last_frame = now;
                state.window_start = now;
                return;
            }

            const long long elapsed_ticks = now.QuadPart - state.last_frame.QuadPart;
            state.last_frame = now;
            if(elapsed_ticks <= 0) {
                return;
            }

            const double frame_ms =
                static_cast<double>(elapsed_ticks) * 1000.0 /
                static_cast<double>(state.frequency.QuadPart);

            // A >1 s gap is almost always an Alt+Tab, breakpoint, map transition,
            // or suspended window. Restart the measurement window instead of
            // reporting it as ordinary in-game frame pacing.
            if(frame_ms > 1000.0) {
                state.count = 0;
                state.sum_ms = 0.0;
                state.window_start = now;
                return;
            }

            if(state.count < BenchmarkState::CAPACITY) {
                state.frame_ms[state.count++] = frame_ms;
                state.sum_ms += frame_ms;
            }

            const double window_seconds =
                static_cast<double>(now.QuadPart - state.window_start.QuadPart) /
                static_cast<double>(state.frequency.QuadPart);
            if(window_seconds >= state.interval_seconds || state.count == BenchmarkState::CAPACITY) {
                report_benchmark_window();
                state.window_start = now;
            }
        }

        inline void set_up_benchmark() noexcept {
            auto &state = benchmark_state();
            const auto *ini = get_chimera().get_ini();
            if(!ini || !ini->get_value_bool("debug.benchmark").value_or(false)) {
                return;
            }
            if(!QueryPerformanceFrequency(&state.frequency) || state.frequency.QuadPart <= 0) {
                console_error("Chimera Graphics benchmark: high-resolution timer unavailable.");
                return;
            }

            state.enabled = true;
            state.csv = ini->get_value_bool("debug.benchmark_csv").value_or(true);
            double interval = ini->get_value_float("debug.benchmark_interval").value_or(5.0);
            if(interval < 1.0) {
                interval = 1.0;
            }
            else if(interval > 60.0) {
                interval = 60.0;
            }
            state.interval_seconds = interval;
            add_frame_event(benchmark_frame, EVENT_PRIORITY_AFTER);
            console_output(
                "Chimera Graphics benchmark enabled: %.1f-second windows%s.",
                state.interval_seconds,
                state.csv ? " with CSV output" : ""
            );
        }

        inline bool multisample_supported(
            IDirect3D9 *d3d9,
            UINT adapter,
            D3DDEVTYPE device_type,
            D3DFORMAT color_format,
            const D3DPRESENT_PARAMETERS &params,
            D3DMULTISAMPLE_TYPE type
        ) noexcept {
            if(!d3d9 || color_format == D3DFMT_UNKNOWN) {
                return false;
            }
            DWORD color_levels = 0;
            if(FAILED(IDirect3D9_CheckDeviceMultiSampleType(
                d3d9,
                adapter,
                device_type,
                color_format,
                params.Windowed,
                type,
                &color_levels
            )) || color_levels == 0) {
                return false;
            }
            if(params.EnableAutoDepthStencil && params.AutoDepthStencilFormat != D3DFMT_UNKNOWN) {
                DWORD depth_levels = 0;
                if(FAILED(IDirect3D9_CheckDeviceMultiSampleType(
                    d3d9,
                    adapter,
                    device_type,
                    params.AutoDepthStencilFormat,
                    params.Windowed,
                    type,
                    &depth_levels
                )) || depth_levels == 0) {
                    return false;
                }
            }
            return true;
        }

        inline bool texture_format_supported(
            IDirect3D9 *d3d9,
            UINT adapter,
            D3DDEVTYPE device_type,
            D3DFORMAT adapter_format,
            D3DFORMAT texture_format
        ) noexcept {
            return d3d9 && adapter_format != D3DFMT_UNKNOWN && SUCCEEDED(
                IDirect3D9_CheckDeviceFormat(
                    d3d9,
                    adapter,
                    device_type,
                    adapter_format,
                    0,
                    D3DRTYPE_TEXTURE,
                    texture_format
                )
            );
        }

        inline void report_once(
            IDirect3DDevice9 *device,
            bool pre_hud_validated,
            bool smaa_active
        ) noexcept {
            static bool reported = false;
            if(reported || !device) {
                return;
            }

            const auto *ini = get_chimera().get_ini();
            if(!ini || !ini->get_value_bool("debug.diagnostics").value_or(false)) {
                return;
            }
            reported = true;

            D3DCAPS9 caps {};
            IDirect3DDevice9_GetDeviceCaps(device, &caps);

            D3DDEVICE_CREATION_PARAMETERS creation {};
            const bool have_creation = SUCCEEDED(IDirect3DDevice9_GetCreationParameters(device, &creation));
            const UINT adapter = have_creation ? creation.AdapterOrdinal : D3DADAPTER_DEFAULT;
            const D3DDEVTYPE device_type = have_creation ? creation.DeviceType : D3DDEVTYPE_HAL;

            IDirect3D9 *d3d9 = nullptr;
            IDirect3DDevice9_GetDirect3D(device, &d3d9);

            D3DADAPTER_IDENTIFIER9 identifier {};
            if(d3d9) {
                IDirect3D9_GetAdapterIdentifier(d3d9, adapter, 0, &identifier);
            }

            D3DPRESENT_PARAMETERS params {};
            IDirect3DSwapChain9 *swap_chain = nullptr;
            if(SUCCEEDED(IDirect3DDevice9_GetSwapChain(device, 0, &swap_chain)) && swap_chain) {
                IDirect3DSwapChain9_GetPresentParameters(swap_chain, &params);
            }

            D3DSURFACE_DESC back_desc {};
            IDirect3DSurface9 *back_buffer = nullptr;
            if(SUCCEEDED(IDirect3DDevice9_GetBackBuffer(
                device,
                0,
                0,
                D3DBACKBUFFER_TYPE_MONO,
                &back_buffer
            )) && back_buffer) {
                IDirect3DSurface9_GetDesc(back_buffer, &back_desc);
            }

            D3DDISPLAYMODE display_mode {};
            if(d3d9) {
                IDirect3D9_GetAdapterDisplayMode(d3d9, adapter, &display_mode);
            }

            D3DFORMAT color_format = back_desc.Format;
            if(color_format == D3DFMT_UNKNOWN) {
                color_format = params.BackBufferFormat;
            }
            if(color_format == D3DFMT_UNKNOWN) {
                color_format = display_mode.Format;
            }

            D3DFORMAT adapter_format = display_mode.Format;
            if(adapter_format == D3DFMT_UNKNOWN) {
                adapter_format = color_format;
            }

            const bool msaa2 = multisample_supported(
                d3d9, adapter, device_type, color_format, params, D3DMULTISAMPLE_2_SAMPLES
            );
            const bool msaa4 = multisample_supported(
                d3d9, adapter, device_type, color_format, params, D3DMULTISAMPLE_4_SAMPLES
            );
            const bool msaa8 = multisample_supported(
                d3d9, adapter, device_type, color_format, params, D3DMULTISAMPLE_8_SAMPLES
            );

            const bool a8 = texture_format_supported(d3d9, adapter, device_type, adapter_format, D3DFMT_A8);
            const bool l8 = texture_format_supported(d3d9, adapter, device_type, adapter_format, D3DFMT_L8);
            const bool a8l8 = texture_format_supported(d3d9, adapter, device_type, adapter_format, D3DFMT_A8L8);
            const bool p8 = texture_format_supported(d3d9, adapter, device_type, adapter_format, D3DFMT_P8);

            const unsigned int ps_major = (caps.PixelShaderVersion >> 8U) & 0xFFU;
            const unsigned int ps_minor = caps.PixelShaderVersion & 0xFFU;
            const bool history_clamp = ini->get_value_bool("graphics.smaa_t2x_history_clamp").value_or(true);

            std::ofstream log("chimera_graphics_diagnostics.log", std::ios::trunc);
            if(log.good()) {
                log << "Chimera Graphics diagnostics\n";
                log << "engine=" << engine_name() << '\n';
                log << "adapter=" << (identifier.Description[0] ? identifier.Description : "unknown") << '\n';
                log << "vendor_id=0x" << std::hex << identifier.VendorId << std::dec << '\n';
                log << "device_id=0x" << std::hex << identifier.DeviceId << std::dec << '\n';
                log << "pixel_shader=" << ps_major << '.' << ps_minor << '\n';
                log << "max_anisotropy=" << caps.MaxAnisotropy << '\n';
                log << "backbuffer=" << back_desc.Width << 'x' << back_desc.Height << '\n';
                log << "windowed=" << (params.Windowed ? 1 : 0) << '\n';
                log << "anti_aliasing=" << anti_aliasing_name() << '\n';
                log << "pre_hud_validated=" << (pre_hud_validated ? 1 : 0) << '\n';
                log << "smaa_active=" << (smaa_active ? 1 : 0) << '\n';
                log << "smaa_t2x_history_clamp=" << (history_clamp ? 1 : 0) << '\n';
                log << "msaa2_supported=" << (msaa2 ? 1 : 0) << '\n';
                log << "msaa4_supported=" << (msaa4 ? 1 : 0) << '\n';
                log << "msaa8_supported=" << (msaa8 ? 1 : 0) << '\n';
                log << "texture_a8_supported=" << (a8 ? 1 : 0) << '\n';
                log << "texture_l8_supported=" << (l8 ? 1 : 0) << '\n';
                log << "texture_a8l8_supported=" << (a8l8 ? 1 : 0) << '\n';
                log << "texture_p8_supported=" << (p8 ? 1 : 0) << '\n';
            }

            console_output(
                "Chimera Graphics diagnostics: %s | PS %u.%u | MSAA 2x:%s 4x:%s 8x:%s",
                identifier.Description[0] ? identifier.Description : "unknown adapter",
                ps_major,
                ps_minor,
                msaa2 ? "yes" : "no",
                msaa4 ? "yes" : "no",
                msaa8 ? "yes" : "no"
            );
            console_output("Chimera Graphics diagnostics written to chimera_graphics_diagnostics.log");

            if(back_buffer) {
                IDirect3DSurface9_Release(back_buffer);
            }
            if(swap_chain) {
                IDirect3DSwapChain9_Release(swap_chain);
            }
            if(d3d9) {
                IDirect3D9_Release(d3d9);
            }
        }
    }
}

#endif
