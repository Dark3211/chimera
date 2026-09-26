

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>

#include "interface_render.hpp"
#include "../chimera.hpp"
#include "../halo_data/game_engine.hpp"
#include "../rasterizer/rasterizer.hpp"
#include "../signature/hook.hpp"
#include "../signature/hac/codefinder.h"
#include "../output/output.hpp"

namespace Chimera {
    namespace {
        static std::vector<Event<InterfaceRenderEventFunction>> hud_render_events;
        static std::vector<Event<InterfaceRenderEventFunction>> ui_render_events;
        static std::size_t hud_render_events_version = 0;
        static std::size_t ui_render_events_version = 0;
        static ReusableEventDispatcher<InterfaceRenderEventFunction> hud_render_dispatcher;
        static ReusableEventDispatcher<InterfaceRenderEventFunction> ui_render_dispatcher;

        static Hook hud_render_hook;
        static Hook ui_render_hook;

        static std::byte *hud_render_call_site = nullptr;
        static std::byte *ui_render_call_site = nullptr;
        static bool hud_render_discovery_attempted = false;
        static bool ui_render_discovery_attempted = false;

        static std::byte *relative_call_target(std::byte *site) noexcept {
            if(!is_executable_memory_range(site, 5) ||
               *reinterpret_cast<const std::uint8_t *>(site) != 0xE8) {
                return nullptr;
            }

            std::int32_t displacement = 0;
            std::memcpy(&displacement, site + 1, sizeof(displacement));
            auto *target = site + 5 + displacement;
            return is_executable_memory_range(target, 1) ? target : nullptr;
        }

        template<std::size_t N>
        static std::vector<std::byte *> find_pattern_matches(
            const std::array<short, N> &pattern
        ) noexcept {
            std::vector<std::byte *> matches;
            auto module = GetModuleHandle(nullptr);
            if(!module) {
                return matches;
            }

            try {
                CodeFinder finder(
                    module,
                    pattern.data(),
                    static_cast<unsigned int>(pattern.size())
                );
                const auto locations = finder.find();
                matches.reserve(locations.size());
                for(const auto location : locations) {
                    if(location) {
                        matches.emplace_back(reinterpret_cast<std::byte *>(location));
                    }
                }
            }
            catch(...) {
                matches.clear();
            }
            return matches;
        }

        template<std::size_t N>
        static std::byte *find_unique_pattern(
            const std::array<short, N> &pattern
        ) noexcept {
            const auto matches = find_pattern_matches(pattern);
            return matches.size() == 1 ? matches[0] : nullptr;
        }

        static bool contains_address(
            const std::vector<std::byte *> &matches,
            const std::byte *address
        ) noexcept {
            if(!address) {
                return false;
            }
            for(const auto *match : matches) {
                if(match == address) {
                    return true;
                }
            }
            return false;
        }

        static std::byte *find_hud_render_function() noexcept {

            static constexpr std::array<short, 26> pattern {{
                0x8B, 0x0D, -1, -1, -1, -1,
                0x66, 0x83, 0xF9, 0xFF, 0x53, 0x57, 0x74, 0x15,
                0x66, 0x83, 0xF9, 0x01, 0x7D, 0x0F,
                0x8B, 0x15, -1, -1, -1, -1
            }};
            return find_unique_pattern(pattern);
        }

        static std::byte *discover_hud_render_call_site() noexcept {
            if(hud_render_call_site) {
                return hud_render_call_site;
            }
            if(hud_render_discovery_attempted) {
                return nullptr;
            }
            hud_render_discovery_attempted = true;

            if(!get_chimera().feature_present("client") ||
               (game_engine() != GameEngine::GAME_ENGINE_CUSTOM_EDITION &&
                game_engine() != GameEngine::GAME_ENGINE_RETAIL)) {
                return nullptr;
            }

            if(game_engine() == GameEngine::GAME_ENGINE_CUSTOM_EDITION) {
                auto *module = reinterpret_cast<std::byte *>(GetModuleHandle(nullptr));
                constexpr std::uintptr_t CE_HUD_CALL_RVA = 0x10FE61;
                constexpr std::uintptr_t CE_HUD_FUNCTION_RVA = 0x0974F0;
                if(!module) {
                    return nullptr;
                }

                auto *call_site = module + CE_HUD_CALL_RVA;
                auto *expected_function = module + CE_HUD_FUNCTION_RVA;
                if(relative_call_target(call_site) != expected_function) {
                    return nullptr;
                }

                hud_render_call_site = call_site;
                return hud_render_call_site;
            }

            static constexpr std::array<short, 18> pattern {{
                0xE8, -1, -1, -1, -1,
                0xE8, -1, -1, -1, -1,
                0xA1, -1, -1, -1, -1,
                0x85, 0xC0, 0x5E
            }};

            auto *sequence = find_unique_pattern(pattern);
            auto *expected_function = find_hud_render_function();
            if(!sequence || !expected_function) {
                return nullptr;
            }

            auto *call_site = sequence + 5;
            if(relative_call_target(call_site) != expected_function ||
               validated_retail_pre_hud_call_site() != call_site) {
                return nullptr;
            }

            hud_render_call_site = call_site;
            return hud_render_call_site;
        }

