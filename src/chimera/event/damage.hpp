// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA__EVENT__DAMAGE_HPP
#define CHIMERA__EVENT__DAMAGE_HPP

#include <cstdint>
#include "../event/event.hpp"
#include "../halo_data/player.hpp"

namespace Chimera {
    struct DamageEventMetadata {
        std::int32_t node_index = -1;
        std::int32_t region_index = -1;
        std::int32_t material_index = -1;
    };

    struct DamageResultEvent {
        ObjectID target_object = HaloID::null_id();
        TagID damage_effect = HaloID::null_id();
        float multiplier = 1.0F;
        PlayerID causing_player = HaloID::null_id();
        ObjectID causing_object = HaloID::null_id();

        std::int32_t node_index = -1;
        std::int32_t region_index = -1;
        std::int32_t material_index = -1;

        bool target_existed_before = false;
        bool target_exists_after = false;
        bool calculation_valid = false;

        float maximum_health = 0.0F;
        float maximum_shields = 0.0F;
        float health_before = 0.0F;
        float health_after = 0.0F;
        float shields_before = 0.0F;
        float shields_after = 0.0F;

        float raw_health_loss = 0.0F;
        float raw_shield_loss = 0.0F;
        float health_loss = 0.0F;
        float shield_loss = 0.0F;
        float health_damage = 0.0F;
        float shield_damage = 0.0F;
        float total_damage = 0.0F;

        bool hit_health = false;
        bool hit_shield = false;
        bool shield_broken = false;
        bool dead_before = false;
        bool dead_after = false;
        bool killed = false;
    };

    /**
     * Get read-only metadata for the damage event currently being dispatched.
     * Values are -1 when the engine did not provide a specific index.
     */
    const DamageEventMetadata &current_damage_event_metadata() noexcept;

    using DamageEventFunction = bool (*)(ObjectID &object, TagID &damage_effect, float &multiplier, PlayerID &causing_player, ObjectID &causing_object);
    using DamageResultEventFunction = void (*)(const DamageResultEvent &result);

    void add_damage_event(const DamageEventFunction function, EventPriority priority = EventPriority::EVENT_PRIORITY_DEFAULT);
    void remove_damage_event(const DamageEventFunction function);

    /**
     * Add/remove a read-only event fired immediately after Halo's original
     * apply-damage routine returns.
     */
    void add_damage_result_event(const DamageResultEventFunction function, EventPriority priority = EventPriority::EVENT_PRIORITY_DEFAULT);
    void remove_damage_result_event(const DamageResultEventFunction function);

    void set_bypass_damage_events(bool bypass) noexcept;
    bool get_bypass_damage_events() noexcept;
}

#endif
