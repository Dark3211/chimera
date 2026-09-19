// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <cmath>
#include <cstdint>

#include "damage.hpp"
#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../halo_data/damage.hpp"
#include "../halo_data/object.hpp"

namespace Chimera {
    static std::vector<Event<DamageEventFunction>> damage_events;
    static std::size_t damage_events_version = 0;
    static ReusableEventDispatcher<DamageEventFunction> damage_dispatcher;

    static std::vector<Event<DamageResultEventFunction>> damage_result_events;
    static std::size_t damage_result_events_version = 0;
    static ReusableEventDispatcher<DamageResultEventFunction> damage_result_dispatcher;

    static DamageEventMetadata damage_event_metadata;

    struct DamageVitalitySnapshot {
        bool valid = false;
        float maximum_health = 0.0F;
        float maximum_shields = 0.0F;
        float health = 0.0F;
        float shields = 0.0F;
        bool dead = false;
    };

    struct DamageResultContext {
        std::uintptr_t return_address = 0;
        ObjectID target_object = HaloID::null_id();
        TagID damage_effect = HaloID::null_id();
        float multiplier = 1.0F;
        PlayerID causing_player = HaloID::null_id();
        ObjectID causing_object = HaloID::null_id();
        std::int32_t node_index = -1;
        std::int32_t region_index = -1;
        std::int32_t material_index = -1;
        DamageVitalitySnapshot before;
    };

    static constexpr std::size_t MAXIMUM_NESTED_DAMAGE_RESULTS = 64;
    static std::array<DamageResultContext, MAXIMUM_NESTED_DAMAGE_RESULTS> damage_result_stack;
    static std::size_t damage_result_depth = 0;

    const DamageEventMetadata &current_damage_event_metadata() noexcept {
        return damage_event_metadata;
    }

    static void enable_damage_hook();

    static bool should_bypass = false;
    void set_bypass_damage_events(bool bypass) noexcept {
        should_bypass = bypass;
    }
    bool get_bypass_damage_events() noexcept {
        return should_bypass;
    }

    static DamageVitalitySnapshot read_vitality(ObjectID object_id) noexcept {
        DamageVitalitySnapshot snapshot;
        auto *object = ObjectTable::get_object_table().get_dynamic_object(object_id);
        if(!object) {
            return snapshot;
        }

        const auto &datum = object->object;
        const bool finite =
            std::isfinite(datum.maximum_body_vitality) &&
            std::isfinite(datum.maximum_shield_vitality) &&
            std::isfinite(datum.body_vitality) &&
            std::isfinite(datum.shield_vitality);

        if(!finite) {
            return snapshot;
        }

        snapshot.valid = true;
        snapshot.maximum_health = datum.maximum_body_vitality;
        snapshot.maximum_shields = datum.maximum_shield_vitality;
        snapshot.health = datum.body_vitality;
        snapshot.shields = datum.shield_vitality;
        snapshot.dead = (datum.damage_flags & (1u << OBJECT_DAMAGE_FLAGS_DEAD_BIT)) != 0;
        return snapshot;
    }

    extern "C" {
        void on_damage_asm();
        const void *do_continue_damage_effect;
    }

    void add_damage_event(const DamageEventFunction function, EventPriority priority) {
        remove_damage_event(function);
        enable_damage_hook();
        damage_events.emplace_back(Event<DamageEventFunction> { function, priority });
        damage_events_version++;
    }

    void remove_damage_event(const DamageEventFunction function) {
        for(std::size_t i = 0; i < damage_events.size(); i++) {
            if(damage_events[i].function == function) {
                damage_events.erase(damage_events.begin() + i);
                damage_events_version++;
                return;
            }
        }
    }

    void add_damage_result_event(const DamageResultEventFunction function, EventPriority priority) {
        remove_damage_result_event(function);
        enable_damage_hook();
        damage_result_events.emplace_back(Event<DamageResultEventFunction> { function, priority });
        damage_result_events_version++;
    }

    void remove_damage_result_event(const DamageResultEventFunction function) {
        for(std::size_t i = 0; i < damage_result_events.size(); i++) {
            if(damage_result_events[i].function == function) {
                damage_result_events.erase(damage_result_events.begin() + i);
                damage_result_events_version++;
                return;
            }
        }
    }