        static std::byte *discover_ui_render_call_site() noexcept {
            if(ui_render_call_site) {
                return ui_render_call_site;
            }
            if(ui_render_discovery_attempted) {
                return nullptr;
            }
            ui_render_discovery_attempted = true;

            if(!get_chimera().feature_present("client") ||
               (game_engine() != GameEngine::GAME_ENGINE_CUSTOM_EDITION &&
                game_engine() != GameEngine::GAME_ENGINE_RETAIL)) {
                return nullptr;
            }

            if(game_engine() == GameEngine::GAME_ENGINE_CUSTOM_EDITION) {
                auto *module = reinterpret_cast<std::byte *>(GetModuleHandle(nullptr));
                constexpr std::uintptr_t CE_UI_CALL_RVA = 0x10FE6D;
                constexpr std::uintptr_t CE_UI_FUNCTION_RVA = 0x09B450;
                if(!module) {
                    return nullptr;
                }

                auto *call_site = module + CE_UI_CALL_RVA;
                auto *expected_function = module + CE_UI_FUNCTION_RVA;
                if(relative_call_target(call_site) != expected_function) {
                    return nullptr;
                }

                ui_render_call_site = call_site;
                return ui_render_call_site;
            }

            static constexpr std::array<short, 25> call_pattern {{
                0xE8, -1, -1, -1, -1,
                0xE8, -1, -1, -1, -1,
                0x8B, 0xC5,
                0xE8, -1, -1, -1, -1,
                0x66, 0x83, 0x3D, -1, -1, -1, -1, 0xFF
            }};
            static constexpr std::array<short, 25> function_pattern {{
                0x33, 0xC9, 0x83, 0xEC, 0x14, 0x66, 0x3D, 0xFF, 0xFF,
                0x0F, 0x94, 0xC1, 0x53, 0x33, 0xDB, 0x49, 0x23, 0xC8,
                0x66, 0x89, 0x0D, -1, -1, -1, -1
            }};
            static constexpr std::array<short, 5> any_call_pattern {{
                0xE8, -1, -1, -1, -1
            }};

            const auto call_sequences = find_pattern_matches(call_pattern);
            const auto ui_functions = find_pattern_matches(function_pattern);

            std::byte *candidate = nullptr;
            unsigned int relational_candidates = 0;
            for(auto *sequence : call_sequences) {
                auto *call_site = sequence + 12;
                auto *target = relative_call_target(call_site);
                if(!contains_address(ui_functions, target)) {
                    continue;
                }

                relational_candidates++;
                if(candidate && candidate != call_site) {
                    console_error(
                        "API 2.076 Retail UI: ambiguous Balltze candidates (%u).",
                        relational_candidates
                    );
                    return nullptr;
                }
                candidate = call_site;
            }

            if(candidate && relational_candidates == 1) {
                ui_render_call_site = candidate;
                return ui_render_call_site;
            }

            std::byte *unique_ui_caller = nullptr;
            unsigned int ui_caller_count = 0;
            if(call_sequences.empty() && ui_functions.size() == 1) {
                const auto all_calls = find_pattern_matches(any_call_pattern);
                for(auto *call_site : all_calls) {
                    if(relative_call_target(call_site) == ui_functions[0]) {
                        ui_caller_count++;
                        if(ui_caller_count == 1) {
                            unique_ui_caller = call_site;
                        }
                        else {
                            unique_ui_caller = nullptr;
                            break;
                        }
                    }
                }
            }

            if(unique_ui_caller && ui_caller_count == 1) {
                ui_render_call_site = unique_ui_caller;
                return ui_render_call_site;
            }

            console_error(
                "API 2.076 Retail UI unresolved: call_sequences=%u ui_functions=%u relational=%u ui_callers=%u",
                static_cast<unsigned int>(call_sequences.size()),
                static_cast<unsigned int>(ui_functions.size()),
                relational_candidates,
                ui_caller_count
            );
            return nullptr;
        }

