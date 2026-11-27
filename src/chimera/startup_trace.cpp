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
        using GetProcAddressFunction = FARPROC (WINAPI *)(HMODULE, LPCSTR);
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
        static GetProcAddressFunction original_get_proc_address = nullptr;
        static ChangeDisplaySettingsAFunction original_change_display_settings_a = nullptr;
        static ChangeDisplaySettingsWFunction original_change_display_settings_w = nullptr;
        static ChangeDisplaySettingsExAFunction original_change_display_settings_ex_a = nullptr;
        static ChangeDisplaySettingsExWFunction original_change_display_settings_ex_w = nullptr;

        static IDirect3D9 *WINAPI traced_direct3d_create9(UINT sdk_version);
        static CreateDeviceFunction original_create_device = nullptr;
        static DeviceResetFunction original_device_reset = nullptr;
        static DevicePresentFunction original_device_present = nullptr;
        // The D3D9 runtime requires Reset on the thread that created the device.
        // Keep this diagnostic identity so a rejected Reset can be attributed correctly.
        static PVOID volatile traced_device_instance = nullptr;
        static volatile LONG traced_device_creation_thread_id = 0;
        static volatile LONG traced_device_creation_flags = 0;

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

        static void log_calling_module(const char *label, const void *return_address) noexcept {
            HMODULE module = nullptr;
            if(!return_address || !GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(return_address),
                &module
            )) {
                startup_trace_log("%s: return_address=%p module=unresolved Win32=%lu",
                                  label,
                                  return_address,
                                  static_cast<unsigned long>(GetLastError()));
                return;
            }

            char module_path[MAX_PATH] {};
            const DWORD length = GetModuleFileNameA(module, module_path, static_cast<DWORD>(sizeof(module_path)));
            const auto base = reinterpret_cast<std::uintptr_t>(module);
            const auto address = reinterpret_cast<std::uintptr_t>(return_address);
            startup_trace_log(
                "%s: return_address=%p module=%s RVA=0x%08lx",
                label,
                return_address,
                length > 0 && length < sizeof(module_path) ? module_path : "(path unavailable)",
                static_cast<unsigned long>(address >= base ? address - base : 0)
            );
        }

        static const char *cooperative_level_name(HRESULT result) noexcept {
            switch(result) {
                case D3D_OK: return "D3D_OK";
                case D3DERR_DEVICELOST: return "D3DERR_DEVICELOST";
                case D3DERR_DEVICENOTRESET: return "D3DERR_DEVICENOTRESET";
                case D3DERR_INVALIDCALL: return "D3DERR_INVALIDCALL";
                default: return "OTHER";
            }
        }

        static void log_vtable_entry_details(const char *label, const void *entry) noexcept {
            if(!entry) {
                startup_trace_log("%s: entry=null", label);
                return;
            }

            MEMORY_BASIC_INFORMATION memory {};
            if(VirtualQuery(entry, &memory, sizeof(memory))) {
                startup_trace_log(
                    "%s: entry=%p allocation_base=%p region_base=%p region_size=%llu state=0x%08lx protect=0x%08lx type=0x%08lx",
                    label,
                    entry,
                    memory.AllocationBase,
                    memory.BaseAddress,
                    static_cast<unsigned long long>(memory.RegionSize),
                    static_cast<unsigned long>(memory.State),
                    static_cast<unsigned long>(memory.Protect),
                    static_cast<unsigned long>(memory.Type)
                );
            }
            else {
                startup_trace_log("%s: entry=%p VirtualQuery failed Win32=%lu",
                                  label,
                                  entry,
                                  static_cast<unsigned long>(GetLastError()));
            }
            log_calling_module(label, entry);
        }

        static void log_device_vtable(const char *label, IDirect3DDevice9 *device) noexcept {
            static void **last_vtable = nullptr;
            static void *last_reset_entry = nullptr;
            static void *last_present_entry = nullptr;

            if(!device) {
                startup_trace_log("%s: device=null", label);
                return;
            }

            auto **vtable = *reinterpret_cast<void ***>(device);
            if(!vtable) {
                startup_trace_log("%s: device=%p vtable=null", label, static_cast<void *>(device));
                return;
            }

            const bool vtable_changed = vtable != last_vtable;
            const bool reset_changed = vtable_changed || vtable[16] != last_reset_entry;
            const bool present_changed = vtable_changed || vtable[17] != last_present_entry;
            if(!reset_changed && !present_changed) {
                return;
            }

            startup_trace_log(
                "%s: device=%p vtable=%p Reset[16]=%p Present[17]=%p",
                label,
                static_cast<void *>(device),
                static_cast<void *>(vtable),
                vtable[16],
                vtable[17]
            );
            if(reset_changed) {
                log_vtable_entry_details("D3D9 vtable Reset[16] target", vtable[16]);
            }
            if(present_changed) {
                log_vtable_entry_details("D3D9 vtable Present[17] target", vtable[17]);
            }

            last_vtable = vtable;
            last_reset_entry = vtable[16];
            last_present_entry = vtable[17];
        }

        static void log_reset_window_thread(HWND window) noexcept {
            if(!window) {
                startup_trace_log("Reset window thread: hwnd=null");
                return;
            }

            DWORD process_id = 0;
            const DWORD window_thread = GetWindowThreadProcessId(window, &process_id);
            const DWORD current_thread = GetCurrentThreadId();
            startup_trace_log(
                "Reset window thread: hwnd=%p valid=%u window_thread=%lu current_thread=%lu "
                "window_process=%lu current_process=%lu same_thread=%u iconic=%u visible=%u "
                "foreground=%p active=%p focus=%p",
                static_cast<void *>(window),
                static_cast<unsigned int>(IsWindow(window)),
                static_cast<unsigned long>(window_thread),
                static_cast<unsigned long>(current_thread),
                static_cast<unsigned long>(process_id),
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned int>(window_thread != 0 && window_thread == current_thread),
                static_cast<unsigned int>(IsIconic(window)),
                static_cast<unsigned int>(IsWindowVisible(window)),
                static_cast<void *>(GetForegroundWindow()),
                static_cast<void *>(GetActiveWindow()),
                static_cast<void *>(GetFocus())
            );
        }

        static void log_reset_bound_resources(IDirect3DDevice9 *device) noexcept {
            if(!device) {
                startup_trace_log("Reset bound-resource inspection skipped: device=null");
                return;
            }

            IDirect3DSurface9 *back_buffer = nullptr;
            const HRESULT back_buffer_result = IDirect3DDevice9_GetBackBuffer(
                device,
                0,
                0,
                D3DBACKBUFFER_TYPE_MONO,
                &back_buffer
            );
            startup_trace_log(
                "Reset bound-resource inspection: GetBackBuffer HRESULT=0x%08lx surface=%p",
                static_cast<unsigned long>(back_buffer_result),
                static_cast<void *>(back_buffer)
            );

            for(DWORD target_index = 0; target_index < 4; target_index++) {
                IDirect3DSurface9 *surface = nullptr;
                const HRESULT result = IDirect3DDevice9_GetRenderTarget(device, target_index, &surface);
                if(FAILED(result) || !surface) {
                    if(target_index == 0) {
                        startup_trace_log(
                            "Reset bound render target #%lu unavailable: HRESULT=0x%08lx",
                            static_cast<unsigned long>(target_index),
                            static_cast<unsigned long>(result)
                        );
                    }
                    continue;
                }

                D3DSURFACE_DESC description {};
                const HRESULT description_result = IDirect3DSurface9_GetDesc(surface, &description);
                startup_trace_log(
                    "Reset bound render target #%lu: surface=%p desc_hr=0x%08lx "
                    "type=%u format=%u size=%lux%lu usage=0x%08lx pool=%u implicit_backbuffer=%u",
                    static_cast<unsigned long>(target_index),
                    static_cast<void *>(surface),
                    static_cast<unsigned long>(description_result),
                    static_cast<unsigned int>(description.Type),
                    static_cast<unsigned int>(description.Format),
                    static_cast<unsigned long>(description.Width),
                    static_cast<unsigned long>(description.Height),
                    static_cast<unsigned long>(description.Usage),
                    static_cast<unsigned int>(description.Pool),
                    static_cast<unsigned int>(back_buffer && surface == back_buffer)
                );
                surface->Release();
            }

            IDirect3DSurface9 *depth_surface = nullptr;
            const HRESULT depth_result = IDirect3DDevice9_GetDepthStencilSurface(device, &depth_surface);
            if(SUCCEEDED(depth_result) && depth_surface) {
                D3DSURFACE_DESC description {};
                const HRESULT description_result = IDirect3DSurface9_GetDesc(depth_surface, &description);
                startup_trace_log(
                    "Reset bound depth-stencil: surface=%p desc_hr=0x%08lx "
                    "format=%u size=%lux%lu usage=0x%08lx pool=%u",
                    static_cast<void *>(depth_surface),
                    static_cast<unsigned long>(description_result),
                    static_cast<unsigned int>(description.Format),
                    static_cast<unsigned long>(description.Width),
                    static_cast<unsigned long>(description.Height),
                    static_cast<unsigned long>(description.Usage),
                    static_cast<unsigned int>(description.Pool)
                );
                depth_surface->Release();
            }
            else {
                startup_trace_log(
                    "Reset bound depth-stencil unavailable: HRESULT=0x%08lx",
                    static_cast<unsigned long>(depth_result)
                );
            }

            for(DWORD stage = 0; stage < 16; stage++) {
                IDirect3DBaseTexture9 *texture = nullptr;
                const HRESULT result = IDirect3DDevice9_GetTexture(device, stage, &texture);
                if(FAILED(result) || !texture) {
                    continue;
                }

                const D3DRESOURCETYPE resource_type = IDirect3DBaseTexture9_GetType(texture);
                if(resource_type == D3DRTYPE_TEXTURE) {
                    auto *texture_2d = static_cast<IDirect3DTexture9 *>(texture);
                    D3DSURFACE_DESC description {};
                    const HRESULT description_result = IDirect3DTexture9_GetLevelDesc(texture_2d, 0, &description);
                    startup_trace_log(
                        "Reset bound texture stage=%lu: texture=%p type=%u desc_hr=0x%08lx "
                        "format=%u size=%lux%lu usage=0x%08lx pool=%u",
                        static_cast<unsigned long>(stage),
                        static_cast<void *>(texture),
                        static_cast<unsigned int>(resource_type),
                        static_cast<unsigned long>(description_result),
                        static_cast<unsigned int>(description.Format),
                        static_cast<unsigned long>(description.Width),
                        static_cast<unsigned long>(description.Height),
                        static_cast<unsigned long>(description.Usage),
                        static_cast<unsigned int>(description.Pool)
                    );
                }
                else {
                    startup_trace_log(
                        "Reset bound texture stage=%lu: texture=%p type=%u",
                        static_cast<unsigned long>(stage),
                        static_cast<void *>(texture),
                        static_cast<unsigned int>(resource_type)
                    );
                }
                texture->Release();
            }

            for(UINT stream = 0; stream < 16; stream++) {
                IDirect3DVertexBuffer9 *buffer = nullptr;
                UINT offset = 0;
                UINT stride = 0;
                const HRESULT result = IDirect3DDevice9_GetStreamSource(
                    device,
                    stream,
                    &buffer,
                    &offset,
                    &stride
                );
                if(FAILED(result) || !buffer) {
                    continue;
                }

                D3DVERTEXBUFFER_DESC description {};
                const HRESULT description_result = IDirect3DVertexBuffer9_GetDesc(buffer, &description);
                startup_trace_log(
                    "Reset bound vertex buffer stream=%u: buffer=%p desc_hr=0x%08lx "
                    "size=%lu usage=0x%08lx fvf=0x%08lx pool=%u offset=%u stride=%u",
                    stream,
                    static_cast<void *>(buffer),
                    static_cast<unsigned long>(description_result),
                    static_cast<unsigned long>(description.Size),
                    static_cast<unsigned long>(description.Usage),
                    static_cast<unsigned long>(description.FVF),
                    static_cast<unsigned int>(description.Pool),
                    offset,
                    stride
                );
                buffer->Release();
            }

            IDirect3DIndexBuffer9 *index_buffer = nullptr;
            const HRESULT index_result = IDirect3DDevice9_GetIndices(device, &index_buffer);
            if(SUCCEEDED(index_result) && index_buffer) {
                D3DINDEXBUFFER_DESC description {};
                const HRESULT description_result = IDirect3DIndexBuffer9_GetDesc(index_buffer, &description);
                startup_trace_log(
                    "Reset bound index buffer: buffer=%p desc_hr=0x%08lx "
                    "size=%lu usage=0x%08lx format=%u pool=%u",
                    static_cast<void *>(index_buffer),
                    static_cast<unsigned long>(description_result),
                    static_cast<unsigned long>(description.Size),
                    static_cast<unsigned long>(description.Usage),
                    static_cast<unsigned int>(description.Format),
                    static_cast<unsigned int>(description.Pool)
                );
                index_buffer->Release();
            }

            const UINT swap_chain_count = IDirect3DDevice9_GetNumberOfSwapChains(device);
            startup_trace_log("Reset swap-chain count=%u", swap_chain_count);
            for(UINT index = 1; index < swap_chain_count && index < 8; index++) {
                IDirect3DSwapChain9 *swap_chain = nullptr;
                const HRESULT result = IDirect3DDevice9_GetSwapChain(device, index, &swap_chain);
                startup_trace_log(
                    "Reset additional swap-chain #%u: HRESULT=0x%08lx chain=%p",
                    index,
                    static_cast<unsigned long>(result),
                    static_cast<void *>(swap_chain)
                );
                if(swap_chain) {
                    swap_chain->Release();
                }
            }

            if(back_buffer) {
                back_buffer->Release();
            }
        }

        static HRESULT WINAPI traced_device_reset(
            IDirect3DDevice9 *device,
            D3DPRESENT_PARAMETERS *params
        ) {
            static LONG reset_count = 0;
            const LONG number = InterlockedIncrement(&reset_count);
            const DWORD reset_thread_id = GetCurrentThreadId();
            const auto created_device = InterlockedCompareExchangePointer(
                &traced_device_instance,
                nullptr,
                nullptr
            );
            const DWORD creation_thread_id = static_cast<DWORD>(
                InterlockedCompareExchange(&traced_device_creation_thread_id, 0, 0)
            );
            const DWORD creation_flags = static_cast<DWORD>(
                InterlockedCompareExchange(&traced_device_creation_flags, 0, 0)
            );
            const bool creation_identity_matches =
                created_device == static_cast<PVOID>(device) && creation_thread_id != 0;
            const bool reset_thread_matches =
                creation_identity_matches && reset_thread_id == creation_thread_id;
            const bool multithreaded_creation =
                creation_identity_matches && (creation_flags & D3DCREATE_MULTITHREADED) != 0;

            startup_trace_log(
                "D3D9 Reset #%ld BEGIN device=%p thread=%lu created_device=%p creation_thread=%lu thread_match=%u multithreaded_creation=%u",
                static_cast<long>(number),
                static_cast<void *>(device),
                static_cast<unsigned long>(reset_thread_id),
                created_device,
                static_cast<unsigned long>(creation_thread_id),
                static_cast<unsigned int>(reset_thread_matches),
                static_cast<unsigned int>(multithreaded_creation)
            );
            if(creation_identity_matches && !reset_thread_matches) {
                startup_trace_log(
                    "D3D9 Reset THREAD MISMATCH: Reset thread=%lu, device creation thread=%lu, D3DCREATE_MULTITHREADED=%u",
                    static_cast<unsigned long>(reset_thread_id),
                    static_cast<unsigned long>(creation_thread_id),
                    static_cast<unsigned int>(multithreaded_creation)
                );
            }
            else if(!creation_identity_matches) {
                startup_trace_log(
                    "D3D9 Reset thread comparison unavailable: Reset device=%p tracked device=%p creation_thread=%lu",
                    static_cast<void *>(device),
                    created_device,
                    static_cast<unsigned long>(creation_thread_id)
                );
            }
            log_calling_module("D3D9 Reset caller", __builtin_return_address(0));
            log_device_vtable("D3D9 Reset vtable at entry", device);
            startup_trace_log_device_cooperative_level("D3D9 Reset immediately before native call", device);
            log_present_parameters("Reset input", params);
            log_reset_window_thread(params ? params->hDeviceWindow : nullptr);
            log_reset_bound_resources(device);
            log_current_display_mode("Before Reset");

            if(!original_device_reset) {
                startup_trace_log("D3D9 Reset #%ld ABORT: original function pointer is null", static_cast<long>(number));
                return D3DERR_INVALIDCALL;
            }

            const HRESULT result = original_device_reset(device, params);
            startup_trace_log("D3D9 Reset #%ld END HRESULT=0x%08lx",
                              static_cast<long>(number),
                              static_cast<unsigned long>(result));
            startup_trace_log_device_cooperative_level("D3D9 Reset immediately after native call", device);
            log_device_vtable("D3D9 Reset vtable after native call", device);
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

            const bool reset_needs_hook = vtable[16] != reinterpret_cast<void *>(traced_device_reset);
            const bool present_needs_hook = vtable[17] != reinterpret_cast<void *>(traced_device_present);
            if(!reset_needs_hook && !present_needs_hook) {
                return;
            }

            startup_trace_log(
                "D3D9 vtable hook verification: device=%p vtable=%p Reset[16]=%p Present[17]=%p reset_rehook=%u present_rehook=%u",
                static_cast<void *>(device),
                static_cast<void *>(vtable),
                vtable[16],
                vtable[17],
                static_cast<unsigned int>(reset_needs_hook),
                static_cast<unsigned int>(present_needs_hook)
            );

            if(reset_needs_hook) {
                log_vtable_entry_details("Reset[16] target before Chimera trace hook", vtable[16]);
                const void *original = nullptr;
                if(!patch_vtable_entry(
                    vtable,
                    16, // IDirect3DDevice9::Reset
                    reinterpret_cast<const void *>(traced_device_reset),
                    &original
                )) {
                    startup_trace_log("IDirect3DDevice9::Reset hook could not be installed");
                }
                else {
                    if(!original_device_reset) {
                        static_assert(sizeof(original_device_reset) == sizeof(original));
                        std::memcpy(&original_device_reset, &original, sizeof(original_device_reset));
                    }
                    startup_trace_log(
                        "IDirect3DDevice9::Reset hook installed: device=%p vtable=%p observed_original=%p stored_original=%p",
                        static_cast<void *>(device),
                        static_cast<void *>(vtable),
                        const_cast<void *>(original),
                        reinterpret_cast<void *>(original_device_reset)
                    );
                }
            }

            if(present_needs_hook) {
                log_vtable_entry_details("Present[17] target before Chimera trace hook", vtable[17]);
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
                    startup_trace_log(
                        "IDirect3DDevice9::Present hook installed: device=%p observed_original=%p stored_original=%p",
                        static_cast<void *>(device),
                        const_cast<void *>(present_original),
                        reinterpret_cast<void *>(original_device_present)
                    );
                }
                else {
                    startup_trace_log("IDirect3DDevice9::Present hook could not be installed");
                }
            }

            log_device_vtable("D3D9 vtable after hook verification", device);
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
            const DWORD create_thread_id = GetCurrentThreadId();
            startup_trace_log(
                "D3D9 CreateDevice #%ld BEGIN d3d=%p adapter=%u type=%u focus=%p thread=%lu behavior=0x%08lx multithreaded=%u",
                static_cast<long>(number),
                static_cast<void *>(d3d),
                adapter,
                static_cast<unsigned int>(device_type),
                static_cast<void *>(focus_window),
                static_cast<unsigned long>(create_thread_id),
                static_cast<unsigned long>(behavior_flags),
                static_cast<unsigned int>((behavior_flags & D3DCREATE_MULTITHREADED) != 0)
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
                InterlockedExchangePointer(&traced_device_instance, static_cast<PVOID>(device));
                InterlockedExchange(&traced_device_creation_flags, static_cast<LONG>(behavior_flags));
                // Publish the thread ID last so readers never treat a partial identity as valid.
                InterlockedExchange(&traced_device_creation_thread_id, static_cast<LONG>(create_thread_id));
                startup_trace_log(
                    "D3D9 device creation identity captured: device=%p creation_thread=%lu behavior=0x%08lx multithreaded=%u",
                    static_cast<void *>(device),
                    static_cast<unsigned long>(create_thread_id),
                    static_cast<unsigned long>(behavior_flags),
                    static_cast<unsigned int>((behavior_flags & D3DCREATE_MULTITHREADED) != 0)
                );
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

        static bool install_main_executable_import_hook(
            const char *import_module_name,
            const char *import_function_name,
            const void *replacement,
            const void **original
        ) noexcept {
            if(!import_module_name || !import_function_name || !replacement || !original) {
                return false;
            }

            auto *module_base = reinterpret_cast<std::byte *>(GetModuleHandleA(nullptr));
            if(!module_base) {
                startup_trace_log("IAT hook: main executable module handle is null");
                return false;
            }

            auto *dos_header = reinterpret_cast<IMAGE_DOS_HEADER *>(module_base);
            if(dos_header->e_magic != IMAGE_DOS_SIGNATURE || dos_header->e_lfanew <= 0) {
                startup_trace_log("IAT hook: invalid DOS header");
                return false;
            }

            auto *nt_headers = reinterpret_cast<IMAGE_NT_HEADERS *>(module_base + dos_header->e_lfanew);
            if(nt_headers->Signature != IMAGE_NT_SIGNATURE) {
                startup_trace_log("IAT hook: invalid NT header");
                return false;
            }

            const auto &directory =
                nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if(directory.VirtualAddress == 0 || directory.Size == 0) {
                startup_trace_log("IAT hook: executable has no import directory");
                return false;
            }

            auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(module_base + directory.VirtualAddress);
            for(; descriptor->Name != 0; descriptor++) {
                const char *module_name = reinterpret_cast<const char *>(module_base + descriptor->Name);
                if(lstrcmpiA(module_name, import_module_name) != 0) {
                    continue;
                }

                if(descriptor->OriginalFirstThunk == 0) {
                    startup_trace_log(
                        "IAT hook unavailable: %s import table has no OriginalFirstThunk; cannot safely resolve names",
                        import_module_name
                    );
                    return false;
                }

                auto *lookup = reinterpret_cast<IMAGE_THUNK_DATA *>(
                    module_base + descriptor->OriginalFirstThunk
                );
                auto *iat = reinterpret_cast<IMAGE_THUNK_DATA *>(module_base + descriptor->FirstThunk);
                bool installed = false;

                for(; lookup->u1.AddressOfData != 0 && iat->u1.Function != 0; lookup++, iat++) {
                    if(IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal)) {
                        continue;
                    }

                    auto *name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(
                        module_base + lookup->u1.AddressOfData
                    );
                    const char *symbol_name = reinterpret_cast<const char *>(name->Name);
                    if(std::strcmp(symbol_name, import_function_name) != 0) {
                        continue;
                    }

                    DWORD old_protection = 0;
                    if(!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &old_protection)) {
                        startup_trace_log(
                            "IAT hook failed for %s!%s: VirtualProtect Win32=%lu",
                            import_module_name,
                            import_function_name,
                            static_cast<unsigned long>(GetLastError())
                        );
                        continue;
                    }

                    const auto current = reinterpret_cast<const void *>(
                        static_cast<std::uintptr_t>(iat->u1.Function)
                    );
                    if(current != replacement) {
                        *original = current;
                        iat->u1.Function = static_cast<ULONG_PTR>(
                            reinterpret_cast<std::uintptr_t>(replacement)
                        );
                    }
                    else if(!*original) {
                        *original = current;
                    }

                    DWORD ignored = 0;
                    VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), old_protection, &ignored);
                    FlushInstructionCache(GetCurrentProcess(), &iat->u1.Function, sizeof(iat->u1.Function));
                    startup_trace_log(
                        "IAT hook installed: %s!%s slot=%p original=%p",
                        import_module_name,
                        import_function_name,
                        static_cast<void *>(&iat->u1.Function),
                        const_cast<void *>(*original)
                    );
                    installed = true;
                }

                if(!installed) {
                    startup_trace_log(
                        "IAT hook unavailable: executable imports %s but not %s",
                        import_module_name,
                        import_function_name
                    );
                }
                return installed;
            }

            startup_trace_log("IAT hook unavailable: executable does not import module %s", import_module_name);
            return false;
        }

        static FARPROC WINAPI traced_get_proc_address(HMODULE module, LPCSTR function_name) {
            if(!original_get_proc_address) {
                startup_trace_log("GetProcAddress trace ABORT: original function pointer is null");
                return nullptr;
            }

            FARPROC address = original_get_proc_address(module, function_name);
            if(!address || !module || !function_name ||
               reinterpret_cast<std::uintptr_t>(function_name) <= 0xFFFFU ||
               std::strcmp(function_name, "Direct3DCreate9") != 0) {
                return address;
            }

            char module_path[MAX_PATH] {};
            const DWORD path_length = GetModuleFileNameA(
                module,
                module_path,
                static_cast<DWORD>(sizeof(module_path))
            );
            if(path_length == 0 || path_length >= sizeof(module_path)) {
                return address;
            }

            const char *basename = std::strrchr(module_path, '\\');
            basename = basename ? basename + 1 : module_path;
            if(lstrcmpiA(basename, "d3d9.dll") != 0) {
                return address;
            }

            static_assert(sizeof(original_direct3d_create9) == sizeof(address));
            std::memcpy(&original_direct3d_create9, &address, sizeof(original_direct3d_create9));
            startup_trace_log(
                "GetProcAddress captured d3d9.dll!Direct3DCreate9 at %p; returning traced wrapper",
                reinterpret_cast<void *>(address)
            );
            return reinterpret_cast<FARPROC>(traced_direct3d_create9);
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
            else {
                original = nullptr;
                if(install_main_executable_import_hook(
                    "USER32.dll",
                    "ChangeDisplaySettingsA",
                    reinterpret_cast<const void *>(traced_change_display_settings_a),
                    &original
                )) {
                    static_assert(sizeof(original_change_display_settings_a) == sizeof(original));
                    std::memcpy(&original_change_display_settings_a, &original, sizeof(original_change_display_settings_a));
                }
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
            else {
                original = nullptr;
                if(install_main_executable_import_hook(
                    "USER32.dll",
                    "ChangeDisplaySettingsW",
                    reinterpret_cast<const void *>(traced_change_display_settings_w),
                    &original
                )) {
                    static_assert(sizeof(original_change_display_settings_w) == sizeof(original));
                    std::memcpy(&original_change_display_settings_w, &original, sizeof(original_change_display_settings_w));
                }
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
            else {
                original = nullptr;
                if(install_main_executable_import_hook(
                    "USER32.dll",
                    "ChangeDisplaySettingsExA",
                    reinterpret_cast<const void *>(traced_change_display_settings_ex_a),
                    &original
                )) {
                    static_assert(sizeof(original_change_display_settings_ex_a) == sizeof(original));
                    std::memcpy(&original_change_display_settings_ex_a, &original, sizeof(original_change_display_settings_ex_a));
                }
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
            else {
                original = nullptr;
                if(install_main_executable_import_hook(
                    "USER32.dll",
                    "ChangeDisplaySettingsExW",
                    reinterpret_cast<const void *>(traced_change_display_settings_ex_w),
                    &original
                )) {
                    static_assert(sizeof(original_change_display_settings_ex_w) == sizeof(original));
                    std::memcpy(&original_change_display_settings_ex_w, &original, sizeof(original_change_display_settings_ex_w));
                }
            }
        }
    }

    void startup_trace_on_reset_call(
        IDirect3DDevice9 *device,
        D3DPRESENT_PARAMETERS *params
    ) noexcept {
        if(!trace_enabled) {
            return;
        }

        startup_trace_log(
            "Halo Reset call-site: device=%p params=%p",
            static_cast<void *>(device),
            static_cast<void *>(params)
        );
        log_present_parameters("Halo Reset call-site parameters", params);
        log_current_display_mode("Immediately before Halo Reset");

        const HRESULT cooperative_result =
            startup_trace_log_device_cooperative_level("Halo Reset call-site device state", device);
        if(device && cooperative_result == D3D_OK) {
            D3DDEVICE_CREATION_PARAMETERS creation {};
            const HRESULT creation_result = device->GetCreationParameters(&creation);
            startup_trace_log(
                "D3D9 device creation parameters: HRESULT=0x%08lx adapter=%u device_type=%u focus_window=%p behavior=0x%08lx",
                static_cast<unsigned long>(creation_result),
                creation.AdapterOrdinal,
                static_cast<unsigned int>(creation.DeviceType),
                static_cast<void *>(creation.hFocusWindow),
                static_cast<unsigned long>(creation.BehaviorFlags)
            );

            IDirect3D9 *d3d = nullptr;
            const HRESULT get_d3d_result = device->GetDirect3D(&d3d);
            startup_trace_log(
                "IDirect3DDevice9::GetDirect3D HRESULT=0x%08lx interface=%p",
                static_cast<unsigned long>(get_d3d_result),
                static_cast<void *>(d3d)
            );
            if(SUCCEEDED(get_d3d_result) && d3d) {
                D3DDISPLAYMODE current_adapter_mode {};
                const HRESULT adapter_mode_result = d3d->GetAdapterDisplayMode(
                    creation.AdapterOrdinal,
                    &current_adapter_mode
                );
                startup_trace_log(
                    "D3D9 adapter current mode: HRESULT=0x%08lx %ux%u %uHz format=%u",
                    static_cast<unsigned long>(adapter_mode_result),
                    current_adapter_mode.Width,
                    current_adapter_mode.Height,
                    current_adapter_mode.RefreshRate,
                    static_cast<unsigned int>(current_adapter_mode.Format)
                );

                if(params) {
                    const D3DFORMAT requested_format = params->BackBufferFormat != D3DFMT_UNKNOWN
                        ? params->BackBufferFormat
                        : current_adapter_mode.Format;
                    const UINT mode_count = d3d->GetAdapterModeCount(
                        creation.AdapterOrdinal,
                        requested_format
                    );
                    unsigned int matching_modes = 0;
                    for(UINT mode_index = 0; mode_index < mode_count; mode_index++) {
                        D3DDISPLAYMODE mode {};
                        if(FAILED(d3d->EnumAdapterModes(
                            creation.AdapterOrdinal,
                            requested_format,
                            mode_index,
                            &mode
                        ))) {
                            continue;
                        }
                        if(mode.Width == params->BackBufferWidth &&
                           mode.Height == params->BackBufferHeight) {
                            startup_trace_log(
                                "D3D9 adapter matching mode #%u: %ux%u %uHz format=%u%s",
                                mode_index,
                                mode.Width,
                                mode.Height,
                                mode.RefreshRate,
                                static_cast<unsigned int>(mode.Format),
                                mode.RefreshRate == params->FullScreen_RefreshRateInHz
                                    ? " [REQUESTED REFRESH SUPPORTED]"
                                    : ""
                            );
                            matching_modes++;
                        }
                    }
                    startup_trace_log(
                        "D3D9 adapter reports %u mode(s) for requested resolution %lux%lu and format %u",
                        matching_modes,
                        static_cast<unsigned long>(params->BackBufferWidth),
                        static_cast<unsigned long>(params->BackBufferHeight),
                        static_cast<unsigned int>(requested_format)
                    );
                }
                d3d->Release();
            }
        }
        else if(device) {
            startup_trace_log(
                "Skipping GetCreationParameters/GetDirect3D: device cooperative level is %s (0x%08lx)",
                cooperative_level_name(cooperative_result),
                static_cast<unsigned long>(cooperative_result)
            );
        }

        if(params && !params->Windowed && params->FullScreen_RefreshRateInHz > 0) {
            unsigned int matching_modes = 0;
            for(DWORD mode_index = 0; mode_index < 512; mode_index++) {
                DEVMODEA mode {};
                mode.dmSize = sizeof(mode);
                if(!EnumDisplaySettingsA(nullptr, mode_index, &mode)) {
                    break;
                }
                if(mode.dmPelsWidth == params->BackBufferWidth &&
                   mode.dmPelsHeight == params->BackBufferHeight) {
                    startup_trace_log(
                        "Desktop mode candidate #%lu: %lux%lu %luHz %lu-bpp",
                        static_cast<unsigned long>(mode_index),
                        static_cast<unsigned long>(mode.dmPelsWidth),
                        static_cast<unsigned long>(mode.dmPelsHeight),
                        static_cast<unsigned long>(mode.dmDisplayFrequency),
                        static_cast<unsigned long>(mode.dmBitsPerPel)
                    );
                    matching_modes++;
                }
            }
            startup_trace_log(
                "Display modes matching requested fullscreen resolution %lux%lu: %u",
                static_cast<unsigned long>(params->BackBufferWidth),
                static_cast<unsigned long>(params->BackBufferHeight),
                matching_modes
            );
        }

        // Halo's call-site hook runs immediately before the original IDirect3DDevice9::Reset.
        // Install the COM hooks here so this very first Reset and all subsequent Present calls
        // are captured, even though d3d9.dll was dynamically loaded after Chimera initialized.
        install_device_reset_hook_real(device);
    }

    HRESULT startup_trace_log_device_cooperative_level(
        const char *label,
        IDirect3DDevice9 *device
    ) noexcept {
        if(!trace_enabled) {
            return S_FALSE;
        }
        if(!device) {
            startup_trace_log("%s: TestCooperativeLevel skipped because device=null",
                              label ? label : "D3D9 device");
            return E_POINTER;
        }

        const HRESULT result = device->TestCooperativeLevel();
        startup_trace_log(
            "%s: TestCooperativeLevel=%s HRESULT=0x%08lx",
            label ? label : "D3D9 device",
            cooperative_level_name(result),
            static_cast<unsigned long>(result)
        );
        return result;
    }

    void startup_trace_log_window(const char *label, void *window) noexcept {
        if(!trace_enabled) {
            return;
        }
        log_window(label, reinterpret_cast<HWND>(window));
    }

    void startup_trace_log_address_module(const char *label, const void *address) noexcept {
        if(!trace_enabled) {
            return;
        }
        log_calling_module(label ? label : "D3D9 callback implementation", address);
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
        // This tracer patches D3D9 vtables and performs synchronous file I/O.
        // Keep it opt-in so normal gameplay does not inherit diagnostic overhead.
        trace_enabled = ini && ini->get_value_bool("debug.startup_trace").value_or(false);
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

        const void *get_proc_original = nullptr;
        if(install_main_executable_import_hook(
            "KERNEL32.dll",
            "GetProcAddress",
            reinterpret_cast<const void *>(traced_get_proc_address),
            &get_proc_original
        )) {
            static_assert(sizeof(original_get_proc_address) == sizeof(get_proc_original));
            std::memcpy(&original_get_proc_address, &get_proc_original, sizeof(original_get_proc_address));
        }

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
