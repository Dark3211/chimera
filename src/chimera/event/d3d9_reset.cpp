// SPDX-License-Identifier: GPL-3.0-only

#include "d3d9_reset.hpp"
#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../rasterizer/graphics_runtime_metrics.hpp"
#include "../output/output.hpp"
#include "../startup_trace.hpp"

namespace Chimera {
    static std::vector<Event<ResetEventFunction>> reset_events;
    static bool reset_hook_installed = false;

    static void enable_d3d9_reset_hook();

    extern "C" {
        void on_d3d9_reset_asm();
    }

    void add_d3d9_reset_event(const ResetEventFunction function, EventPriority priority) {
        // Remove if exists
        remove_d3d9_reset_event(function);

        // Enable hook if necessary
        enable_d3d9_reset_hook();

        // Add the event
        reset_events.emplace_back(Event<ResetEventFunction> { function, priority });
    }

    void remove_d3d9_reset_event(const ResetEventFunction function) {
        for(std::size_t i = 0; i < reset_events.size(); i++) {
            if(reset_events[i].function == function) {
                reset_events.erase(reset_events.begin() + i);
                return;
            }
        }
    }

    bool d3d9_reset_hook_active() noexcept {
        return reset_hook_installed;
    }

    extern "C" void do_d3d9_reset_event(LPDIRECT3DDEVICE9 device, D3DPRESENT_PARAMETERS *present) {
        startup_trace_log("Halo Reset call-site hook reached: device=%p queued_events=%zu",
                          static_cast<void *>(device),
                          reset_events.size());
        if(present) {
            startup_trace_log(
                "Halo Reset call-site parameters: Windowed=%u BackBuffer=%lux%lu Format=%u Swap=%u Flags=0x%08lx Refresh=%lu Interval=0x%08lx",
                static_cast<unsigned int>(present->Windowed),
                static_cast<unsigned long>(present->BackBufferWidth),
                static_cast<unsigned long>(present->BackBufferHeight),
                static_cast<unsigned int>(present->BackBufferFormat),
                static_cast<unsigned int>(present->SwapEffect),
                static_cast<unsigned long>(present->Flags),
                static_cast<unsigned long>(present->FullScreen_RefreshRateInHz),
                static_cast<unsigned long>(present->PresentationInterval)
            );
        }
        GraphicsRuntimeMetrics::reset_begin();
        call_in_order(reset_events, device, present);
    }

    static void enable_d3d9_reset_hook() {
        static bool enabled = false;
        static bool failure_reported = false;
        static Hook hook;
        if(enabled) {
            return;
        }
        auto *call_site = get_chimera().get_signature("d3d9_call_reset_sig").data();
        if(!call_site) {
            if(!failure_reported) {
                console_error("Chimera D3D9 Reset hook disabled: signature was not found.");
                failure_reported = true;
            }
            return;
        }
        write_jmp_call(call_site, hook, reinterpret_cast<const void *>(on_d3d9_reset_asm), nullptr, false);
        enabled = hook.address == call_site && hook.hook && !hook.original_bytes.empty();
        reset_hook_installed = enabled;
        if(!enabled && !failure_reported) {
            console_error("Chimera D3D9 Reset hook disabled: executable validation or trampoline creation failed.");
            failure_reported = true;
        }
    }
}
