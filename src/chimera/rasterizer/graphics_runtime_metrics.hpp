// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_RASTERIZER_GRAPHICS_RUNTIME_METRICS_HPP
#define CHIMERA_RASTERIZER_GRAPHICS_RUNTIME_METRICS_HPP

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <windows.h>

namespace Chimera {
    namespace GraphicsRuntimeMetrics {
        enum class Subsystem : std::uint8_t { ENHANCED = 0, SMAA = 1 };

        struct State {
            std::uint64_t resources_created = 0;
            std::uint64_t resources_released = 0;
            std::uint64_t reset_events = 0;
            std::uint64_t enhanced_recoveries = 0;
            std::uint64_t smaa_recoveries = 0;
            std::uint64_t shader_compiles = 0;
            std::uint64_t shader_cache_hits = 0;
            double last_enhanced_recovery_ms = 0.0;
            double last_smaa_recovery_ms = 0.0;
            LARGE_INTEGER frequency {};
            LARGE_INTEGER reset_started {};
            bool enhanced_reset_pending = false;
            bool smaa_reset_pending = false;
            bool diagnostics_enabled = false;
        };

        inline State &state() noexcept {
            static State instance;
            return instance;
        }

        inline void set_enabled(bool enabled) noexcept {
            state().diagnostics_enabled = enabled;
        }

        inline bool enabled() noexcept {
            return state().diagnostics_enabled;
        }

        inline void ensure_frequency() noexcept {
            if(!enabled()) {
                return;
            }
            auto &s = state();
            if(s.frequency.QuadPart <= 0) {
                QueryPerformanceFrequency(&s.frequency);
            }
        }

        inline std::uint64_t live_resources() noexcept {
            const auto &s = state();
            return s.resources_created >= s.resources_released
                ? s.resources_created - s.resources_released
                : 0;
        }

        inline void write_log() noexcept {
            if(!enabled()) {
                return;
            }
            const auto &s = state();
            std::ofstream log("chimera_graphics_runtime.log", std::ios::trunc);
            if(!log.good()) {
                return;
            }
            log << "Chimera Graphics runtime validation\n";
            log << "reset_events=" << s.reset_events << '\n';
            log << "resources_created=" << s.resources_created << '\n';
            log << "resources_released=" << s.resources_released << '\n';
            log << "live_resources=" << live_resources() << '\n';
            log << "enhanced_recoveries=" << s.enhanced_recoveries << '\n';
            log << "smaa_recoveries=" << s.smaa_recoveries << '\n';
            log << std::fixed << std::setprecision(3);
            log << "last_enhanced_recovery_ms=" << s.last_enhanced_recovery_ms << '\n';
            log << "last_smaa_recovery_ms=" << s.last_smaa_recovery_ms << '\n';
            log << "shader_compiles=" << s.shader_compiles << '\n';
            log << "shader_cache_hits=" << s.shader_cache_hits << '\n';
        }

        inline void reset_begin() noexcept {
            if(!enabled()) {
                return;
            }
            auto &s = state();
            ensure_frequency();
            QueryPerformanceCounter(&s.reset_started);
            s.reset_events++;
            write_log();
        }

        inline void subsystem_reset(Subsystem subsystem) noexcept {
            if(!enabled()) {
                return;
            }
            auto &s = state();
            if(subsystem == Subsystem::ENHANCED) {
                s.enhanced_reset_pending = true;
            }
            else {
                s.smaa_reset_pending = true;
            }
        }

        inline void resources_created(std::uint32_t count) noexcept {
            if(enabled()) {
                state().resources_created += count;
            }
        }

        inline void resources_released(std::uint32_t count) noexcept {
            if(enabled()) {
                state().resources_released += count;
            }
        }

        inline double recovery_ms() noexcept {
            if(!enabled()) {
                return 0.0;
            }
            auto &s = state();
            ensure_frequency();
            if(s.frequency.QuadPart <= 0 || s.reset_started.QuadPart <= 0) {
                return 0.0;
            }
            LARGE_INTEGER now {};
            if(!QueryPerformanceCounter(&now)) {
                return 0.0;
            }
            return static_cast<double>(now.QuadPart - s.reset_started.QuadPart) * 1000.0 /
                   static_cast<double>(s.frequency.QuadPart);
        }

        inline void subsystem_recovered(Subsystem subsystem) noexcept {
            if(!enabled()) {
                return;
            }
            auto &s = state();
            if(subsystem == Subsystem::ENHANCED && s.enhanced_reset_pending) {
                s.last_enhanced_recovery_ms = recovery_ms();
                s.enhanced_recoveries++;
                s.enhanced_reset_pending = false;
            }
            else if(subsystem == Subsystem::SMAA && s.smaa_reset_pending) {
                s.last_smaa_recovery_ms = recovery_ms();
                s.smaa_recoveries++;
                s.smaa_reset_pending = false;
            }
            write_log();
        }

        inline void shader_compiled() noexcept {
            if(!enabled()) {
                return;
            }
            state().shader_compiles++;
            write_log();
        }

        inline void shader_cache_hit() noexcept {
            if(!enabled()) {
                return;
            }
            state().shader_cache_hits++;
            write_log();
        }
    }
}

#endif
