// SPDX-License-Identifier: GPL-3.0-only

#include "startup_trace.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "config/ini.hpp"
#include "signature/hook.hpp"

namespace Chimera {
    namespace {
        using Direct3DCreate9Function = IDirect3D9 *(WINAPI *)(UINT);
        using CreateDeviceFunction = HRESULT (WINAPI *)(
            IDirect3D9 *,
            UINT,
            D3DDEVTYPE,
            HWND,
            DWORD,
            D3DPRESENT_PARAMETERS *,
            IDirect3DDevice9 **
        );
        using DeviceResetFunction = HRESULT (WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
        using DevicePresentFunction = HRESULT (WINAPI *)(
            IDirect3DDevice9 *,
            const RECT *,
            const RECT *,
            HWND,
            const RGNDATA *
        );
        using ChangeDisplaySettingsAFunction = LONG (WINAPI *)(DEVMODEA *, DWORD);
        using ChangeDisplaySettingsWFunction = LONG (WINAPI *)(DEVMODEW *, DWORD);
        using ChangeDisplaySettingsExAFunction = LONG (WINAPI *)(LPCSTR, DEVMODEA *, HWND, DWORD, LPVOID);
        using ChangeDisplaySettingsExWFunction = LONG (WINAPI *)(LPCWSTR, DEVMODEW *, HWND, DWORD, LPVOID);

        static bool trace_enabled = false;
        static bool trace_initialized = false;
        static HANDLE trace_file = INVALID_HANDLE_VALUE;
        static char trace_file_path[MAX_PATH] {};
        static LONG trace_lock = 0;
        static LARGE_INTEGER trace_start {};
        static LARGE_INTEGER trace_frequency {};

        static Hook direct3d_create9_hook;
        static Hook change_display_settings_a_hook;
        static Hook change_display_settings_w_hook;
        static Hook change_display_settings_ex_a_hook;
        static Hook change_display_settings_ex_w_hook;

        static Direct3DCreate9Function original_direct3d_create9 = nullptr;
        static ChangeDisplaySettingsAFunction original_change_display_settings_a = nullptr;
        static ChangeDisplaySettingsWFunction original_change_display_settings_w = nullptr;
        static ChangeDisplaySettingsExAFunction original_change_display_settings_ex_a = nullptr;
        static ChangeDisplaySettingsExWFunction original_change_display_settings_ex_w = nullptr;

        static CreateDeviceFunction original_create_device = nullptr;
        static DeviceResetFunction original_device_reset = nullptr;
        static DevicePresentFunction original_device_present = nullptr;

        static void log_current_display_mode(const char *label) noexcept {
            DEVMODEA mode {};
            mode.dmSize = sizeof(mode);
            if(EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
                startup_trace_log(
                    "%s: desktop=%lux%lu %luHz %lu-bpp position=(%ld,%ld)",
                    label,
                    static_cast<unsigned long>(mode.dmPelsWidth),
                    static_cast<unsigned long>(mode.dmPelsHeight),
                    static_cast<unsigned long>(mode.dmDisplayFrequency),
                    static_cast<unsigned long>(mode.dmBitsPerPel),
                    static_cast<long>(mode.dmPosition.x),
                    static_cast<long>(mode.dmPosition.y)
                );
            }
            else {
                startup_trace_log("%s: EnumDisplaySettings(ENUM_CURRENT_SETTINGS) failed, Win32=%lu",
                                  label, static_cast<unsigned long>(GetLastError()));
            }
        }

        static void log_window(const char *label, HWND window) noexcept {
            if(!window || !IsWindow(window)) {
                startup_trace_log("%s: hwnd=%p (invalid or null)", label, static_cast<void *>(window));
                return;
            }

            RECT outer {};
            RECT client {};
            GetWindowRect(window, &outer);
            GetClientRect(window, &client);
            const auto style = GetWindowLongPtrA(window, GWL_STYLE);
            const auto ex_style = GetWindowLongPtrA(window, GWL_EXSTYLE);

            char title[160] {};
            GetWindowTextA(window, title, static_cast<int>(sizeof(title)));

            startup_trace_log(
                "%s: hwnd=%p title=\"%s\" outer=%ldx%ld at (%ld,%ld) client=%ldx%ld style=0x%08lx ex=0x%08lx",
                label,
                static_cast<void *>(window),
                title,
                static_cast<long>(outer.right - outer.left),
                static_cast<long>(outer.bottom - outer.top),
                static_cast<long>(outer.left),
                static_cast<long>(outer.top),
                static_cast<long>(client.right - client.left),
                static_cast<long>(client.bottom - client.top),
                static_cast<unsigned long>(style),
                static_cast<unsigned long>(ex_style)
            );
        }

        static void log_present_parameters(const char *label, const D3DPRESENT_PARAMETERS *params) noexcept {
            if(!params) {
                startup_trace_log("%s: D3DPRESENT_PARAMETERS=null", label);
                return;
            }

            startup_trace_log(
                "%s: Windowed=%u BackBuffer=%lux%lu Format=%u Count=%lu MultiSample=%u Quality=%lu SwapEffect=%u hDeviceWindow=%p Refresh=%lu PresentationInterval=0x%08lx Flags=0x%08lx AutoDepth=%u DepthFormat=%u",
                label,
                static_cast<unsigned int>(params->Windowed),
                static_cast<unsigned long>(params->BackBufferWidth),
                static_cast<unsigned long>(params->BackBufferHeight),
                static_cast<unsigned int>(params->BackBufferFormat),
                static_cast<unsigned long>(params->BackBufferCount),
                static_cast<unsigned int>(params->MultiSampleType),
                static_cast<unsigned long>(params->MultiSampleQuality),
                static_cast<unsigned int>(params->SwapEffect),
                static_cast<void *>(params->hDeviceWindow),
                static_cast<unsigned long>(params->FullScreen_RefreshRateInHz),
                static_cast<unsigned long>(params->PresentationInterval),
                static_cast<unsigned long>(params->Flags),
                static_cast<unsigned int>(params->EnableAutoDepthStencil),
                static_cast<unsigned int>(params->AutoDepthStencilFormat)
            );
            log_window("D3D presentation window", params->hDeviceWindow);
        }

        static void log_display_mode_a(const char *label, const DEVMODEA *mode) noexcept {
            if(!mode) {
                startup_trace_log("%s: DEVMODE=null (request default/current mode)", label);
                return;
            }
            startup_trace_log(
                "%s: requested=%lux%lu %luHz %lu-bpp fields=0x%08lx position=(%ld,%ld)",
                label,
                static_cast<unsigned long>(mode->dmPelsWidth),
                static_cast<unsigned long>(mode->dmPelsHeight),
                static_cast<unsigned long>(mode->dmDisplayFrequency),
                static_cast<unsigned long>(mode->dmBitsPerPel),
                static_cast<unsigned long>(mode->dmFields),
                static_cast<long>(mode->dmPosition.x),
                static_cast<long>(mode->dmPosition.y)
            );
        }

        static void log_display_mode_w(const char *label, const DEVMODEW *mode) noexcept {
            if(!mode) {
                startup_trace_log("%s: DEVMODE=null (request default/current mode)", label);
                return;
            }
            startup_trace_log(
                "%s: requested=%lux%lu %luHz %lu-bpp fields=0x%08lx position=(%ld,%ld)",
                label,
                static_cast<unsigned long>(mode->dmPelsWidth),
                static_cast<unsigned long>(mode->dmPelsHeight),
                static_cast<unsigned long>(mode->dmDisplayFrequency),
                static_cast<unsigned long>(mode->dmBitsPerPel),
                static_cast<unsigned long>(mode->dmFields),
                static_cast<long>(mode->dmPosition.x),
                static_cast<long>(mode->dmPosition.y)
            );
        }

        static bool patch_vtable_entry(
            void **vtable,
            std::size_t index,
            const void *replacement,
            const void **original
        ) noexcept {
            if(!vtable || !replacement || !original) {
                return false;
            }

            if(vtable[index] == replacement) {
                return true;
            }

            DWORD old_protection = 0;
            if(!VirtualProtect(&vtable[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protection)) {
                startup_trace_log(
                    "VTable patch failed at %p[%zu], Win32=%lu",
                    static_cast<void *>(vtable),
                    index,
                    static_cast<unsigned long>(GetLastError())
                );
                return false;
            }

            *original = vtable[index];
            vtable[index] = const_cast<void *>(replacement);

            DWORD ignored = 0;
            VirtualProtect(&vtable[index], sizeof(void *), old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &vtable[index], sizeof(void *));
            return true;
        }

        static HRESULT WINAPI traced_device_reset(
            IDirect3DDevice9 *device,
            D3DPRESENT_PARAMETERS *params
        ) {
            static LONG reset_count = 0;
            const LONG number = InterlockedIncrement(&reset_count);
            startup_trace_log("D3D9 Reset #%ld BEGIN device=%p", static_cast<long>(number), static_cast<void *>(device));
            log_present_parameters("Reset input", params);
            log_current_display_mode("Before Reset");

            if(!original_device_reset) {
                startup_trace_log("D3D9 Reset #%ld ABORT: original function pointer is null", static_cast<long>(number));
                return D3DERR_INVALIDCALL;
            }

            const HRESULT result = original_device_reset(device, params);
            startup_trace_log("D3D9 Reset #%ld END HRESULT=0x%08lx", static_cast<long>(number),
                              static_cast<unsigned long>(result));
            log_present_parameters("Reset output", params);
            log_current_display_mode("After Reset");
            return result;
        }

        static HRESULT WINAPI traced_device_present(
            IDirect3DDevice9 *device,
            const RECT *source_rect,
            const RECT *destination_rect,
            HWND destination_window,
            const RGNDATA *dirty_region
        ) {
            if(!original_device_present) {
                startup_trace_log("D3D9 Present ABORT: original function pointer is null");
                return D3DERR_INVALIDCALL;
            }

            const HRESULT result = original_device_present(
                device,
                source_rect,
                destination_rect,
                destination_window,
                dirty_region
            );

            static LONG present_count = 0;
            static ULONGLONG last_summary_tick = 0;
            static ULONGLONG last_failure_tick = 0;
            static HRESULT last_failure = S_OK;
            const LONG number = InterlockedIncrement(&present_count);
            const ULONGLONG now = GetTickCount64();
            const bool new_failure = FAILED(result) &&
                (result != last_failure || now - last_failure_tick >= 1000);
            if(new_failure) {
                last_failure = result;
                last_failure_tick = now;
            }
            if(number <= 8 || new_failure || now - last_summary_tick >= 1000) {
                startup_trace_log(
                    "D3D9 Present #%ld HRESULT=0x%08lx destination-window=%p",
                    static_cast<long>(number),
                    static_cast<unsigned long>(result),
                    static_cast<void *>(destination_window)
                );
                last_summary_tick = now;
            }
            return result;
        }

        static void install_device_reset_hook_real(IDirect3DDevice9 *device) noexcept {
            if(!device) {
                return;
            }

            auto **vtable = *reinterpret_cast<void ***>(device);
            if(!vtable) {
                return;
            }

            if(vtable[16] == reinterpret_cast<void *>(traced_device_reset)) {
                return;
            }

            const void *original = nullptr;
            if(!patch_vtable_entry(
                vtable,
                16, // IDirect3DDevice9::Reset
                reinterpret_cast<const void *>(traced_device_reset),
                &original
            )) {
                return;
            }

            if(!original_device_reset) {
                static_assert(sizeof(original_device_reset) == sizeof(original));
                std::memcpy(&original_device_reset, &original, sizeof(original_device_reset));
            }
            startup_trace_log("IDirect3DDevice9::Reset hook installed: device=%p vtable=%p original=%p",
                              static_cast<void *>(device),
                              static_cast<void *>(vtable),
                              const_cast<void *>(original));

            if(vtable[17] != reinterpret_cast<void *>(traced_device_present)) {
                const void *present_original = nullptr;
                if(patch_vtable_entry(
                    vtable,
                    17, // IDirect3DDevice9::Present
                    reinterpret_cast<const void *>(traced_device_present),
                    &present_original
                )) {
                    if(!original_device_present) {
                        static_assert(sizeof(original_device_present) == sizeof(present_original));
                        std::memcpy(&original_device_present, &present_original, sizeof(original_device_present));
                    }
                    startup_trace_log("IDirect3DDevice9::Present hook installed: device=%p original=%p",
                                      static_cast<void *>(device),
                                      const_cast<void *>(present_original));
                }
                else {
                    startup_trace_log("IDirect3DDevice9::Present hook could not be installed");
                }
            }
        }

        static HRESULT WINAPI traced_create_device(
            IDirect3D9 *d3d,
            UINT adapter,
            D3DDEVTYPE device_type,
            HWND focus_window,
            DWORD behavior_flags,
            D3DPRESENT_PARAMETERS *params,
            IDirect3DDevice9 **returned_device
        ) {
            static LONG create_count = 0;
            const LONG number = InterlockedIncrement(&create_count);
            startup_trace_log(
                "D3D9 CreateDevice #%ld BEGIN d3d=%p adapter=%u type=%u focus=%p behavior=0x%08lx",
                static_cast<long>(number),
                static_cast<void *>(d3d),
                adapter,
                static_cast<unsigned int>(device_type),
                static_cast<void *>(focus_window),
                static_cast<unsigned long>(behavior_flags)
            );
            log_present_parameters("CreateDevice input", params);
            log_current_display_mode("Before CreateDevice");
            log_window("CreateDevice focus window", focus_window);

            if(!original_create_device) {
                startup_trace_log("D3D9 CreateDevice #%ld ABORT: original function pointer is null",
                                  static_cast<long>(number));
                return D3DERR_INVALIDCALL;
            }

            const HRESULT result = original_create_device(
                d3d,
                adapter,
                device_type,
                focus_window,
                behavior_flags,
                params,
                returned_device
            );

            IDirect3DDevice9 *device = returned_device ? *returned_device : nullptr;
            startup_trace_log("D3D9 CreateDevice #%ld END HRESULT=0x%08lx device=%p",
                              static_cast<long>(number),
                              static_cast<unsigned long>(result),
                              static_cast<void *>(device));
            log_present_parameters("CreateDevice output", params);
            log_current_display_mode("After CreateDevice");

            if(SUCCEEDED(result) && device) {
                install_device_reset_hook_real(device);
            }
            return result;
        }

        static void install_create_device_hook(IDirect3D9 *d3d) noexcept {
            if(!d3d) {
                return;
            }

            auto **vtable = *reinterpret_cast<void ***>(d3d);
            if(!vtable) {
                startup_trace_log("IDirect3D9 vtable is null; CreateDevice hook not installed");
                return;
            }
            if(vtable[16] == reinterpret_cast<void *>(traced_create_device)) {
                return;
            }

            const void *original = nullptr;
            if(!patch_vtable_entry(
                vtable,
                16, // IDirect3D9::CreateDevice
                reinterpret_cast<const void *>(traced_create_device),
                &original
            )) {
                startup_trace_log("Could not install IDirect3D9::CreateDevice hook");
                return;
            }

            if(!original_create_device) {
                static_assert(sizeof(original_create_device) == sizeof(original));
                std::memcpy(&original_create_device, &original, sizeof(original_create_device));
            }
            startup_trace_log("IDirect3D9::CreateDevice hook installed: interface=%p vtable=%p original=%p",
                              static_cast<void *>(d3d),
                              static_cast<void *>(vtable),
                              const_cast<void *>(original));
        }

        static IDirect3D9 *WINAPI traced_direct3d_create9(UINT sdk_version) {
            static LONG create9_count = 0;
            const LONG number = InterlockedIncrement(&create9_count);
            startup_trace_log("Direct3DCreate9 #%ld BEGIN SDK=%u", static_cast<long>(number), sdk_version);

            if(!original_direct3d_create9) {
                startup_trace_log("Direct3DCreate9 #%ld ABORT: original function pointer is null",
                                  static_cast<long>(number));
                return nullptr;
            }

            IDirect3D9 *d3d = original_direct3d_create9(sdk_version);
            startup_trace_log("Direct3DCreate9 #%ld END interface=%p", static_cast<long>(number), static_cast<void *>(d3d));
            if(d3d) {
                D3DDISPLAYMODE mode {};
                if(SUCCEEDED(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode))) {
                    startup_trace_log(
                        "Direct3D9 adapter display mode=%ux%u %uHz format=%u",
                        mode.Width,
                        mode.Height,
                        mode.RefreshRate,
                        static_cast<unsigned int>(mode.Format)
                    );
                }
                install_create_device_hook(d3d);
            }
            return d3d;
        }

        static bool install_export_hook(
            const char *module_name,
            const char *function_name,
            Hook &hook,
            const void *replacement,
            const void **original
        ) noexcept {
            HMODULE module = GetModuleHandleA(module_name);
            if(!module) {
                startup_trace_log("Hook unavailable: module %s is not loaded (function %s)",
                                  module_name, function_name);
                return false;
            }

            FARPROC address = GetProcAddress(module, function_name);
            if(!address) {
                startup_trace_log("Hook unavailable: %s!%s not exported, Win32=%lu",
                                  module_name, function_name, static_cast<unsigned long>(GetLastError()));
                return false;
            }

            write_function_override(
                reinterpret_cast<void *>(address),
                hook,
                replacement,
                original
            );
            if(!hook.address || !hook.hook || !original || !*original) {
                startup_trace_log("Hook installation failed: %s!%s", module_name, function_name);
                return false;
            }
            startup_trace_log("Hook installed: %s!%s at %p", module_name, function_name,
                              reinterpret_cast<void *>(address));
            return true;
        }

        static bool install_main_executable_iat_hook(
            const void *target,
            const void *replacement,
            const void **original
        ) noexcept {
            if(!target || !replacement || !original) {
                return false;
            }

            auto *module_base = reinterpret_cast<std::byte *>(GetModuleHandleA(nullptr));
            if(!module_base) {
                startup_trace_log("D3D9 IAT fallback: main executable module handle is null");
                return false;
            }

            auto *dos_header = reinterpret_cast<IMAGE_DOS_HEADER *>(module_base);
            if(dos_header->e_magic != IMAGE_DOS_SIGNATURE || dos_header->e_lfanew <= 0) {
                startup_trace_log("D3D9 IAT fallback: invalid DOS header");
                return false;
            }

            auto *nt_headers = reinterpret_cast<IMAGE_NT_HEADERS *>(module_base + dos_header->e_lfanew);
            if(nt_headers->Signature != IMAGE_NT_SIGNATURE) {
                startup_trace_log("D3D9 IAT fallback: invalid NT header");
                return false;
            }

            const auto &import_directory =
                nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if(import_directory.VirtualAddress == 0 || import_directory.Size == 0) {
                startup_trace_log("D3D9 IAT fallback: executable has no import directory");
                return false;
            }

            auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(
                module_base + import_directory.VirtualAddress
            );
            for(; descriptor->Name != 0; descriptor++) {
                const char *imported_module = reinterpret_cast<const char *>(module_base + descriptor->Name);
                if(std::strcmp(imported_module, "d3d9.dll") != 0) {
                    continue;
                }

                auto *iat = reinterpret_cast<IMAGE_THUNK_DATA *>(module_base + descriptor->FirstThunk);
                for(; iat->u1.Function != 0; iat++) {
                    auto *current_function = reinterpret_cast<void *>(
                        static_cast<std::uintptr_t>(iat->u1.Function)
                    );
                    if(current_function != target) {
                        continue;
                    }

                    DWORD old_protection = 0;
                    if(!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &old_protection)) {
                        startup_trace_log("D3D9 IAT fallback: VirtualProtect failed, Win32=%lu",
                                          static_cast<unsigned long>(GetLastError()));
                        return false;
                    }

                    *original = current_function;
                    iat->u1.Function = static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(replacement));

                    DWORD ignored = 0;
                    VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), old_protection, &ignored);
                    FlushInstructionCache(GetCurrentProcess(), &iat->u1.Function, sizeof(iat->u1.Function));
                    startup_trace_log("D3D9 IAT fallback installed: executable import %s!Direct3DCreate9 slot=%p original=%p",
                                      imported_module,
                                      static_cast<void *>(&iat->u1.Function),
                                      current_function);
                    return true;
                }

