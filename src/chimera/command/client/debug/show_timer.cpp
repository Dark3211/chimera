// SPDX-License-Identifier: GPL-3.0-only

#include <cstdio>
#include <cstdint>

#include "../../../event/frame.hpp"
#include "../../../event/tick.hpp"
#include "../../../event/map_load.hpp"
#include "../../../output/draw_text.hpp"
#include "../../../output/output.hpp"
#include "../../../command/command.hpp"

namespace Chimera {
    static std::int64_t counter = 0;

    static void show_timer() noexcept;

    static void increment_counter() noexcept {
        if(get_tick_count() == 0) {
            counter = 0;
        }
        counter++;
    }

    static void reset_counter() noexcept {
        counter = 0;
    }

    bool show_timer_command(int argc, const char **argv) noexcept {
        static bool enabled = false;

        if(argc) {
            const bool new_enabled = STR_TO_BOOL(argv[0]);
            if(new_enabled != enabled) {
                if(new_enabled) {
                    add_preframe_event(show_timer, EventPriority::EVENT_PRIORITY_FINAL);
                    add_pretick_event(increment_counter);
                    add_map_load_event(reset_counter);
                }
                else {
                    remove_preframe_event(show_timer);
                    remove_pretick_event(increment_counter);
                    remove_map_load_event(reset_counter);
                }
                enabled = new_enabled;
            }
        }

        console_output(BOOL_TO_STR(enabled));
        return true;
    }

    static void show_timer() noexcept {
        const auto current_tick = get_tick_count();
        if(counter < current_tick) {
            counter = current_tick;
        }

        constexpr std::int64_t TICKS_PER_SECOND = 30;
        constexpr std::int64_t TICKS_PER_MINUTE = TICKS_PER_SECOND * 60;
        constexpr std::int64_t TICKS_PER_HOUR = TICKS_PER_MINUTE * 60;

        const auto hours = counter / TICKS_PER_HOUR;
        const auto minutes = (counter - hours * TICKS_PER_HOUR) / TICKS_PER_MINUTE;
        const float seconds = static_cast<float>(counter - hours * TICKS_PER_HOUR - minutes * TICKS_PER_MINUTE) /
                              static_cast<float>(TICKS_PER_SECOND);

        char time[64] {};
        std::snprintf(time, sizeof(time), "%02lld:%02lld:%05.2f",
                      static_cast<long long>(hours),
                      static_cast<long long>(minutes),
                      static_cast<double>(seconds));

        static ColorARGB blue = ColorARGB { 0.7, 0.45, 0.72, 1.0 };
        apply_text(time, 170, font_pixel_height(GenericFont::FONT_CONSOLE) + 10, 150,
                   font_pixel_height(GenericFont::FONT_CONSOLE), blue, GenericFont::FONT_CONSOLE,
                   FontAlignment::ALIGN_RIGHT, TextAnchor::ANCHOR_BOTTOM_RIGHT);
    }
}
