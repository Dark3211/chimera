#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>
#include "../../command.hpp"
#include "../../../event/frame.hpp"
#include "../../../event/map_load.hpp"
#include "../../../halo_data/game_engine.hpp"
#include "../../../halo_data/hud_defs.hpp"
#include "../../../halo_data/map.hpp"
#include "../../../halo_data/tag.hpp"
#include "../../../output/output.hpp"

namespace Chimera {
    struct HudColorValue {
        float red = 1.0F;
        float green = 1.0F;
        float blue = 1.0F;
    };

    struct HudColorSlot {
        Pixel32 *address = nullptr;
        Pixel32 original = 0;
        Pixel32 last_written = 0;
        bool active = false;
        bool blocked = false;
    };

    struct HudColorUnitShieldMeter {
        HUDMeterElement meter;
        Pixel32 overcharge_minimum_color;
        Pixel32 overcharge_maximum_color;
        Pixel32 overcharge_flash_color;
        Pixel32 overcharge_empty_color;
        PAD(0x10);
    };

    struct HudColorUnitHealthMeter {
        HUDMeterElement meter;
        Pixel32 medium_health_left_color;
        float maximum_color_health_fraction_cutoff;
        float minimum_color_health_fraction_cutoff;
        PAD(0x14);
    };

    struct HudColorUnitAuxiliaryOverlay {
        HUDPlacement placement;
        TagReference interface_bitmap;
        HUDColor colors;
        std::int16_t sequence_index;
        PAD(0x2);
        TagBlock multitexture_overlays;
        PAD(0x4);
        std::uint16_t type;
        std::uint16_t flags;
        PAD(0x18);
    };

    struct HudColorUnitAuxiliaryPanel {
        PAD(0x14);
        HUDStaticElement background;
        HUDMeterElement meter;
        float minimum_fraction_cutoff;
        std::uint32_t more_flags;
        PAD(0x18);
        PAD(0x40);
    };

    struct HudColorUnitInterface {
        HUDAbsolutePlacement absolute_placement;
        HUDStaticElement hud_background;
        HUDStaticElement shield_panel_background;
        HudColorUnitShieldMeter shield_panel_meter;
        HUDStaticElement health_panel_background;
        HudColorUnitHealthMeter health_panel_meter;
        HUDStaticElement motion_sensor_background;
        HUDStaticElement motion_sensor_foreground;
        PAD(0x20);
        HUDPlacement motion_sensor_center;
        HUDAbsolutePlacement auxiliary_overlay_placement;
        TagBlock overlays;
        PAD(0x10);
        TagBlock sounds;
        TagBlock meters;
        PAD(0x164);
        PAD(0x30);
    };

    struct HudColorGrenadeOverlay {
        HUDPlacement placement;
        HUDColor colors;
        float frame_rate;
        std::int16_t sequence_index;
        std::uint16_t type;
        std::uint32_t flags;
        PAD(0x10);
        PAD(0x28);
    };

    struct HudColorGrenadeInterface {
        HUDAbsolutePlacement absolute_placement;
        HUDStaticElement background;
        HUDStaticElement total_grenades_background;
        HUDNumberElement total_grenades_numbers;
        std::int16_t flash_cutoff;
        PAD(0x2);
        TagReference total_grenades_overlay_bitmap;
        TagBlock overlays;
        TagBlock warning_sounds;
        PAD(0x44);
        HUDIconElement messaging_information;
        PAD(0x30);
    };

    static_assert(sizeof(HudColorUnitShieldMeter) == 0x88);
    static_assert(sizeof(HudColorUnitHealthMeter) == 0x88);
    static_assert(sizeof(HudColorUnitAuxiliaryOverlay) == 0x84);
    static_assert(sizeof(HudColorUnitAuxiliaryPanel) == 0x144);
    static_assert(sizeof(HudColorUnitInterface) == 0x56C);
    static_assert(sizeof(HudColorGrenadeOverlay) == 0x88);
    static_assert(sizeof(HudColorGrenadeInterface) == 0x1F8);

