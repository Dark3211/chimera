// SPDX-License-Identifier: GPL-3.0-only

#include "d3d9_reset.hpp"
#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../rasterizer/graphics_runtime_metrics.hpp"
#include "../output/output.hpp"
#include "../startup_trace.hpp"

namespace Chimera {
    struct D3D9ResetEvent {
        ResetEventFunction function;
        EventPriority priority;
        const char *debug_name;
    };

    static std::vector<D3D9ResetEvent> reset_events;
    static bool reset_hook_installed = false;

    static void enable_d3d9_reset_hook();

    extern "C" {
        void on_d3d9_reset_asm();
    }

    void add_d3d9_reset_event(
        const ResetEventFunction function,
        EventPriority priority,
        const char *debug_name
    ) {
        // Remove if exists
        remove_d3d9_reset_event(function);

        // Enable hook if necessary
        enable_d3d9_reset_hook();

        // Add the event and its stable diagnostic name.
        reset_events.emplace_back(D3D9ResetEvent { function, priority, debug_name });
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
        // Keep the same snapshot semantics as call_in_order(): callbacks may add or
        // remove registrations, but those changes apply to the next Reset operation.
        const auto events = reset_events;
        startup_trace_log("Halo Reset call-site hook reached: device=%p queued_events=%zu",
                          static_cast<void *>(device),
                          events.size());
        startup_trace_on_reset_call(device, present);
        GraphicsRuntimeMetrics::reset_begin();

        std::size_t callback_index = 0;
        const auto run_priority = [&](EventPriority priority) {
            for(const auto &event : events) {
                if(event.priority != priority || !event.function) {
                    continue;
                }

                const auto index = ++callback_index;
                const char *name = event.debug_name ? event.debug_name : "(unnamed reset callback)";
                startup_trace_log(
                    "D3D9 Reset callback #%zu BEGIN name=%s function=%p priority=%u",
                    index,
                    name,
                    reinterpret_cast<void *>(event.function),
                    static_cast<unsigned int>(event.priority)
                );
                startup_trace_log_device_cooperative_level("Before D3D9 Reset callback", device);
                event.function(device, present);
                startup_trace_log_device_cooperative_level("After D3D9 Reset callback", device);
                startup_trace_log(
                    "D3D9 Reset callback #%zu END name=%s function=%p",
                    index,
                    name,
                    reinterpret_cast<void *>(event.function)
                );
            }
        };

        run_priority(EVENT_PRIORITY_BEFORE);
        run_priority(EVENT_PRIORITY_DEFAULT);
        run_priority(EVENT_PRIORITY_AFTER);
        run_priority(EVENT_PRIORITY_FINAL);
        startup_trace_log("D3D9 Reset callbacks completed: invoked=%zu registered=%zu",
                          callback_index,
                          events.size());
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
