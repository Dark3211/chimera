// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>

#include "index_buffer_fix.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"

namespace Chimera {
    void set_up_index_buffer_fix() noexcept {
        // Halo's native D3D9 path contains an invalid index-buffer Lock range.
        // Chimera normally changes the immediate size from 4 bytes to 8 bytes.
        // D3D9On12 translates resource locks/copies through D3D12 and does not
        // tolerate this workaround the same way; on affected geometry it can
        // result in corrupted indices and long/exploding triangles. Keep the
        // original Halo lock behavior when explicitly testing the 9On12 backend.
        auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
        if(backend && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0)) {
            return;
        }

        if(get_chimera().feature_present("client_invalid_lock")) {
            SigByte mod[] = { 0x08 };
            write_code_s(get_chimera().get_signature("d3d_lock_index_buffer_sig").data() + 6, mod);
        }
    }
}