    struct HudColorState {
        Tag *tag_array = nullptr;
        std::uint32_t tag_count = 0;
        TagID scenario_tag = TagID::null_id();
        char map_name[32] = {};
        std::uint32_t map_crc32 = 0;
        bool built = false;
        std::vector<HudColorSlot> slots;
    };

    static HudColorState hud_color_state;
    static std::optional<HudColorValue> hud_color_setting;
    static bool hud_color_auto = false;
    static std::chrono::steady_clock::time_point hud_color_auto_start;
    static std::chrono::steady_clock::time_point hud_color_auto_last_update;

    static bool hud_color_range_valid(const void *address, std::size_t size) noexcept {
        static constexpr std::uintptr_t region_size = 0x1700000;
        if(!address) {
            return false;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(get_tag_data_address());
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        if(base > std::numeric_limits<std::uintptr_t>::max() - region_size) {
            return false;
        }

        const auto end = base + region_size;
        if(start < base || start >= end) {
            return false;
        }

        return size <= end - start;
    }

    static bool hud_color_block_valid(const TagBlock &block, std::size_t element_size) noexcept {
        static constexpr std::size_t region_size = 0x1700000;
        if(element_size == 0 || static_cast<std::size_t>(block.count) > region_size / element_size) {
            return false;
        }
        if(block.count == 0) {
            return true;
        }
        return hud_color_range_valid(block.address, static_cast<std::size_t>(block.count) * element_size);
    }

    template<typename T>
    static T *hud_color_block_element(TagBlock &block, std::uint32_t index) noexcept {
        if(index >= block.count || !hud_color_block_valid(block, sizeof(T))) {
            return nullptr;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(block.address);
        const auto offset = static_cast<std::uintptr_t>(index) * sizeof(T);
        auto *element = reinterpret_cast<T *>(base + offset);
        return hud_color_range_valid(element, sizeof(T)) ? element : nullptr;
    }

    static void hud_color_add(Pixel32 *address) noexcept {
        if(!hud_color_range_valid(address, sizeof(*address))) {
            return;
        }

        for(const auto &slot : hud_color_state.slots) {
            if(slot.address == address) {
                return;
            }
        }

        HudColorSlot slot;
        slot.address = address;
        slot.original = *address;
        hud_color_state.slots.emplace_back(slot);
    }

    static void hud_color_add(HUDColor &colors) noexcept {
        hud_color_add(&colors.color);
        hud_color_add(&colors.flash_color);
        hud_color_add(&colors.disabled_color);
    }

    static void hud_color_add(HUDMeterElement &meter) noexcept {
        hud_color_add(&meter.min_color);
        hud_color_add(&meter.max_color);
        hud_color_add(&meter.flash_color);
        hud_color_add(&meter.empty_color);
        hud_color_add(&meter.disabled_color);
    }

    static void hud_color_collect_weapon(WeaponHUDInterface &hud) noexcept {
        if(hud_color_block_valid(hud.statics, sizeof(WeaponHUDInterfaceStaticElement))) {
            for(std::uint32_t i = 0; i < hud.statics.count; i++) {
                auto *element = hud_color_block_element<WeaponHUDInterfaceStaticElement>(hud.statics, i);
                if(element) {
                    hud_color_add(element->static_element.colors);
                }
            }
        }

        if(hud_color_block_valid(hud.meters, sizeof(WeaponHUDInterfaceMeterElement))) {
            for(std::uint32_t i = 0; i < hud.meters.count; i++) {
                auto *element = hud_color_block_element<WeaponHUDInterfaceMeterElement>(hud.meters, i);
                if(element) {
                    hud_color_add(element->meter_element);
                }
            }
        }

        if(hud_color_block_valid(hud.numbers, sizeof(WeaponHUDInterfaceNumberElement))) {
            for(std::uint32_t i = 0; i < hud.numbers.count; i++) {
                auto *element = hud_color_block_element<WeaponHUDInterfaceNumberElement>(hud.numbers, i);
                if(element) {
                    hud_color_add(element->number_element.colors);
                }
            }
        }

        if(hud_color_block_valid(hud.overlays, sizeof(WeaponHUDInterfaceOverlaysElement))) {
            for(std::uint32_t i = 0; i < hud.overlays.count; i++) {
                auto *element = hud_color_block_element<WeaponHUDInterfaceOverlaysElement>(hud.overlays, i);
                if(!element || !hud_color_block_valid(element->overlays.items, sizeof(WeaponHUDInterfaceOverlayItem))) {
                    continue;
                }

                for(std::uint32_t j = 0; j < element->overlays.items.count; j++) {
                    auto *item = hud_color_block_element<WeaponHUDInterfaceOverlayItem>(element->overlays.items, j);
                    if(item) {
                        hud_color_add(item->colors);
                    }
                }
            }
        }

        hud_color_add(&hud.messaging_icon.color);
    }

    static void hud_color_collect_unit(HudColorUnitInterface &hud) noexcept {
        hud_color_add(hud.hud_background.colors);
        hud_color_add(hud.shield_panel_background.colors);
        hud_color_add(hud.shield_panel_meter.meter);
        hud_color_add(&hud.shield_panel_meter.overcharge_minimum_color);
        hud_color_add(&hud.shield_panel_meter.overcharge_maximum_color);
        hud_color_add(&hud.shield_panel_meter.overcharge_flash_color);
        hud_color_add(&hud.shield_panel_meter.overcharge_empty_color);
        hud_color_add(hud.health_panel_background.colors);
        hud_color_add(hud.health_panel_meter.meter);
        hud_color_add(&hud.health_panel_meter.medium_health_left_color);

        if(hud_color_block_valid(hud.overlays, sizeof(HudColorUnitAuxiliaryOverlay))) {
            for(std::uint32_t i = 0; i < hud.overlays.count; i++) {
                auto *overlay = hud_color_block_element<HudColorUnitAuxiliaryOverlay>(hud.overlays, i);
                if(overlay) {
                    hud_color_add(overlay->colors);
                }
            }
        }

        if(hud_color_block_valid(hud.meters, sizeof(HudColorUnitAuxiliaryPanel))) {
            for(std::uint32_t i = 0; i < hud.meters.count; i++) {
                auto *meter = hud_color_block_element<HudColorUnitAuxiliaryPanel>(hud.meters, i);
                if(meter) {
                    hud_color_add(meter->background.colors);
                    hud_color_add(meter->meter);
                }
            }
        }
    }

    static void hud_color_collect_grenade(HudColorGrenadeInterface &hud) noexcept {
        hud_color_add(hud.background.colors);
        hud_color_add(hud.total_grenades_background.colors);
        hud_color_add(hud.total_grenades_numbers.colors);
        hud_color_add(&hud.messaging_information.color);

        if(hud_color_block_valid(hud.overlays, sizeof(HudColorGrenadeOverlay))) {
            for(std::uint32_t i = 0; i < hud.overlays.count; i++) {
                auto *overlay = hud_color_block_element<HudColorGrenadeOverlay>(hud.overlays, i);
                if(overlay) {
                    hud_color_add(overlay->colors);
                }
            }
        }
    }

    static void hud_color_clear_cache() noexcept {
        hud_color_state.slots.clear();
        hud_color_state.built = false;
    }

    static bool hud_color_identity_changed() noexcept {
        auto &header = get_tag_data_header();
        auto &map_header = get_map_header();
        return hud_color_state.tag_array != header.tag_array ||
               hud_color_state.tag_count != header.tag_count ||
               hud_color_state.scenario_tag != header.scenario_tag ||
               hud_color_state.map_crc32 != map_header.crc32 ||
               std::memcmp(hud_color_state.map_name, map_header.name, sizeof(hud_color_state.map_name)) != 0;
    }

    static void hud_color_update_identity() noexcept {
        auto &header = get_tag_data_header();
        auto &map_header = get_map_header();
        hud_color_state.tag_array = header.tag_array;
        hud_color_state.tag_count = header.tag_count;
        hud_color_state.scenario_tag = header.scenario_tag;
        hud_color_state.map_crc32 = map_header.crc32;
        std::memcpy(hud_color_state.map_name, map_header.name, sizeof(hud_color_state.map_name));
    }

    static void hud_color_collect() noexcept {
        if(hud_color_identity_changed()) {
            hud_color_clear_cache();
            hud_color_update_identity();
        }

        if(hud_color_state.built) {
            return;
        }

        auto &header = get_tag_data_header();
        if(header.tag_count > 0x1700000u / sizeof(Tag)) {
            return;
        }

        for(std::uint32_t i = 0; i < header.tag_count; i++) {
            auto *tag = get_tag(static_cast<std::size_t>(i));
            if(!tag || !tag->data) {
                continue;
            }

            if(tag->primary_class == TagClassInt::TAG_CLASS_WEAPON_HUD_INTERFACE &&
               hud_color_range_valid(tag->data, sizeof(WeaponHUDInterface))) {
                hud_color_collect_weapon(*reinterpret_cast<WeaponHUDInterface *>(tag->data));
            }
            else if(tag->primary_class == TagClassInt::TAG_CLASS_UNIT_HUD_INTERFACE &&
                    hud_color_range_valid(tag->data, sizeof(HudColorUnitInterface))) {
                hud_color_collect_unit(*reinterpret_cast<HudColorUnitInterface *>(tag->data));
            }
            else if(tag->primary_class == TagClassInt::TAG_CLASS_GRENADE_HUD_INTERFACE &&
                    hud_color_range_valid(tag->data, sizeof(HudColorGrenadeInterface))) {
                hud_color_collect_grenade(*reinterpret_cast<HudColorGrenadeInterface *>(tag->data));
            }
        }

        hud_color_state.built = true;
    }

    static Pixel32 hud_color_pack(Pixel32 current, const HudColorValue &color) noexcept {
        const auto red = static_cast<std::uint32_t>(std::lround(color.red * 255.0F));
        const auto green = static_cast<std::uint32_t>(std::lround(color.green * 255.0F));
        const auto blue = static_cast<std::uint32_t>(std::lround(color.blue * 255.0F));
        return (current & 0xFF000000u) | (red << 16) | (green << 8) | blue;
    }

    static void hud_color_apply_value(const HudColorValue &color) noexcept {
        hud_color_collect();

        for(auto &slot : hud_color_state.slots) {
            if(slot.blocked || !hud_color_range_valid(slot.address, sizeof(*slot.address))) {
                continue;
            }

            const auto current = *slot.address;
            if(slot.active && current != slot.last_written) {
                slot.active = false;
                slot.blocked = true;
                continue;
            }

            if(!slot.active) {
                slot.original = current;
            }

            slot.last_written = hud_color_pack(current, color);
            *slot.address = slot.last_written;
            slot.active = true;
        }
    }

    static void hud_color_apply() noexcept {
        if(hud_color_setting.has_value()) {
            hud_color_apply_value(*hud_color_setting);
        }
    }

    static HudColorValue hud_color_from_hue(float hue) noexcept {
        hue -= std::floor(hue);
        const float scaled = hue * 6.0F;
        const int sector = static_cast<int>(scaled);
        const float fraction = scaled - static_cast<float>(sector);

        switch(sector) {
            case 0:
                return {1.0F, fraction, 0.0F};
            case 1:
                return {1.0F - fraction, 1.0F, 0.0F};
            case 2:
                return {0.0F, 1.0F, fraction};
            case 3:
                return {0.0F, 1.0F - fraction, 1.0F};
            case 4:
                return {fraction, 0.0F, 1.0F};
            default:
                return {1.0F, 0.0F, 1.0F - fraction};
        }
    }

    static void hud_color_auto_frame() noexcept {
        if(!hud_color_auto) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if(hud_color_auto_last_update.time_since_epoch().count() != 0 &&
           now - hud_color_auto_last_update < std::chrono::milliseconds(33)) {
            return;
        }

        hud_color_auto_last_update = now;
        const auto elapsed = std::chrono::duration<float>(now - hud_color_auto_start).count();
        const float hue = std::fmod(elapsed / 6.0F, 1.0F);
        hud_color_apply_value(hud_color_from_hue(hue));
    }

    static void hud_color_restore() noexcept {
        if(hud_color_identity_changed()) {
            hud_color_clear_cache();
            hud_color_update_identity();
            return;
        }

        for(auto &slot : hud_color_state.slots) {
            if(!slot.active) {
                continue;
            }

            if(hud_color_range_valid(slot.address, sizeof(*slot.address)) &&
               *slot.address == slot.last_written) {
                *slot.address = slot.original;
            }

            slot.active = false;
        }
    }

    static void hud_color_map_load() noexcept {
        hud_color_clear_cache();
        hud_color_update_identity();
        if(hud_color_auto) {
            hud_color_auto_frame();
        }
        else {
            hud_color_apply();
        }
    }

    static bool hud_color_parse_component(const char *text, float &value) noexcept {
        if(!text) {
            return false;
        }

        errno = 0;
        char *end = nullptr;
        value = std::strtof(text, &end);
        return errno != ERANGE &&
               end != text &&
               end &&
               *end == 0 &&
               std::isfinite(value) &&
               value >= 0.0F &&
               value <= 1.0F;
    }

    bool hud_color_command(int argc, const char **argv) {
        if(game_engine() != GameEngine::GAME_ENGINE_CUSTOM_EDITION &&
           game_engine() != GameEngine::GAME_ENGINE_RETAIL) {
            return false;
        }

        if(argc == 1) {
            if(std::strcmp(argv[0], "off") == 0) {
                hud_color_restore();
                hud_color_setting.reset();
                hud_color_auto = false;
                remove_frame_event(hud_color_auto_frame);
                remove_map_load_event(hud_color_map_load);
            }
            else if(std::strcmp(argv[0], "auto") == 0) {
                hud_color_setting.reset();
                hud_color_auto = true;
                hud_color_auto_start = std::chrono::steady_clock::now();
                hud_color_auto_last_update = {};
                add_map_load_event(hud_color_map_load);
                add_frame_event(hud_color_auto_frame);
                hud_color_auto_frame();
            }
            else {
                console_error("Expected: off, auto, or red green blue");
                return false;
            }
        }
        else if(argc == 3) {
            HudColorValue color;
            if(!hud_color_parse_component(argv[0], color.red) ||
               !hud_color_parse_component(argv[1], color.green) ||
               !hud_color_parse_component(argv[2], color.blue)) {
                console_error("HUD color values must be finite numbers from 0 to 1.");
                return false;
            }

            hud_color_auto = false;
            remove_frame_event(hud_color_auto_frame);
            hud_color_setting = color;
            add_map_load_event(hud_color_map_load);
            hud_color_apply();
        }
        else if(argc != 0) {
            console_error("Expected: off, auto, or red green blue");
            return false;
        }

        if(hud_color_auto) {
            console_output("auto");
        }
        else if(hud_color_setting.has_value()) {
            console_output("%.3f %.3f %.3f",
                           hud_color_setting->red,
                           hud_color_setting->green,
                           hud_color_setting->blue);
        }
        else {
            console_output("off");
        }

        return true;
    }
}
