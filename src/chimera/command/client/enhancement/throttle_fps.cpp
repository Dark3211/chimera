// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <limits>

#include "../../command.hpp"
#include "../../../signature/hook.hpp"
#include "../../../signature/signature.hpp"
#include "../../../chimera.hpp"
#include "../../../output/output.hpp"

namespace Chimera {
    static LARGE_INTEGER pc_freq {};
    static long long frame_time_ticks = 0;
    static long long next_frame_tick = 0;
    static bool limiter_enabled = false;
    static HANDLE pacing_timer = nullptr;

    static void close_pacing_timer() noexcept {
        if(pacing_timer) {
            CancelWaitableTimer(pacing_timer);
            CloseHandle(pacing_timer);
            pacing_timer = nullptr;
        }
    }

    static HANDLE create_pacing_timer() noexcept {
        if(pacing_timer) {
            return pacing_timer;
        }

        // Use high-resolution waitable timers when available.
        using CreateWaitableTimerExWProc = HANDLE (WINAPI *)(
            LPSECURITY_ATTRIBUTES,
            LPCWSTR,
            DWORD,
            DWORD
        );
        constexpr DWORD HIGH_RESOLUTION_TIMER_FLAG = 0x00000002;

        HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
        if(kernel32) {
            FARPROC address = GetProcAddress(kernel32, "CreateWaitableTimerExW");
            if(address) {
                CreateWaitableTimerExWProc create_timer_ex = nullptr;
                static_assert(sizeof(create_timer_ex) == sizeof(address),
                              "Unexpected CreateWaitableTimerExW function-pointer size");
                std::memcpy(&create_timer_ex, &address, sizeof(address));
                pacing_timer = create_timer_ex(
                    nullptr,
                    nullptr,
                    HIGH_RESOLUTION_TIMER_FLAG,
                    TIMER_ALL_ACCESS
                );
            }
        }

        if(!pacing_timer) {
            pacing_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
        return pacing_timer;
    }

    static void reset_frame_deadline() noexcept {
        next_frame_tick = 0;
    }

    static void wait_until_deadline(long long deadline) noexcept {
        LARGE_INTEGER now {};
        if(!QueryPerformanceCounter(&now)) {
            return;
        }

        // Reserve ~0.35 ms for QPC polling.
        const long long spin_ticks = std::max<long long>(
            1,
            static_cast<long long>(static_cast<double>(pc_freq.QuadPart) * 0.00035)
        );

        while(now.QuadPart < deadline) {
            const long long remaining = deadline - now.QuadPart;
            if(remaining > spin_ticks) {
                const long long coarse_ticks = remaining - spin_ticks;
                HANDLE timer = create_pacing_timer();
                if(timer) {
                    LARGE_INTEGER due {};
                    long long hundred_ns = static_cast<long long>(
                        (static_cast<long double>(coarse_ticks) * 10000000.0L) /
                        static_cast<long double>(pc_freq.QuadPart)
                    );
                    if(hundred_ns < 1) {
                        hundred_ns = 1;
                    }
                    due.QuadPart = -hundred_ns;
                    if(SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                        WaitForSingleObject(timer, INFINITE);
                    }
                    else {
                        Sleep(0);
                    }
                }
                else {
                    Sleep(0);
                }
            }

            if(!QueryPerformanceCounter(&now)) {
                return;
            }
        }
    }

    void on_frame() noexcept {
        if(!limiter_enabled || pc_freq.QuadPart <= 0 || frame_time_ticks <= 0) {
            return;
        }

        LARGE_INTEGER now {};
        if(!QueryPerformanceCounter(&now)) {
            reset_frame_deadline();
            return;
        }

        if(next_frame_tick <= 0) {
            next_frame_tick = now.QuadPart + frame_time_ticks;
            return;
        }

        // Reset cadence after long stalls.
        const long long late_by = now.QuadPart - next_frame_tick;
        if(late_by > frame_time_ticks * 4) {
            next_frame_tick = now.QuadPart + frame_time_ticks;
            return;
        }

        if(now.QuadPart < next_frame_tick) {
            wait_until_deadline(next_frame_tick);
            if(!QueryPerformanceCounter(&now)) {
                reset_frame_deadline();
                return;
            }
        }

        // Advance from the scheduled deadline.
        next_frame_tick += frame_time_ticks;
        if(now.QuadPart - next_frame_tick > frame_time_ticks * 2) {
            next_frame_tick = now.QuadPart + frame_time_ticks;
        }
    }

    bool throttle_fps_command(int argument_count, const char **arguments) noexcept {
        static bool enabled = false;
        static bool hook_enabled = false;
        static float frame_time_target = 0.0f;

        if(argument_count) {
            errno = 0;
            char *end = nullptr;
            float new_fps = std::strtof(arguments[0], &end);
            const bool valid_fps =
                errno != ERANGE && end != arguments[0] && end && *end == 0 &&
                std::isfinite(new_fps);

            // If the user inputs an invalid framerate, assume they are turning it off.
            if(!valid_fps || new_fps <= 0.0f) {
                enabled = false;
                limiter_enabled = false;
                reset_frame_deadline();
                close_pacing_timer();
            }
            else {
                // 1 means the default cap.
                if(new_fps == 1.0f) {
                    new_fps = 300.0f;
                }

                if(!QueryPerformanceFrequency(&pc_freq) || pc_freq.QuadPart <= 0) {
                    enabled = false;
                    limiter_enabled = false;
                    console_error("Unable to initialize the high-resolution timer.");
                    return false;
                }

                const long double requested_ticks =
                    static_cast<long double>(pc_freq.QuadPart) /
                    static_cast<long double>(new_fps);
                if(!std::isfinite(static_cast<double>(requested_ticks)) ||
                   requested_ticks > static_cast<long double>(std::numeric_limits<long long>::max() / 8)) {
                    enabled = false;
                    limiter_enabled = false;
                    console_error("Requested FPS cap is outside the supported timer range.");
                    return false;
                }

                enabled = true;
                frame_time_target = 1.0f / new_fps;
                frame_time_ticks = std::max<long long>(1, static_cast<long long>(requested_ticks));
                reset_frame_deadline();
                create_pacing_timer();

                if(!hook_enabled) {
                    static Hook hook;
                    auto *present_site = get_chimera().get_signature("d3d9_present_frame_sig").data();
                    if(present_site) {
                        write_jmp_call(present_site, hook, reinterpret_cast<const void *>(on_frame), nullptr);
                    }
                    hook_enabled = present_site && hook.address == present_site && hook.hook && !hook.original_bytes.empty();
                    if(!hook_enabled) {
                        enabled = false;
                        limiter_enabled = false;
                        reset_frame_deadline();
                        close_pacing_timer();
                        console_error("FPS limiter disabled: D3D9 Present signature/hook validation failed.");
                        return false;
                    }
                }
                limiter_enabled = true;
            }
        }

        if(enabled) {
            console_output("%.02f FPS", 1.0 / frame_time_target);
        }
        else {
            console_output("off");
        }

        return true;
    }
}