        static void on_hud_render_before() noexcept {
            hud_render_dispatcher.dispatch_versioned(
                hud_render_events, hud_render_events_version, false
            );
        }

        static void on_hud_render_after() noexcept {
            hud_render_dispatcher.dispatch_versioned(
                hud_render_events, hud_render_events_version, true
            );
        }

        static void on_ui_render_before() noexcept {
            ui_render_dispatcher.dispatch_versioned(
                ui_render_events, ui_render_events_version, false
            );
        }

        static void on_ui_render_after() noexcept {
            ui_render_dispatcher.dispatch_versioned(
                ui_render_events, ui_render_events_version, true
            );
        }

        static bool ensure_hud_render_hook() noexcept {
            if(hud_render_hook.address && hud_render_hook.hook &&
               !hud_render_hook.original_bytes.empty()) {
                return true;
            }

            auto *call_site = discover_hud_render_call_site();
            discover_ui_render_call_site();
            if(!call_site || *reinterpret_cast<const std::uint8_t *>(call_site) != 0xE8) {
                return false;
            }

            write_jmp_call(call_site, hud_render_hook,
                           reinterpret_cast<const void *>(on_hud_render_before),
                           reinterpret_cast<const void *>(on_hud_render_after));
            return hud_render_hook.address == call_site && hud_render_hook.hook &&
                   !hud_render_hook.original_bytes.empty();
        }

        static bool ensure_ui_render_hook() noexcept {
            if(ui_render_hook.address && ui_render_hook.hook &&
               !ui_render_hook.original_bytes.empty()) {
                return true;
            }

            discover_hud_render_call_site();
            auto *call_site = discover_ui_render_call_site();
            if(!call_site || *reinterpret_cast<const std::uint8_t *>(call_site) != 0xE8) {
                return false;
            }

            write_jmp_call(call_site, ui_render_hook,
                           reinterpret_cast<const void *>(on_ui_render_before),
                           reinterpret_cast<const void *>(on_ui_render_after));
            return ui_render_hook.address == call_site && ui_render_hook.hook &&
                   !ui_render_hook.original_bytes.empty();
        }
    }

    bool hud_render_event_supported() noexcept {
        return discover_hud_render_call_site() != nullptr;
    }

    bool ui_render_event_supported() noexcept {
        return discover_ui_render_call_site() != nullptr;
    }

    const void *hud_render_event_call_site() noexcept {
        return discover_hud_render_call_site();
    }

    const void *ui_render_event_call_site() noexcept {
        return discover_ui_render_call_site();
    }

    bool add_hud_render_event(InterfaceRenderEventFunction function, EventPriority priority) noexcept {
        if(!function || !ensure_hud_render_hook()) {
            return false;
        }

        remove_hud_render_event(function);
        hud_render_events.emplace_back(Event<InterfaceRenderEventFunction> { function, priority });
        hud_render_events_version++;
        return true;
    }

    void remove_hud_render_event(InterfaceRenderEventFunction function) noexcept {
        for(std::size_t i = 0; i < hud_render_events.size(); i++) {
            if(hud_render_events[i].function == function) {
                hud_render_events.erase(hud_render_events.begin() + i);
                hud_render_events_version++;
                return;
            }
        }
    }

    bool add_ui_render_event(InterfaceRenderEventFunction function, EventPriority priority) noexcept {
        if(!function || !ensure_ui_render_hook()) {
            return false;
        }

        remove_ui_render_event(function);
        ui_render_events.emplace_back(Event<InterfaceRenderEventFunction> { function, priority });
        ui_render_events_version++;
        return true;
    }

    void remove_ui_render_event(InterfaceRenderEventFunction function) noexcept {
        for(std::size_t i = 0; i < ui_render_events.size(); i++) {
            if(ui_render_events[i].function == function) {
                ui_render_events.erase(ui_render_events.begin() + i);
                ui_render_events_version++;
                return;
            }
        }
    }
}
