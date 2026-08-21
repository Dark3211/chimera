// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <cstring>
#include <new>

#include "d3d9_backend.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace {
        constexpr UINT MAX_D3D9ON12_QUEUES = 2;

        // MinGW releases do not all ship d3d9on12.h yet. Keep the ABI-compatible
        // definition here and resolve the Windows entrypoint dynamically.
        struct D3D9On12Args {
            BOOL enable_9_on_12;
            IUnknown *d3d12_device;
            IUnknown *d3d12_queues[MAX_D3D9ON12_QUEUES];
            UINT queue_count;
            UINT node_mask;
        };

        using Direct3DCreate9Function = IDirect3D9 *(WINAPI *)(UINT);
        using Direct3DCreate9On12Function = IDirect3D9 *(WINAPI *)(UINT, D3D9On12Args *, UINT);
        using GetProcAddressFunction = FARPROC (WINAPI *)(HMODULE, LPCSTR);

        Direct3DCreate9Function native_direct3d_create9 = nullptr;
        Direct3DCreate9On12Function direct3d_create9_on_12 = nullptr;
        GetProcAddressFunction native_get_proc_address = nullptr;

        enum class D3D9BackendStatus {
            DISABLED,
            HOOK_INSTALLED,
            HOOK_INSTALL_FAILED,
            ENTRYPOINT_UNAVAILABLE,
            ENUMERATOR_CREATION_FAILED,
            ENUMERATOR_CREATED,
            ON_12_ACTIVE,
            NATIVE_FALLBACK,
            DEVICE_CREATION_FAILED
        };

        D3D9BackendStatus backend_status = D3D9BackendStatus::DISABLED;

        constexpr GUID IID_IUNKNOWN_VALUE = {
            0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
        };
        constexpr GUID IID_IDIRECT3D9_VALUE = {
            0x81BDCBCA, 0x64D4, 0x426D, {0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C}
        };
        constexpr GUID IID_IDIRECT3DDEVICE9ON12_VALUE = {
            0xE7FDA234, 0xB589, 0x4049, {0x94, 0x0D, 0x88, 0x78, 0x97, 0x75, 0x31, 0xC8}
        };

        bool equal_guid(REFIID first, const GUID &second) noexcept {
            return std::memcmp(&first, &second, sizeof(GUID)) == 0;
        }

        bool is_d3d9_on_12_device(IDirect3DDevice9 *device) noexcept {
            if(!device) {
                return false;
            }

            IUnknown *on_12_interface = nullptr;
            auto result = device->QueryInterface(IID_IDIRECT3DDEVICE9ON12_VALUE,
                                                 reinterpret_cast<void **>(&on_12_interface));
            if(SUCCEEDED(result) && on_12_interface) {
                on_12_interface->Release();
                return true;
            }
            return false;
        }

        Direct3DCreate9On12Function resolve_direct3d_create9_on_12() noexcept {
            if(direct3d_create9_on_12) {
                return direct3d_create9_on_12;
            }

            // Chimera is initialized before Halo loads D3D9 on some systems.
            // Resolve the optional entrypoint only when Halo calls Direct3DCreate9.
            auto d3d9_module = GetModuleHandleA("d3d9.dll");
            if(!d3d9_module) {
                backend_status = D3D9BackendStatus::ENTRYPOINT_UNAVAILABLE;
                return nullptr;
            }

            auto procedure = GetProcAddress(d3d9_module, "Direct3DCreate9On12");
            if(!procedure) {
                backend_status = D3D9BackendStatus::ENTRYPOINT_UNAVAILABLE;
                return nullptr;
            }

            static_assert(sizeof(procedure) == sizeof(direct3d_create9_on_12));
            std::memcpy(&direct3d_create9_on_12, &procedure, sizeof(direct3d_create9_on_12));
            return direct3d_create9_on_12;
        }

        class Direct3D9On12Fallback final : public IDirect3D9 {
        public:
            Direct3D9On12Fallback(IDirect3D9 *on_12, IDirect3D9 *native) noexcept
                : on_12(on_12), native(native), active(on_12) {}

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
                if(!object) {
                    return E_POINTER;
                }
                if(equal_guid(riid, IID_IUNKNOWN_VALUE) || equal_guid(riid, IID_IDIRECT3D9_VALUE)) {
                    *object = this;
                    AddRef();
                    return S_OK;
                }
                return this->active->QueryInterface(riid, object);
            }

            ULONG STDMETHODCALLTYPE AddRef() override {
                return static_cast<ULONG>(InterlockedIncrement(&this->references));
            }

            ULONG STDMETHODCALLTYPE Release() override {
                auto remaining = InterlockedDecrement(&this->references);
                if(remaining == 0) {
                    delete this;
                }
                return static_cast<ULONG>(remaining);
            }

            HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *initialize_function) override {
                return this->active->RegisterSoftwareDevice(initialize_function);
            }

            UINT STDMETHODCALLTYPE GetAdapterCount() override {
                return this->active->GetAdapterCount();
            }

            HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *identifier) override {
                return this->active->GetAdapterIdentifier(adapter, flags, identifier);
            }

            UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter, D3DFORMAT format) override {
                return this->active->GetAdapterModeCount(adapter, format);
            }

            HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE *display_mode) override {
                return this->active->EnumAdapterModes(adapter, format, mode, display_mode);
            }

            HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *mode) override {
                return this->active->GetAdapterDisplayMode(adapter, mode);
            }

            HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT adapter_format, D3DFORMAT back_buffer_format, BOOL windowed) override {
                return this->active->CheckDeviceType(adapter, device_type, adapter_format, back_buffer_format, windowed);
            }

            HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource_type, D3DFORMAT check_format) override {
                return this->active->CheckDeviceFormat(adapter, device_type, adapter_format, usage, resource_type, check_format);
            }

            HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT surface_format, BOOL windowed, D3DMULTISAMPLE_TYPE multisample_type, DWORD *quality_levels) override {
                return this->active->CheckDeviceMultiSampleType(adapter, device_type, surface_format, windowed, multisample_type, quality_levels);
            }

            HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT adapter_format, D3DFORMAT render_target_format, D3DFORMAT depth_stencil_format) override {
                return this->active->CheckDepthStencilMatch(adapter, device_type, adapter_format, render_target_format, depth_stencil_format);
            }

            HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT source_format, D3DFORMAT target_format) override {
                return this->active->CheckDeviceFormatConversion(adapter, device_type, source_format, target_format);
            }

            HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE device_type, D3DCAPS9 *caps) override {
                return this->active->GetDeviceCaps(adapter, device_type, caps);
            }

            HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override {
                return this->active->GetAdapterMonitor(adapter);
            }

            HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter,
                                                   D3DDEVTYPE device_type,
                                                   HWND focus_window,
                                                   DWORD behavior_flags,
                                                   D3DPRESENT_PARAMETERS *presentation_parameters,
                                                   IDirect3DDevice9 **device) override {
                auto result = this->active->CreateDevice(adapter,
                                                         device_type,
                                                         focus_window,
                                                         behavior_flags,
                                                         presentation_parameters,
                                                         device);

                if(SUCCEEDED(result) && this->active == this->on_12) {
                    backend_status = device && is_d3d9_on_12_device(*device)
                                         ? D3D9BackendStatus::ON_12_ACTIVE
                                         : D3D9BackendStatus::NATIVE_FALLBACK;
                }

                // If 9On12 cannot create Halo's device, retry once using the
                // exact native D3D9 entrypoint that Halo originally imported.
                if(FAILED(result) && this->active == this->on_12 && this->native) {
                    if(device && *device) {
                        (*device)->Release();
                        *device = nullptr;
                    }
                    this->active = this->native;
                    result = this->active->CreateDevice(adapter,
                                                        device_type,
                                                        focus_window,
                                                        behavior_flags,
                                                        presentation_parameters,
                                                        device);
                    backend_status = SUCCEEDED(result)
                                         ? D3D9BackendStatus::NATIVE_FALLBACK
                                         : D3D9BackendStatus::DEVICE_CREATION_FAILED;
                }
                else if(FAILED(result)) {
                    backend_status = D3D9BackendStatus::DEVICE_CREATION_FAILED;
                }
                return result;
            }

        private:
            ~Direct3D9On12Fallback() {
                this->on_12->Release();
                this->native->Release();
            }

            LONG references = 1;
            IDirect3D9 *on_12;
            IDirect3D9 *native;
            IDirect3D9 *active;
        };

        IDirect3D9 *WINAPI direct3d_create9_hook(UINT sdk_version) noexcept {
            if(!native_direct3d_create9) {
                backend_status = D3D9BackendStatus::ENUMERATOR_CREATION_FAILED;
                return nullptr;
            }

            auto create_9_on_12 = resolve_direct3d_create9_on_12();
            if(!create_9_on_12) {
                return native_direct3d_create9(sdk_version);
            }

            D3D9On12Args arguments = {};
            arguments.enable_9_on_12 = TRUE;

            auto *on_12 = create_9_on_12(sdk_version, &arguments, 1);
            if(!on_12) {
                backend_status = D3D9BackendStatus::ENUMERATOR_CREATION_FAILED;
                return native_direct3d_create9(sdk_version);
            }
            backend_status = D3D9BackendStatus::ENUMERATOR_CREATED;

            auto *native = native_direct3d_create9(sdk_version);
            if(!native) {
                return on_12;
            }

            auto *fallback = new(std::nothrow) Direct3D9On12Fallback(on_12, native);
            if(!fallback) {
                native->Release();
                return on_12;
            }
            return fallback;
        }

        bool patch_imported_function(const char *module_name,
                                     const char *function_name,
                                     ULONG_PTR replacement,
                                     ULONG_PTR &original) noexcept {
            auto *image = reinterpret_cast<std::byte *>(GetModuleHandleW(nullptr));
            if(!image) {
                return false;
            }

            auto *dos_header = reinterpret_cast<IMAGE_DOS_HEADER *>(image);
            if(dos_header->e_magic != IMAGE_DOS_SIGNATURE || dos_header->e_lfanew <= 0) {
                return false;
            }

            auto *nt_headers = reinterpret_cast<IMAGE_NT_HEADERS *>(image + dos_header->e_lfanew);
            if(nt_headers->Signature != IMAGE_NT_SIGNATURE) {
                return false;
            }

            const auto &import_directory = nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if(import_directory.VirtualAddress == 0) {
                return false;
            }

            FARPROC resolved_function = nullptr;
            if(auto imported_module = GetModuleHandleA(module_name)) {
                resolved_function = GetProcAddress(imported_module, function_name);
            }
            const auto resolved_address = reinterpret_cast<ULONG_PTR>(resolved_function);

            auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(image + import_directory.VirtualAddress);
            for(; descriptor->Name != 0; descriptor++) {
                auto *imported_module_name = reinterpret_cast<const char *>(image + descriptor->Name);
                if(_stricmp(imported_module_name, module_name) != 0 || descriptor->FirstThunk == 0) {
                    continue;
                }

                auto *function_thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(image + descriptor->FirstThunk);
                IMAGE_THUNK_DATA *name_thunk = nullptr;
                if(descriptor->OriginalFirstThunk != 0) {
                    name_thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(image + descriptor->OriginalFirstThunk);
                }

                for(std::size_t index = 0; function_thunk[index].u1.Function != 0; index++) {
                    bool matches_name = false;
                    if(name_thunk && !IMAGE_SNAP_BY_ORDINAL(name_thunk[index].u1.Ordinal)) {
                        auto *import = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(image + name_thunk[index].u1.AddressOfData);
                        matches_name = std::strcmp(reinterpret_cast<const char *>(import->Name), function_name) == 0;
                    }

                    const bool matches_address = resolved_address != 0 &&
                                                 function_thunk[index].u1.Function == resolved_address;
                    if(!matches_name && !matches_address) {
                        continue;
                    }

                    DWORD old_protection;
                    auto *import_address = &function_thunk[index].u1.Function;
                    if(!VirtualProtect(import_address, sizeof(*import_address), PAGE_READWRITE, &old_protection)) {
                        return false;
                    }

                    original = function_thunk[index].u1.Function;
                    function_thunk[index].u1.Function = replacement;

                    DWORD restored_protection;
                    VirtualProtect(import_address, sizeof(*import_address), old_protection, &restored_protection);
                    FlushInstructionCache(GetCurrentProcess(), import_address, sizeof(*import_address));
                    return original != 0;
                }
            }
            return false;
        }

        bool patch_direct3d_create9_import() noexcept {
            ULONG_PTR original = 0;
            if(!patch_imported_function("d3d9.dll",
                                        "Direct3DCreate9",
                                        reinterpret_cast<ULONG_PTR>(direct3d_create9_hook),
                                        original)) {
                return false;
            }

            native_direct3d_create9 = reinterpret_cast<Direct3DCreate9Function>(original);
            return native_direct3d_create9 != nullptr;
        }

        FARPROC WINAPI get_proc_address_hook(HMODULE module, LPCSTR procedure_name) noexcept {
            if(!native_get_proc_address) {
                return nullptr;
            }

            auto resolved = native_get_proc_address(module, procedure_name);
            if(!resolved || !procedure_name || (reinterpret_cast<ULONG_PTR>(procedure_name) >> 16) == 0) {
                return resolved;
            }

            if(std::strcmp(procedure_name, "Direct3DCreate9") != 0) {
                return resolved;
            }

            auto d3d9_module = GetModuleHandleA("d3d9.dll");
            if(!d3d9_module || module != d3d9_module) {
                return resolved;
            }

            static_assert(sizeof(resolved) == sizeof(native_direct3d_create9));
            std::memcpy(&native_direct3d_create9, &resolved, sizeof(native_direct3d_create9));

            auto hook = &direct3d_create9_hook;
            FARPROC replacement = nullptr;
            static_assert(sizeof(hook) == sizeof(replacement));
            std::memcpy(&replacement, &hook, sizeof(replacement));
            return replacement;
        }

        bool patch_get_proc_address_import() noexcept {
            ULONG_PTR original = 0;
            auto hook = &get_proc_address_hook;
            static_assert(sizeof(hook) == sizeof(ULONG_PTR));

            ULONG_PTR replacement = 0;
            std::memcpy(&replacement, &hook, sizeof(replacement));
            if(!patch_imported_function("kernel32.dll", "GetProcAddress", replacement, original)) {
                return false;
            }

            native_get_proc_address = reinterpret_cast<GetProcAddressFunction>(original);
            return native_get_proc_address != nullptr;
        }
    }

    void set_up_d3d9_backend() noexcept {
        auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
        if(!backend || (_stricmp(backend, "9on12") != 0 && _stricmp(backend, "d3d9on12") != 0)) {
            return;
        }

        // Halo builds and wrappers do not all reach Direct3DCreate9 through the
        // same import-table shape. Prefer the direct D3D9 IAT entry, but if it is
        // absent also intercept Halo's dynamic GetProcAddress resolution path.
        if(patch_direct3d_create9_import()) {
            backend_status = D3D9BackendStatus::HOOK_INSTALLED;
            return;
        }

        native_direct3d_create9 = nullptr;
        if(patch_get_proc_address_import()) {
            backend_status = D3D9BackendStatus::HOOK_INSTALLED;
            return;
        }

        native_get_proc_address = nullptr;
        backend_status = D3D9BackendStatus::HOOK_INSTALL_FAILED;
    }

    void report_d3d9_backend_status() noexcept {
        switch(backend_status) {
            case D3D9BackendStatus::DISABLED:
                break;
            case D3D9BackendStatus::HOOK_INSTALLED:
                console_warning("D3D9 backend: 9On12 hook was installed but Direct3DCreate9 was not intercepted; native D3D9 is active.");
                break;
            case D3D9BackendStatus::HOOK_INSTALL_FAILED:
                console_warning("D3D9 backend: 9On12 hook installation failed; native D3D9 is active.");
                break;
            case D3D9BackendStatus::ENTRYPOINT_UNAVAILABLE:
                console_warning("D3D9 backend: Direct3DCreate9On12 is unavailable; native D3D9 is active.");
                break;
            case D3D9BackendStatus::ENUMERATOR_CREATION_FAILED:
                console_warning("D3D9 backend: 9On12 initialization failed; native D3D9 is active.");
                break;
            case D3D9BackendStatus::ENUMERATOR_CREATED:
                console_warning("D3D9 backend: 9On12 was selected, but device activation was not observed.");
                break;
            case D3D9BackendStatus::ON_12_ACTIVE:
                console_output("D3D9 backend: D3D9On12 is active.");
                break;
            case D3D9BackendStatus::NATIVE_FALLBACK:
                console_warning("D3D9 backend: 9On12 did not remain active; native D3D9 fallback is active.");
                break;
            case D3D9BackendStatus::DEVICE_CREATION_FAILED:
                console_error("D3D9 backend: both 9On12 and native D3D9 device creation failed.");
                break;
        }
    }
}
