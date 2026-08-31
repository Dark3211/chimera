// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA__EVENT__D3D9_END_SCENE_HPP
#define CHIMERA__EVENT__D3D9_END_SCENE_HPP

#include <d3d9.h>

#include "../event/event.hpp"

namespace Chimera {
    /**
     * This is an event that is triggered around Halo's EndScene call.
     * @param device This is the device.
     */
    using EndSceneEventFunction = void (*)(LPDIRECT3DDEVICE9 device);

    /**
     * Add or replace an EndScene event. This event occurs just before Halo calls EndScene.
     * Use this for rendering that must remain inside Halo's BeginScene/EndScene pair.
     * @param function This is the function to add.
     * @param priority This is the priority used to determine call order.
     */
    void add_d3d9_end_scene_event(const EndSceneEventFunction function, EventPriority priority = EventPriority::EVENT_PRIORITY_DEFAULT);

    /** Remove a pre-EndScene event if the function is being used as an event. */
    void remove_d3d9_end_scene_event(const EndSceneEventFunction function);

    /**
     * Add or replace an event that runs immediately after Halo's EndScene call.
     * This is outside Halo's BeginScene/EndScene pair and before the surrounding
     * render loop continues toward Present. It is intended for operations such as
     * StretchRect and post-processing that are invalid inside a scene.
     * @param function This is the function to add.
     * @param priority This is the priority used to determine call order.
     */
    void add_d3d9_end_scene_after_event(const EndSceneEventFunction function, EventPriority priority = EventPriority::EVENT_PRIORITY_DEFAULT);

    /** Remove a post-EndScene event if the function is being used as an event. */
    void remove_d3d9_end_scene_after_event(const EndSceneEventFunction function);
}

#endif
