

#include <cstdint>
#include <cstring>
#include <vector>

#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "multiplayer_event.hpp"

namespace Chimera {
    static std::vector<Event<MultiplayerEventFunction>> multiplayer_events;
    static std::size_t multiplayer_events_version = 0;
    static ReusableEventDispatcher<MultiplayerEventFunction> multiplayer_event_dispatcher;

    extern "C" void on_multiplayer_event_asm();

    static PlayerID resolve_multiplayer_event_player(std::uint32_t raw_id) noexcept {
        PlayerID id;
        id.whole_id = raw_id;
        if(id.is_null()) {
            return HaloID::null_id();
        }

        auto &players = PlayerTable::get_player_table();
        if(auto *player = players.get_player(id)) {
            return player->get_full_id();
        }

        const auto raw_salt = static_cast<std::uint16_t>(raw_id >> 16);
        const auto raw_index = static_cast<std::uint16_t>(raw_id);
        if(raw_salt != 0 && raw_salt != 0xFFFF) {
            return HaloID::null_id();
        }

        auto *player = players.get_element(raw_index);
        if(!player || player->player_id == 0xFFFF) {
            return HaloID::null_id();
        }
        return player->get_full_id();
    }

    const char *multiplayer_event_type_name(MultiplayerEventType type) noexcept {
        switch(type) {
            case MULTIPLAYER_EVENT_FALLING_DEATH:
                return "falling dead";
            case MULTIPLAYER_EVENT_GUARDIAN_KILL:
                return "guardian kill";
            case MULTIPLAYER_EVENT_VEHICLE_KILL:
                return "vehicle kill";
            case MULTIPLAYER_EVENT_PLAYER_KILL:
                return "player kill";
            case MULTIPLAYER_EVENT_BETRAYED:
                return "betrayed";
            case MULTIPLAYER_EVENT_SUICIDE:
                return "suicide";
            case MULTIPLAYER_EVENT_LOCAL_DOUBLE_KILL:
                return "local double kill";
            case MULTIPLAYER_EVENT_LOCAL_KILLED_PLAYER:
                return "local killed player";
            case MULTIPLAYER_EVENT_LOCAL_TRIPLE_KILL:
                return "local triple kill";
            case MULTIPLAYER_EVENT_LOCAL_KILLTACULAR:
                return "local killtacular";
            case MULTIPLAYER_EVENT_LOCAL_KILLING_SPREE:
                return "local killing spree";
            case MULTIPLAYER_EVENT_LOCAL_RUNNING_RIOT:
                return "local running riot";
            default:
                return "unknown";
        }
    }

    static void enable_multiplayer_event_hook() noexcept {
        static bool attempted = false;
        if(attempted) {
            return;
        }
        attempted = true;

        auto &chimera = get_chimera();
        if(!chimera.feature_present("client_multiplayer_event")) {
            return;
        }

        auto &event_signature = chimera.get_signature("multiplayer_event_sig");
        auto *call_site = event_signature.data();
        const auto *original = event_signature.original_data();
        if(!call_site || !original || event_signature.original_data_size() < 10 ||
           !is_executable_memory_range(call_site, 10)) {
            return;
        }

        if(*reinterpret_cast<const std::uint8_t *>(original + 5) != 0xE8) {
            return;
        }

        std::int32_t displacement = 0;
        std::memcpy(&displacement, original + 6, sizeof(displacement));
        auto *native_event = call_site + 10 + displacement;
        if(!is_executable_memory_range(native_event, 16)) {
            return;
        }

        static Hook hook;
        write_jmp_call(native_event, hook, reinterpret_cast<const void *>(on_multiplayer_event_asm), nullptr, true);
    }

    void add_multiplayer_event(const MultiplayerEventFunction function, EventPriority priority) {
        remove_multiplayer_event(function);
        enable_multiplayer_event_hook();
        multiplayer_events.emplace_back(Event<MultiplayerEventFunction> { function, priority });
        multiplayer_events_version++;
    }

    void remove_multiplayer_event(const MultiplayerEventFunction function) {
        for(std::size_t i = 0; i < multiplayer_events.size(); i++) {
            if(multiplayer_events[i].function == function) {
                multiplayer_events.erase(multiplayer_events.begin() + i);
                multiplayer_events_version++;
                return;
            }
        }
    }

    extern "C" void do_multiplayer_event(std::uint32_t raw_event,
                                           std::uint32_t raw_eax_player,
                                           std::uint32_t raw_ecx_player,
                                           std::uint32_t raw_local_player) noexcept {
        if(raw_event < MULTIPLAYER_EVENT_FALLING_DEATH ||
           raw_event > MULTIPLAYER_EVENT_LOCAL_RUNNING_RIOT) {
            return;
        }

        MultiplayerEvent event;
        event.type = static_cast<MultiplayerEventType>(raw_event);
        event.local_player = resolve_multiplayer_event_player(raw_local_player);

        const auto eax_player = resolve_multiplayer_event_player(raw_eax_player);
        const auto ecx_player = resolve_multiplayer_event_player(raw_ecx_player);
        event.killer_player = HaloID::null_id();
        event.victim_player = HaloID::null_id();

        switch(event.type) {
            case MULTIPLAYER_EVENT_FALLING_DEATH:
            case MULTIPLAYER_EVENT_GUARDIAN_KILL:
            case MULTIPLAYER_EVENT_VEHICLE_KILL:
            case MULTIPLAYER_EVENT_SUICIDE:
                event.victim_player = !ecx_player.is_null() ? ecx_player : eax_player;
                break;
            case MULTIPLAYER_EVENT_PLAYER_KILL:
            case MULTIPLAYER_EVENT_BETRAYED:
                event.killer_player = ecx_player;
                event.victim_player = eax_player;
                break;
            case MULTIPLAYER_EVENT_LOCAL_KILLED_PLAYER:
                event.killer_player = !event.local_player.is_null() ? event.local_player : eax_player;
                event.victim_player = ecx_player;
                break;
            case MULTIPLAYER_EVENT_LOCAL_DOUBLE_KILL:
            case MULTIPLAYER_EVENT_LOCAL_TRIPLE_KILL:
            case MULTIPLAYER_EVENT_LOCAL_KILLTACULAR:
            case MULTIPLAYER_EVENT_LOCAL_KILLING_SPREE:
            case MULTIPLAYER_EVENT_LOCAL_RUNNING_RIOT:
                event.killer_player = event.local_player;
                break;
            default:
                break;
        }

        multiplayer_event_dispatcher.dispatch_versioned(
            multiplayer_events,
            multiplayer_events_version,
            event
        );
    }
}
