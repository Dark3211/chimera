// SPDX-License-Identifier: GPL-3.0-only

#include "d3d9_end_scene.hpp"
#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../output/output.hpp"

namespace Chimera {
    static std::vector<Event<EndSceneEventFunction>> end_scene_events;
    static std::size_t end_scene_events_version = 0;
    static ReusableEventDispatcher<EndSceneEventFunction> end_scene_dispatcher;

    static std::vector<Event<EndSceneEventFunction>> end_scene_after_events;
    static std::size_t end_scene_after_events_version = 0;
    static ReusableEventDispatcher<EndSceneEventFunction> end_scene_after_dispatcher;

    // Preserve the device pointer for post-EndScene callbacks.
    static LPDIRECT3DDEVICE9 active_end_scene_device = nullptr;

    static void enable_d3d9_end_scene_hook();

    extern "C" {
        void on_d3d9_end_scene_asm();
        void on_d3d9_end_scene_after_asm();
    }

    void add_d3d9_end_scene_event(const EndSceneEventFunction function, EventPriority priority) {
        remove_d3d9_end_scene_event(function);
        enable_d3d9_end_scene_hook();
        end_scene_events.emplace_back(Event<EndSceneEventFunction> { function, priority });
        end_scene_events_version++;
    }

    void remove_d3d9_end_scene_event(const EndSceneEventFunction function) {
        for(std::size_t i = 0; i < end_scene_events.size(); i++) {
            if(end_scene_events[i].function == function) {
                end_scene_events.erase(end_scene_events.begin() + i);
                end_scene_events_version++;
                return;
            }
        }
    }

    void add_d3d9_end_scene_after_event(const EndSceneEventFunction function, EventPriority priority) {
        remove_d3d9_end_scene_after_event(function);
        enable_d3d9_end_scene_hook();
        end_scene_after_events.emplace_back(Event<EndSceneEventFunction> { function, priority });
        end_scene_after_events_version++;
    }

    void remove_d3d9_end_scene_after_event(const EndSceneEventFunction function) {
        for(std::size_t i = 0; i < end_scene_after_events.size(); i++) {
            if(end_scene_after_events[i].function == function) {
                end_scene_after_events.erase(end_scene_after_events.begin() + i);
                end_scene_after_events_version++;
                return;
            }
        }
    }

    extern "C" void do_d3d9_end_scene_event(LPDIRECT3DDEVICE9 device) {
        active_end_scene_device = device;
        end_scene_dispatcher.dispatch_versioned(end_scene_events, end_scene_events_version, device);
    }

    extern "C" void do_d3d9_end_scene_after_event(HRESULT end_scene_result) {
        auto *device = active_end_scene_device;
        active_end_scene_device = nullptr;

        // Skip post-processing when EndScene fails.
        if(SUCCEEDED(end_scene_result) && device) {
            end_scene_after_dispatcher.dispatch_versioned(end_scene_after_events, end_scene_after_events_version, device);
        }
    }

    static void enable_d3d9_end_scene_hook() {
        static bool enabled = false;
        static bool failure_reported = false;
        static Hook hook;
        if(enabled) {
            return;
        }
        auto *call_site = get_chimera().get_signature("d3d9_call_end_scene_sig").data();
        if(!call_site) {
            if(!failure_reported) {
                console_error("Chimera D3D9 EndScene hook disabled: signature was not found.");
                failure_reported = true;
            }
            return;
        }
        write_jmp_call(call_site, hook, reinterpret_cast<const void *>(on_d3d9_end_scene_asm),
                       reinterpret_cast<const void *>(on_d3d9_end_scene_after_asm), false);
        enabled = hook.address == call_site && hook.hook && !hook.original_bytes.empty();
        if(!enabled && !failure_reported) {
            console_error("Chimera D3D9 EndScene hook disabled: executable validation or trampoline creation failed.");
            failure_reported = true;
        }
    }
}