    /**
     * Return values consumed by damage.S:
     * 0 = deny damage
     * 1 = run Halo and return through the post-damage wrapper
     * 2 = run Halo directly (no post-damage listener/context)
     */
    extern "C" std::uint8_t do_damage_event(DamageObjectStructThing *damage_thing,
                                             ObjectID *object,
                                             std::int32_t node_index,
                                             std::int32_t region_index,
                                             std::int32_t material_index,
                                             std::uintptr_t return_address) {
        if(should_bypass) {
            return 2;
        }

        const auto previous_metadata = damage_event_metadata;
        damage_event_metadata.node_index = node_index;
        damage_event_metadata.region_index = region_index;
        damage_event_metadata.material_index = material_index;

        bool allow = true;
        damage_dispatcher.dispatch_allow_versioned(
            damage_events,
            damage_events_version,
            allow,
            *object,
            damage_thing->damage_tag_id,
            damage_thing->multiplier,
            damage_thing->causer_player,
            damage_thing->causer_object
        );

        damage_event_metadata = previous_metadata;

        if(!allow) {
            return 0;
        }

        if(damage_result_events.empty() || damage_result_depth >= MAXIMUM_NESTED_DAMAGE_RESULTS) {
            return 2;
        }

        auto &context = damage_result_stack[damage_result_depth++];
        context.return_address = return_address;
        context.target_object = *object;
        context.damage_effect = damage_thing->damage_tag_id;
        context.multiplier = damage_thing->multiplier;
        context.causing_player = damage_thing->causer_player;
        context.causing_object = damage_thing->causer_object;
        context.node_index = node_index;
        context.region_index = region_index;
        context.material_index = material_index;
        context.before = read_vitality(*object);

        return 1;
    }

    extern "C" std::uintptr_t do_damage_result_event() noexcept {
        if(damage_result_depth == 0) {
            return 0;
        }

        const auto context = damage_result_stack[--damage_result_depth];

        DamageResultEvent result;
        result.target_object = context.target_object;
        result.damage_effect = context.damage_effect;
        result.multiplier = context.multiplier;
        result.causing_player = context.causing_player;
        result.causing_object = context.causing_object;
        result.node_index = context.node_index;
        result.region_index = context.region_index;
        result.material_index = context.material_index;

        const auto after = read_vitality(context.target_object);
        result.target_existed_before = context.before.valid;
        result.target_exists_after = after.valid;

        if(context.before.valid) {
            result.maximum_health = context.before.maximum_health;
            result.maximum_shields = context.before.maximum_shields;
            result.health_before = context.before.health;
            result.shields_before = context.before.shields;
            result.dead_before = context.before.dead;
        }

        if(after.valid) {
            result.health_after = after.health;
            result.shields_after = after.shields;
            result.dead_after = after.dead;
        }

        if(context.before.valid && after.valid) {
            result.calculation_valid = true;
            result.raw_health_loss = context.before.health - after.health;
            result.raw_shield_loss = context.before.shields - after.shields;

            // Halo can drive vitality below zero (for example, corpse damage or
            // overkill). Damage numbers should describe vitality that actually
            // existed, while the raw before/after values remain available.
            const float effective_health_before = context.before.health > 0.0F ? context.before.health : 0.0F;
            const float effective_health_after = after.health > 0.0F ? after.health : 0.0F;
            const float effective_shield_before = context.before.shields > 0.0F ? context.before.shields : 0.0F;
            const float effective_shield_after = after.shields > 0.0F ? after.shields : 0.0F;

            const float effective_health_loss = effective_health_before - effective_health_after;
            const float effective_shield_loss = effective_shield_before - effective_shield_after;
            result.health_loss = effective_health_loss > 0.0F ? effective_health_loss : 0.0F;
            result.shield_loss = effective_shield_loss > 0.0F ? effective_shield_loss : 0.0F;

            if(context.before.maximum_health > 0.0F) {
                result.health_damage = result.health_loss * context.before.maximum_health;
            }
            if(context.before.maximum_shields > 0.0F) {
                result.shield_damage = result.shield_loss * context.before.maximum_shields;
            }

            result.total_damage = result.health_damage + result.shield_damage;

            static constexpr float VITALITY_EPSILON = 0.000001F;
            result.hit_health = result.health_loss > VITALITY_EPSILON;
            result.hit_shield = result.shield_loss > VITALITY_EPSILON;
            result.shield_broken =
                result.hit_shield &&
                effective_shield_before > VITALITY_EPSILON &&
                effective_shield_after <= VITALITY_EPSILON;
            result.killed = !context.before.dead && after.dead;
        }

        damage_result_dispatcher.dispatch_versioned(
            damage_result_events,
            damage_result_events_version,
            result
        );

        return context.return_address;
    }

    static void enable_damage_hook() {
        static bool enabled = false;
        if(enabled) {
            return;
        }
        enabled = true;

        static Hook hook;
        write_function_override(
            get_chimera().get_signature("apply_damage_sig").data(),
            hook,
            reinterpret_cast<const void *>(on_damage_asm),
            &do_continue_damage_effect
        );
    }
}
