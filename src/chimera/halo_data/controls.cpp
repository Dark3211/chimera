// SPDX-License-Identifier: GPL-3.0-only

#include "../signature/signature.hpp"
#include "../chimera.hpp"

#include "controls.hpp"

namespace Chimera {
    ControlsCustomEdition &get_custom_edition_controls() noexcept {
        static auto *controls_table = *reinterpret_cast<ControlsCustomEdition **>(get_chimera().get_signature("controls_sig").data() + 11);
        return *controls_table;
    }

    ControlsRetailDemo &get_retail_demo_controls() noexcept {
        static auto *controls_table = *reinterpret_cast<ControlsRetailDemo **>(get_chimera().get_signature("controls_sig").data() + 11);
        return *controls_table;
    }
}
