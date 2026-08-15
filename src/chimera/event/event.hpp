// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_EVENT_HPP
#define CHIMERA_EVENT_HPP

#include <utility>
#include <vector>

namespace Chimera {
    /**
     * Event order is separated by priority. Events that have the same priority are executed based on a first come, first serve basis.
     */
    enum EventPriority {
        /** These events are called before the default priority events are called. Lua script events fall in this priority bracket. */
        EVENT_PRIORITY_BEFORE,

        /** This is the default priority where most events are called. Chimera events typically fall in this priority bracket. */
        EVENT_PRIORITY_DEFAULT,

        /** These events are called after the default priority and get the last say. Some Chimera events may fall in this priority bracket. */
        EVENT_PRIORITY_AFTER,

        /** These events are called last. This is used for monitoring values and should not be used to change the results. Chimera debug events fall in this priority bracket. */
        EVENT_PRIORITY_FINAL
    };

    /**
     * An event is extra code that can be executed before or after something happens.
     */
    template<typename T> class Event {
    public:
        /** This is the function pointer used by the event. */
        T function = nullptr;

        /** This is the priority of the event. */
        EventPriority priority = EventPriority::EVENT_PRIORITY_DEFAULT;
    };

    /** This is a function typename that has no arguments and returns nothing. */
    using EventFunction = void (*)();

    /**
     * Call an already-snapshotted event list in order without copying it again.
     */
    template<typename T, typename ... Args> static inline void call_in_order_snapshot(const std::vector<Event<T>> &events, Args&& ... args) {
        auto call_events = [&](EventPriority priority) {
            for(const auto &event : events) {
                if(event.priority == priority && event.function) {
                    event.function(std::forward<Args>(args) ...);
                }
            }
        };
        call_events(EVENT_PRIORITY_BEFORE);
        call_events(EVENT_PRIORITY_DEFAULT);
        call_events(EVENT_PRIORITY_AFTER);
        call_events(EVENT_PRIORITY_FINAL);
    }

    /**
     * Call events in order.
     * @param events These are the events being used. A copy is made, allowing events to be added/removed when needed.
     * @param args   These are the arguments to pass to each events' function.
     */
    template<typename T, typename ... Args> static inline void call_in_order(std::vector<Event<T>> events, Args&& ... args) {
        call_in_order_snapshot(events, std::forward<Args>(args) ...);
    }

    /**
     * Reusable snapshot dispatcher for hot event paths.
     *
     * The source event list is still snapshotted before callbacks run, preserving the
     * existing add/remove-during-callback behavior. The snapshot vector retains its
     * capacity between dispatches so steady-state frame/tick paths avoid repeated heap
     * allocations. Recursive dispatch falls back to the normal copy-based path so the
     * active snapshot cannot be overwritten.
     */
    template<typename T> class ReusableEventDispatcher {
    public:
        template<typename ... Args> void dispatch(const std::vector<Event<T>> &events, Args&& ... args) {
            if(this->p_dispatching) {
                call_in_order(events, std::forward<Args>(args) ...);
                return;
            }

            this->p_snapshot.assign(events.begin(), events.end());
            this->p_dispatching = true;
            struct DispatchGuard {
                bool &dispatching;
                ~DispatchGuard() {
                    dispatching = false;
                }
            } guard { this->p_dispatching };

            call_in_order_snapshot(this->p_snapshot, std::forward<Args>(args) ...);
        }

    private:
        std::vector<Event<T>> p_snapshot;
        bool p_dispatching = false;
    };

    /**
     * Call events in order but the event can be denied by any function, preventing further events from firing.
     * @param events These are the events being used. A copy is made, allowing events to be added/removed when needed.
     * @param allow  This is a reference to a boolean to use which may be set to false when denied. If it is already false, no events will be fired.
     * @param args   These are the arguments to pass to each events' function.
     */
    template<typename T, typename ... Args> static inline void call_in_order_allow(std::vector<Event<T>> events, bool &allow, Args&& ... args) {
        auto call_events = [&](const std::vector<Event<T>> &events, bool &allow, EventPriority priority) {
            for(const auto &event : events) {
                if(!allow) {
                    break;
                }
                if(event.priority == priority && event.function) {
                    allow = event.function(std::forward<Args>(args) ...);
                }
            }
        };
        call_events(events, allow, EVENT_PRIORITY_BEFORE);
        call_events(events, allow, EVENT_PRIORITY_DEFAULT);
        call_events(events, allow, EVENT_PRIORITY_AFTER);
        call_events(events, allow, EVENT_PRIORITY_FINAL);
    }
}

#endif
