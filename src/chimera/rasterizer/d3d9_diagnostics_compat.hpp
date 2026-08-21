// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_DIAGNOSTICS_COMPAT_HPP
#define CHIMERA_D3D9_DIAGNOSTICS_COMPAT_HPP

#include <windows.h>
#include <d3d9.h>

#include <cstddef>

namespace Chimera {
    namespace D3D9Diagnostics {
        // MinGW treats a pointer to a noexcept hook as a distinct type from the
        // corresponding COM function-pointer typedef. The main diagnostic helper
        // intentionally keeps the original COM typedef for calling convention
        // fidelity, so provide a two-type overload for vtable installation.
        template<typename Function, typename ReplacementFunction>
        static bool patch_device_function(IDirect3DDevice9 *device,
                                          std::size_t index,
                                          ReplacementFunction replacement,
                                          Function &original) noexcept {
            if(!device) {
                return false;
            }

            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) {
                return false;
            }

            auto *entry = &vtable[index];
            const ULONG_PTR replacement_address = reinterpret_cast<ULONG_PTR>(replacement);
            if(*entry == replacement_address) {
                return original != nullptr;
            }

            original = reinterpret_cast<Function>(*entry);
            if(!original) {
                return false;
            }

            DWORD old_protection = 0;
            if(!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE, &old_protection)) {
                original = nullptr;
                return false;
            }

            *entry = replacement_address;
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(*entry), old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(*entry));
            return true;
        }
    }
}

#endif
