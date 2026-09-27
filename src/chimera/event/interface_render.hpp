

#ifndef CHIMERA_INTERFACE_RENDER_HPP
#define CHIMERA_INTERFACE_RENDER_HPP

#include "event.hpp"

namespace Chimera {
    using InterfaceRenderEventFunction = void (*)(bool after);

    bool hud_render_event_supported() noexcept;
    bool ui_render_event_supported() noexcept;

    const void *hud_render_event_call_site() noexcept;
    const void *ui_render_event_call_site() noexcept;

    bool add_hud_render_event(InterfaceRenderEventFunction function,
                              EventPriority priority = EVENT_PRIORITY_DEFAULT) noexcept;
    void remove_hud_render_event(InterfaceRenderEventFunction function) noexcept;

    bool add_ui_render_event(InterfaceRenderEventFunction function,
                             EventPriority priority = EVENT_PRIORITY_DEFAULT) noexcept;
    void remove_ui_render_event(InterfaceRenderEventFunction function) noexcept;
}

#endif
