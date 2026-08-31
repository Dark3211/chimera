// SPDX-License-Identifier: GPL-3.0-only

#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../event/tick.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/structure_bsp.hpp"

extern "C" {
    const void *original_call;
    bool can_update_weather = false;
    void new_weather_update_function();
}

namespace Chimera {
    static constexpr std::uint32_t MAXIMUM_WIND_STATES = 32;

    static void allow_wind_update() noexcept {
        can_update_weather = true;
    }

    extern "C" void meme_up_the_wind_globals() noexcept {
        // Just update these every frame so broken maps with null tag references don't shit the bed.
        StructureBsp *bsp = global_structure_bsp_get();
        const auto weather_count = bsp->weather_palette.count > MAXIMUM_WIND_STATES
            ? MAXIMUM_WIND_STATES
            : bsp->weather_palette.count;

        for(std::uint32_t i = 0; i < weather_count; i++) {
            auto *palette_entry = GET_TAG_BLOCK_ELEMENT(StructureWeatherPaletteEntry, &bsp->weather_palette, i);
            if(palette_entry) {
                wind_globals->wind_states[i].valid = !palette_entry->wind.tag_id.is_null();
            }
        }
        wind_globals->count = static_cast<short>(weather_count);
    }

    void set_up_weather_fix() noexcept {
        static Hook hook;
        write_function_override(get_chimera().get_signature("weather_update_sig").data() + 0x3, hook, reinterpret_cast<const void *>(new_weather_update_function), &original_call);
        add_tick_event(allow_wind_update);
    }
}
