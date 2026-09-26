

#ifndef CHIMERA__EVENT__MULTIPLAYER_EVENT_HPP
#define CHIMERA__EVENT__MULTIPLAYER_EVENT_HPP

#include <cstdint>
#include "event.hpp"
#include "../halo_data/player.hpp"

namespace Chimera {
    enum MultiplayerEventType : std::uint32_t {
        MULTIPLAYER_EVENT_FALLING_DEATH = 1,
        MULTIPLAYER_EVENT_GUARDIAN_KILL,
        MULTIPLAYER_EVENT_VEHICLE_KILL,
        MULTIPLAYER_EVENT_PLAYER_KILL,
        MULTIPLAYER_EVENT_BETRAYED,
        MULTIPLAYER_EVENT_SUICIDE,
        MULTIPLAYER_EVENT_LOCAL_DOUBLE_KILL,
        MULTIPLAYER_EVENT_LOCAL_KILLED_PLAYER,
        MULTIPLAYER_EVENT_LOCAL_TRIPLE_KILL,
        MULTIPLAYER_EVENT_LOCAL_KILLTACULAR,
        MULTIPLAYER_EVENT_LOCAL_KILLING_SPREE,
        MULTIPLAYER_EVENT_LOCAL_RUNNING_RIOT
    };

    struct MultiplayerEvent {
        MultiplayerEventType type = MULTIPLAYER_EVENT_FALLING_DEATH;
        PlayerID local_player = HaloID::null_id();
        PlayerID killer_player = HaloID::null_id();
        PlayerID victim_player = HaloID::null_id();
    };

    using MultiplayerEventFunction = void (*)(const MultiplayerEvent &event);

    const char *multiplayer_event_type_name(MultiplayerEventType type) noexcept;

    void add_multiplayer_event(const MultiplayerEventFunction function, EventPriority priority = EVENT_PRIORITY_DEFAULT);
    void remove_multiplayer_event(const MultiplayerEventFunction function);
}

#endif