                startup_trace_log("D3D9 IAT fallback: d3d9.dll was imported but Direct3DCreate9 was not found in its IAT");
            }

            startup_trace_log("D3D9 IAT fallback: executable does not import d3d9.dll");
            return false;
        }

        static LONG WINAPI traced_change_display_settings_a(DEVMODEA *mode, DWORD flags) {
            startup_trace_log("ChangeDisplaySettingsA BEGIN flags=0x%08lx", static_cast<unsigned long>(flags));
            log_display_mode_a("ChangeDisplaySettingsA", mode);
            log_current_display_mode("Before ChangeDisplaySettingsA");
            const LONG result = original_change_display_settings_a
                ? original_change_display_settings_a(mode, flags)
                : DISP_CHANGE_FAILED;
            startup_trace_log("ChangeDisplaySettingsA END result=%ld (DISP_CHANGE_SUCCESSFUL=%d)",
                              static_cast<long>(result), DISP_CHANGE_SUCCESSFUL);
            log_current_display_mode("After ChangeDisplaySettingsA");
            return result;
        }

        static LONG WINAPI traced_change_display_settings_w(DEVMODEW *mode, DWORD flags) {
            startup_trace_log("ChangeDisplaySettingsW BEGIN flags=0x%08lx", static_cast<unsigned long>(flags));
            log_display_mode_w("ChangeDisplaySettingsW", mode);
            log_current_display_mode("Before ChangeDisplaySettingsW");
            const LONG result = original_change_display_settings_w
                ? original_change_display_settings_w(mode, flags)
                : DISP_CHANGE_FAILED;
            startup_trace_log("ChangeDisplaySettingsW END result=%ld (DISP_CHANGE_SUCCESSFUL=%d)",
                              static_cast<long>(result), DISP_CHANGE_SUCCESSFUL);
            log_current_display_mode("After ChangeDisplaySettingsW");
            return result;
        }

        static LONG WINAPI traced_change_display_settings_ex_a(
            LPCSTR device_name,
            DEVMODEA *mode,
            HWND window,
            DWORD flags,
            LPVOID lparam
        ) {
            startup_trace_log(
                "ChangeDisplaySettingsExA BEGIN device=%s hwnd=%p flags=0x%08lx",
                device_name ? device_name : "(primary)",
                static_cast<void *>(window),
                static_cast<unsigned long>(flags)
            );
            log_display_mode_a("ChangeDisplaySettingsExA", mode);
            log_current_display_mode("Before ChangeDisplaySettingsExA");
            const LONG result = original_change_display_settings_ex_a
                ? original_change_display_settings_ex_a(device_name, mode, window, flags, lparam)
                : DISP_CHANGE_FAILED;
            startup_trace_log("ChangeDisplaySettingsExA END result=%ld (DISP_CHANGE_SUCCESSFUL=%d)",
                              static_cast<long>(result), DISP_CHANGE_SUCCESSFUL);
            log_current_display_mode("After ChangeDisplaySettingsExA");
            return result;
        }

        static LONG WINAPI traced_change_display_settings_ex_w(
            LPCWSTR device_name,
            DEVMODEW *mode,
            HWND window,
            DWORD flags,
            LPVOID lparam
        ) {
            startup_trace_log(
                "ChangeDisplaySettingsExW BEGIN hwnd=%p flags=0x%08lx",
                static_cast<void *>(window),
                static_cast<unsigned long>(flags)
            );
            log_display_mode_w("ChangeDisplaySettingsExW", mode);
            log_current_display_mode("Before ChangeDisplaySettingsExW");
            const LONG result = original_change_display_settings_ex_w
                ? original_change_display_settings_ex_w(device_name, mode, window, flags, lparam)
                : DISP_CHANGE_FAILED;
            startup_trace_log("ChangeDisplaySettingsExW END result=%ld (DISP_CHANGE_SUCCESSFUL=%d)",
                              static_cast<long>(result), DISP_CHANGE_SUCCESSFUL);
            log_current_display_mode("After ChangeDisplaySettingsExW");
            return result;
        }

        static void install_display_change_hooks() noexcept {
            const void *original = nullptr;
            if(install_export_hook(
                "user32.dll",
                "ChangeDisplaySettingsA",
                change_display_settings_a_hook,
                reinterpret_cast<const void *>(traced_change_display_settings_a),
                &original
            )) {
                static_assert(sizeof(original_change_display_settings_a) == sizeof(original));
                std::memcpy(&original_change_display_settings_a, &original, sizeof(original_change_display_settings_a));
            }

            original = nullptr;
            if(install_export_hook(
                "user32.dll",
                "ChangeDisplaySettingsW",
                change_display_settings_w_hook,
                reinterpret_cast<const void *>(traced_change_display_settings_w),
                &original
            )) {
                static_assert(sizeof(original_change_display_settings_w) == sizeof(original));
                std::memcpy(&original_change_display_settings_w, &original, sizeof(original_change_display_settings_w));
            }

            original = nullptr;
            if(install_export_hook(
                "user32.dll",
                "ChangeDisplaySettingsExA",
                change_display_settings_ex_a_hook,
                reinterpret_cast<const void *>(traced_change_display_settings_ex_a),
                &original
            )) {
                static_assert(sizeof(original_change_display_settings_ex_a) == sizeof(original));
                std::memcpy(&original_change_display_settings_ex_a, &original, sizeof(original_change_display_settings_ex_a));
            }

            original = nullptr;
            if(install_export_hook(
                "user32.dll",
                "ChangeDisplaySettingsExW",
                change_display_settings_ex_w_hook,
                reinterpret_cast<const void *>(traced_change_display_settings_ex_w),
                &original
            )) {
                static_assert(sizeof(original_change_display_settings_ex_w) == sizeof(original));
                std::memcpy(&original_change_display_settings_ex_w, &original, sizeof(original_change_display_settings_ex_w));
            }
        }
    }

    void startup_trace_log_window(const char *label, void *window) noexcept {
        if(!trace_enabled) {
            return;
        }
        log_window(label, reinterpret_cast<HWND>(window));
    }

    void startup_trace_log(const char *format, ...) noexcept {
        if(!trace_enabled || trace_file == INVALID_HANDLE_VALUE || !format) {
            return;
        }

        char message[1024] {};
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(message, sizeof(message), format, arguments);
        va_end(arguments);

        LARGE_INTEGER now {};
        QueryPerformanceCounter(&now);
        const double elapsed_ms = trace_frequency.QuadPart > 0
            ? static_cast<double>(now.QuadPart - trace_start.QuadPart) * 1000.0 /
                static_cast<double>(trace_frequency.QuadPart)
            : static_cast<double>(GetTickCount());

        char line[1280] {};
        const int prefix_length = std::snprintf(
            line,
            sizeof(line),
            "[+%10.3f ms] pid=%lu tid=%lu ",
            elapsed_ms,
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long>(GetCurrentThreadId())
        );
        if(prefix_length < 0) {
            return;
        }

        const auto prefix_size = static_cast<std::size_t>(prefix_length);
        const auto message_size = std::strlen(message);
        const auto copy_size = message_size < sizeof(line) - prefix_size - 3
            ? message_size
            : sizeof(line) - prefix_size - 3;
        std::memcpy(line + prefix_size, message, copy_size);
        line[prefix_size + copy_size] = '\r';
        line[prefix_size + copy_size + 1] = '\n';
        line[prefix_size + copy_size + 2] = 0;

        while(InterlockedCompareExchange(&trace_lock, 1, 0) != 0) {
            Sleep(0);
        }
        DWORD written = 0;
        WriteFile(trace_file, line, static_cast<DWORD>(prefix_size + copy_size + 2), &written, nullptr);
        FlushFileBuffers(trace_file);
        InterlockedExchange(&trace_lock, 0);

        OutputDebugStringA(line);
    }

    void startup_trace_initialize(const Ini *ini) noexcept {
        if(trace_initialized) {
            return;
        }
        trace_initialized = true;
        trace_enabled = ini ? ini->get_value_bool("debug.startup_trace").value_or(true) : true;
        if(!trace_enabled) {
            return;
        }

        QueryPerformanceFrequency(&trace_frequency);
        QueryPerformanceCounter(&trace_start);

        trace_file = CreateFileA(
            "chimera_startup_trace.log",
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if(trace_file != INVALID_HANDLE_VALUE) {
            std::snprintf(trace_file_path, sizeof(trace_file_path), "%s", "chimera_startup_trace.log");
        }
        else {
            char temp_path[MAX_PATH] {};
            const DWORD temp_length = GetTempPathA(static_cast<DWORD>(sizeof(temp_path)), temp_path);
            if(temp_length > 0 && temp_length < sizeof(temp_path)) {
                std::snprintf(trace_file_path, sizeof(trace_file_path), "%schimera_startup_trace_%lu.log",
                              temp_path, static_cast<unsigned long>(GetCurrentProcessId()));
                trace_file = CreateFileA(
                    trace_file_path,
                    GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr
                );
            }
        }

        if(trace_file == INVALID_HANDLE_VALUE) {
            trace_enabled = false;
            OutputDebugStringA("Chimera startup trace: could not create log file.\n");
            return;
        }

        char executable_path[MAX_PATH] {};
        char current_directory[MAX_PATH] {};
        GetModuleFileNameA(nullptr, executable_path, static_cast<DWORD>(sizeof(executable_path)));
        GetCurrentDirectoryA(static_cast<DWORD>(sizeof(current_directory)), current_directory);

        startup_trace_log("========== CHIMERA STARTUP TRACE BEGIN ==========");
        startup_trace_log("log file opened: path=\"%s\" cwd=\"%s\" exe=\"%s\"",
                          trace_file_path,
                          current_directory,
                          executable_path);
        startup_trace_log("command line: %s", GetCommandLineA());
        startup_trace_log("system metrics: primary=%dx%d virtual=%dx%d origin=(%d,%d)",
                          GetSystemMetrics(SM_CXSCREEN),
                          GetSystemMetrics(SM_CYSCREEN),
                          GetSystemMetrics(SM_CXVIRTUALSCREEN),
                          GetSystemMetrics(SM_CYVIRTUALSCREEN),
                          GetSystemMetrics(SM_XVIRTUALSCREEN),
                          GetSystemMetrics(SM_YVIRTUALSCREEN));
        log_current_display_mode("Process attach / Chimera initialization");

        startup_trace_log("installing Direct3D9 and display-mode API hooks");
        HMODULE d3d9_module = GetModuleHandleA("d3d9.dll");
        if(!d3d9_module) {
            startup_trace_log("d3d9.dll is not loaded yet; Direct3DCreate9 cannot be hooked at this stage");
        }
        else {
            FARPROC direct3d_create9_address = GetProcAddress(d3d9_module, "Direct3DCreate9");
            if(!direct3d_create9_address) {
                startup_trace_log("d3d9.dll!Direct3DCreate9 was not found, Win32=%lu",
                                  static_cast<unsigned long>(GetLastError()));
            }
            else {
                const void *original = nullptr;
                if(install_export_hook(
                    "d3d9.dll",
                    "Direct3DCreate9",
                    direct3d_create9_hook,
                    reinterpret_cast<const void *>(traced_direct3d_create9),
                    &original
                )) {
                    static_assert(sizeof(original_direct3d_create9) == sizeof(original));
                    std::memcpy(&original_direct3d_create9, &original, sizeof(original_direct3d_create9));
                }
                else {
                    // The function prologue detour may be rejected by a wrapper or
                    // an unexpected system build. Fall back to the game's IAT.
                    original = nullptr;
                    if(install_main_executable_iat_hook(
                        reinterpret_cast<const void *>(direct3d_create9_address),
                        reinterpret_cast<const void *>(traced_direct3d_create9),
                        &original
                    )) {
                        static_assert(sizeof(original_direct3d_create9) == sizeof(original));
                        std::memcpy(&original_direct3d_create9, &original, sizeof(original_direct3d_create9));
                    }
                }
            }
        }

        install_display_change_hooks();
        log_current_display_mode("After diagnostic hooks installed");
        startup_trace_log("========== STARTUP TRACE INITIALIZATION COMPLETE ==========");
    }
}
