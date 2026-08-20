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
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_effects.hpp"

namespace Chimera {
    namespace {
        constexpr UINT MAX_D3D9ON12_QUEUES = 2;
        constexpr std::size_t DEVICE_CREATE_VERTEX_BUFFER_INDEX = 26;
        constexpr std::size_t DEVICE_CREATE_INDEX_BUFFER_INDEX = 27;
        constexpr std::size_t DEVICE_DRAW_PRIMITIVE_INDEX = 81;
        constexpr std::size_t DEVICE_DRAW_INDEXED_PRIMITIVE_INDEX = 82;
        constexpr std::size_t DEVICE_DRAW_PRIMITIVE_UP_INDEX = 83;
        constexpr std::size_t DEVICE_DRAW_INDEXED_PRIMITIVE_UP_INDEX = 84;
        constexpr std::size_t BUFFER_LOCK_INDEX = 11;
        constexpr std::size_t MAX_BUFFER_VTABLES = 16;

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
        using CreateVertexBufferFunction = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *);
        using CreateIndexBufferFunction = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *);
        using DrawPrimitiveFunction = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
        using DrawIndexedPrimitiveFunction = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
        using DrawPrimitiveUPFunction = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
        using DrawIndexedPrimitiveUPFunction = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
        using VertexBufferLockFunction = HRESULT (STDMETHODCALLTYPE *)(IDirect3DVertexBuffer9 *, UINT, UINT, void **, DWORD);
        using IndexBufferLockFunction = HRESULT (STDMETHODCALLTYPE *)(IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD);

        Direct3DCreate9Function native_direct3d_create9 = nullptr;
        Direct3DCreate9On12Function direct3d_create9_on_12 = nullptr;
        GetProcAddressFunction native_get_proc_address = nullptr;
        CreateVertexBufferFunction native_create_vertex_buffer = nullptr;
        CreateIndexBufferFunction native_create_index_buffer = nullptr;
        DrawPrimitiveFunction native_draw_primitive = nullptr;
        DrawIndexedPrimitiveFunction native_draw_indexed_primitive = nullptr;
        DrawPrimitiveUPFunction native_draw_primitive_up = nullptr;
        DrawIndexedPrimitiveUPFunction native_draw_indexed_primitive_up = nullptr;

        struct BufferVtablePatch {
            ULONG_PTR *vtable = nullptr;
            ULONG_PTR original_lock = 0;
        };

        BufferVtablePatch vertex_buffer_vtables[MAX_BUFFER_VTABLES] = {};
        BufferVtablePatch index_buffer_vtables[MAX_BUFFER_VTABLES] = {};
        std::size_t vertex_buffer_vtable_count = 0;
        std::size_t index_buffer_vtable_count = 0;
        bool buffer_lock_compat_ready = false;
        bool selective_software_vertex_processing_ready = false;
        bool selective_software_vertex_processing_active = false;
        bool mixed_vertex_processing_forced = false;

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

        bool should_use_software_vertex_processing(IDirect3DVertexShader9 *shader) noexcept {
            // Full SWVP fixed every corrupt vehicle/weapon/model draw. Keep the
            // known-good heavy world/screen paths on HWVP and use SWVP elsewhere.
            if(!shader || !vertex_shaders) {
                return true;
            }

            // Halo's BSP/environment geometry was already correct on D3D9On12 and
            // is by far the most expensive geometry to move to the CPU.
            for(std::size_t index = VSH_ENVIRONMENT_DIFFUSE_LIGHT; index <= VSH_ENVIRONMENT_TEXTURE; index++) {
                if(vertex_shaders[index].shader == shader) {
                    return false;
                }
            }

            // Full-screen/HUD passes do not exhibit the exploding-vertex issue and
            // are safe to leave on hardware processing.
            for(std::size_t index = VSH_SCREEN; index <= VSH_SCREEN2; index++) {
                if(vertex_shaders[index].shader == shader) {
                    return false;
                }
            }

            if(vertex_shaders[VSH_CONVOLUTION].shader == shader) {
                return false;
            }

            return true;
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

        bool write_vtable_entry(ULONG_PTR *entry, ULONG_PTR replacement, ULONG_PTR &original) noexcept {
            if(!entry) {
                return false;
            }
            if(*entry == replacement) {
                return original != 0;
            }

            DWORD old_protection;
            if(!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE, &old_protection)) {
                return false;
            }

            if(original == 0) {
                original = *entry;
            }
            *entry = replacement;

            DWORD restored_protection;
            VirtualProtect(entry, sizeof(*entry), old_protection, &restored_protection);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(*entry));
            return original != 0;
        }

        ULONG_PTR find_original_lock(void *buffer, BufferVtablePatch *patches, std::size_t patch_count) noexcept {
            if(!buffer) {
                return 0;
            }
            auto *vtable = *reinterpret_cast<ULONG_PTR **>(buffer);
            if(!vtable) {
                return 0;
            }
            for(std::size_t i = 0; i < patch_count; i++) {
                if(patches[i].vtable == vtable) {
                    return patches[i].original_lock;
                }
            }
            return 0;
        }

        bool patch_buffer_lock(void *buffer,
                               ULONG_PTR replacement,
                               BufferVtablePatch *patches,
                               std::size_t &patch_count) noexcept {
            if(!buffer) {
                return false;
            }

            auto *vtable = *reinterpret_cast<ULONG_PTR **>(buffer);
            if(!vtable) {
                return false;
            }

            for(std::size_t i = 0; i < patch_count; i++) {
                if(patches[i].vtable == vtable) {
                    return vtable[BUFFER_LOCK_INDEX] == replacement;
                }
            }

            if(patch_count >= MAX_BUFFER_VTABLES) {
                return false;
            }

            ULONG_PTR original = 0;
            if(!write_vtable_entry(&vtable[BUFFER_LOCK_INDEX], replacement, original)) {
                return false;
            }

            patches[patch_count].vtable = vtable;
            patches[patch_count].original_lock = original;
            patch_count++;
            return true;
        }

        HRESULT STDMETHODCALLTYPE vertex_buffer_lock_hook(IDirect3DVertexBuffer9 *buffer,
                                                          UINT offset_to_lock,
                                                          UINT size_to_lock,
                                                          void **data,
                                                          DWORD flags) noexcept {
            auto original_address = find_original_lock(buffer, vertex_buffer_vtables, vertex_buffer_vtable_count);
            auto original = reinterpret_cast<VertexBufferLockFunction>(original_address);
            if(!original) {
                return D3DERR_INVALIDCALL;
            }

            D3DVERTEXBUFFER_DESC desc = {};
            if(!data || FAILED(buffer->GetDesc(&desc)) || desc.Pool != D3DPOOL_DEFAULT || offset_to_lock > desc.Size) {
                return original(buffer, offset_to_lock, size_to_lock, data, flags);
            }

            // Halo passes incorrect lock ranges for some DEFAULT-pool buffers.
            // Native D3D9 drivers historically tolerate this. D3D9On12 can copy
            // only the advertised range, leaving stale/missing geometry. Match
            // DXVK's Halo compatibility behavior: dirty/map the whole buffer but
            // preserve the pointer offset that Halo expects to write through.
            void *base = nullptr;
            auto result = original(buffer, 0, 0, &base, flags);
            if(SUCCEEDED(result) && base) {
                *data = reinterpret_cast<std::byte *>(base) + offset_to_lock;
            }
            return result;
        }

        HRESULT STDMETHODCALLTYPE index_buffer_lock_hook(IDirect3DIndexBuffer9 *buffer,
                                                         UINT offset_to_lock,
                                                         UINT size_to_lock,
                                                         void **data,
                                                         DWORD flags) noexcept {
            auto original_address = find_original_lock(buffer, index_buffer_vtables, index_buffer_vtable_count);
            auto original = reinterpret_cast<IndexBufferLockFunction>(original_address);
            if(!original) {
                return D3DERR_INVALIDCALL;
            }

            D3DINDEXBUFFER_DESC desc = {};
            if(!data || FAILED(buffer->GetDesc(&desc)) || desc.Pool != D3DPOOL_DEFAULT || offset_to_lock > desc.Size) {
                return original(buffer, offset_to_lock, size_to_lock, data, flags);
            }

            void *base = nullptr;
            auto result = original(buffer, 0, 0, &base, flags);
            if(SUCCEEDED(result) && base) {
                *data = reinterpret_cast<std::byte *>(base) + offset_to_lock;
            }
            return result;
        }

        HRESULT STDMETHODCALLTYPE create_vertex_buffer_hook(IDirect3DDevice9 *device,
                                                            UINT length,
                                                            DWORD usage,
                                                            DWORD fvf,
                                                            D3DPOOL pool,
                                                            IDirect3DVertexBuffer9 **buffer,
                                                            HANDLE *shared_handle) noexcept {
            if(!native_create_vertex_buffer) {
                return D3DERR_INVALIDCALL;
            }

            // Mixed vertex processing requires buffers that may ever be consumed
            // by software vertex processing to opt in at creation time.
            auto compatible_usage = usage | D3DUSAGE_SOFTWAREPROCESSING;
            auto result = native_create_vertex_buffer(device, length, compatible_usage, fvf, pool, buffer, shared_handle);
            if(SUCCEEDED(result) && buffer && *buffer) {
                patch_buffer_lock(*buffer,
                                  reinterpret_cast<ULONG_PTR>(vertex_buffer_lock_hook),
                                  vertex_buffer_vtables,
                                  vertex_buffer_vtable_count);
            }
            return result;
        }

        HRESULT STDMETHODCALLTYPE create_index_buffer_hook(IDirect3DDevice9 *device,
                                                           UINT length,
                                                           DWORD usage,
                                                           D3DFORMAT format,
                                                           D3DPOOL pool,
                                                           IDirect3DIndexBuffer9 **buffer,
                                                           HANDLE *shared_handle) noexcept {
            if(!native_create_index_buffer) {
                return D3DERR_INVALIDCALL;
            }

            auto compatible_usage = usage | D3DUSAGE_SOFTWAREPROCESSING;
            auto result = native_create_index_buffer(device, length, compatible_usage, format, pool, buffer, shared_handle);
            if(SUCCEEDED(result) && buffer && *buffer) {
                patch_buffer_lock(*buffer,
                                  reinterpret_cast<ULONG_PTR>(index_buffer_lock_hook),
                                  index_buffer_vtables,
                                  index_buffer_vtable_count);
            }
            return result;
        }

        bool prepare_vertex_processing_for_draw(IDirect3DDevice9 *device) noexcept {
            if(!device || !selective_software_vertex_processing_ready) {
                return false;
            }

            // Halo uses D3D9 state blocks. Applying a state block can restore a
            // vertex shader without passing through IDirect3DDevice9::SetVertexShader,
            // while SetSoftwareVertexProcessing is explicitly not state-block state.
            // Query the shader at the actual draw boundary so the processing mode
            // always matches the state that D3D9On12 is about to consume.
            IDirect3DVertexShader9 *shader = nullptr;
            auto shader_result = device->GetVertexShader(&shader);
            if(FAILED(shader_result)) {
                return false;
            }

            const bool use_software = should_use_software_vertex_processing(shader);
            if(shader) {
                shader->Release();
            }

            if(use_software == selective_software_vertex_processing_active) {
                return true;
            }

            auto mode_result = device->SetSoftwareVertexProcessing(use_software ? TRUE : FALSE);
            if(FAILED(mode_result)) {
                selective_software_vertex_processing_ready = false;
                return false;
            }

            selective_software_vertex_processing_active = use_software;
            return true;
        }

        HRESULT STDMETHODCALLTYPE draw_primitive_hook(IDirect3DDevice9 *device,
                                                      D3DPRIMITIVETYPE primitive_type,
                                                      UINT start_vertex,
                                                      UINT primitive_count) noexcept {
            prepare_vertex_processing_for_draw(device);
            return native_draw_primitive
                       ? native_draw_primitive(device, primitive_type, start_vertex, primitive_count)
                       : D3DERR_INVALIDCALL;
        }

        HRESULT STDMETHODCALLTYPE draw_indexed_primitive_hook(IDirect3DDevice9 *device,
                                                              D3DPRIMITIVETYPE primitive_type,
                                                              INT base_vertex_index,
                                                              UINT min_vertex_index,
                                                              UINT num_vertices,
                                                              UINT start_index,
                                                              UINT primitive_count) noexcept {
            prepare_vertex_processing_for_draw(device);
            return native_draw_indexed_primitive
                       ? native_draw_indexed_primitive(device,
                                                       primitive_type,
                                                       base_vertex_index,
                                                       min_vertex_index,
                                                       num_vertices,
                                                       start_index,
                                                       primitive_count)
                       : D3DERR_INVALIDCALL;
        }

        HRESULT STDMETHODCALLTYPE draw_primitive_up_hook(IDirect3DDevice9 *device,
                                                         D3DPRIMITIVETYPE primitive_type,
                                                         UINT primitive_count,
                                                         const void *vertex_stream_zero_data,
                                                         UINT vertex_stream_zero_stride) noexcept {
            prepare_vertex_processing_for_draw(device);
            return native_draw_primitive_up
                       ? native_draw_primitive_up(device,
                                                  primitive_type,
                                                  primitive_count,
                                                  vertex_stream_zero_data,
                                                  vertex_stream_zero_stride)
                       : D3DERR_INVALIDCALL;
        }

        HRESULT STDMETHODCALLTYPE draw_indexed_primitive_up_hook(IDirect3DDevice9 *device,
                                                                 D3DPRIMITIVETYPE primitive_type,
                                                                 UINT min_vertex_index,
                                                                 UINT num_vertices,
                                                                 UINT primitive_count,
                                                                 const void *index_data,
                                                                 D3DFORMAT index_data_format,
                                                                 const void *vertex_stream_zero_data,
                                                                 UINT vertex_stream_zero_stride) noexcept {
            prepare_vertex_processing_for_draw(device);
            return native_draw_indexed_primitive_up
                       ? native_draw_indexed_primitive_up(device,
                                                          primitive_type,
                                                          min_vertex_index,
                                                          num_vertices,
                                                          primitive_count,
                                                          index_data,
                                                          index_data_format,
                                                          vertex_stream_zero_data,
                                                          vertex_stream_zero_stride)
                       : D3DERR_INVALIDCALL;
        }

        bool install_buffer_lock_compat(IDirect3DDevice9 *device) noexcept {
            if(!device) {
                return false;
            }

            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) {
                return false;
            }

            ULONG_PTR original_vertex = reinterpret_cast<ULONG_PTR>(native_create_vertex_buffer);
            ULONG_PTR original_index = reinterpret_cast<ULONG_PTR>(native_create_index_buffer);
            ULONG_PTR original_draw_primitive = reinterpret_cast<ULONG_PTR>(native_draw_primitive);
            ULONG_PTR original_draw_indexed_primitive = reinterpret_cast<ULONG_PTR>(native_draw_indexed_primitive);
            ULONG_PTR original_draw_primitive_up = reinterpret_cast<ULONG_PTR>(native_draw_primitive_up);
            ULONG_PTR original_draw_indexed_primitive_up = reinterpret_cast<ULONG_PTR>(native_draw_indexed_primitive_up);

            auto vertex_replacement = reinterpret_cast<ULONG_PTR>(create_vertex_buffer_hook);
            auto index_replacement = reinterpret_cast<ULONG_PTR>(create_index_buffer_hook);
            auto draw_primitive_replacement = reinterpret_cast<ULONG_PTR>(draw_primitive_hook);
            auto draw_indexed_primitive_replacement = reinterpret_cast<ULONG_PTR>(draw_indexed_primitive_hook);
            auto draw_primitive_up_replacement = reinterpret_cast<ULONG_PTR>(draw_primitive_up_hook);
            auto draw_indexed_primitive_up_replacement = reinterpret_cast<ULONG_PTR>(draw_indexed_primitive_up_hook);

            bool vertex_ok = false;
            if(vtable[DEVICE_CREATE_VERTEX_BUFFER_INDEX] == vertex_replacement && native_create_vertex_buffer) {
                vertex_ok = true;
            }
            else if(write_vtable_entry(&vtable[DEVICE_CREATE_VERTEX_BUFFER_INDEX], vertex_replacement, original_vertex)) {
                native_create_vertex_buffer = reinterpret_cast<CreateVertexBufferFunction>(original_vertex);
                vertex_ok = native_create_vertex_buffer != nullptr;
            }

            bool index_ok = false;
            if(vtable[DEVICE_CREATE_INDEX_BUFFER_INDEX] == index_replacement && native_create_index_buffer) {
                index_ok = true;
            }
            else if(write_vtable_entry(&vtable[DEVICE_CREATE_INDEX_BUFFER_INDEX], index_replacement, original_index)) {
                native_create_index_buffer = reinterpret_cast<CreateIndexBufferFunction>(original_index);
                index_ok = native_create_index_buffer != nullptr;
            }

            bool draw_primitive_ok = false;
            if(vtable[DEVICE_DRAW_PRIMITIVE_INDEX] == draw_primitive_replacement && native_draw_primitive) {
                draw_primitive_ok = true;
            }
            else if(write_vtable_entry(&vtable[DEVICE_DRAW_PRIMITIVE_INDEX], draw_primitive_replacement, original_draw_primitive)) {
                native_draw_primitive = reinterpret_cast<DrawPrimitiveFunction>(original_draw_primitive);
                draw_primitive_ok = native_draw_primitive != nullptr;
            }

            bool draw_indexed_ok = false;
            if(vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_INDEX] == draw_indexed_primitive_replacement && native_draw_indexed_primitive) {
                draw_indexed_ok = true;
            }
            else if(write_vtable_entry(&vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_INDEX], draw_indexed_primitive_replacement, original_draw_indexed_primitive)) {
                native_draw_indexed_primitive = reinterpret_cast<DrawIndexedPrimitiveFunction>(original_draw_indexed_primitive);
                draw_indexed_ok = native_draw_indexed_primitive != nullptr;
            }

            bool draw_up_ok = false;
            if(vtable[DEVICE_DRAW_PRIMITIVE_UP_INDEX] == draw_primitive_up_replacement && native_draw_primitive_up) {
                draw_up_ok = true;
            }
            else if(write_vtable_entry(&vtable[DEVICE_DRAW_PRIMITIVE_UP_INDEX], draw_primitive_up_replacement, original_draw_primitive_up)) {
                native_draw_primitive_up = reinterpret_cast<DrawPrimitiveUPFunction>(original_draw_primitive_up);
                draw_up_ok = native_draw_primitive_up != nullptr;
            }

            bool draw_indexed_up_ok = false;
            if(vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_UP_INDEX] == draw_indexed_primitive_up_replacement && native_draw_indexed_primitive_up) {
                draw_indexed_up_ok = true;
            }
            else if(write_vtable_entry(&vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_UP_INDEX], draw_indexed_primitive_up_replacement, original_draw_indexed_primitive_up)) {
                native_draw_indexed_primitive_up = reinterpret_cast<DrawIndexedPrimitiveUPFunction>(original_draw_indexed_primitive_up);
                draw_indexed_up_ok = native_draw_indexed_primitive_up != nullptr;
            }

            selective_software_vertex_processing_ready = draw_primitive_ok &&
                                                          draw_indexed_ok &&
                                                          draw_up_ok &&
                                                          draw_indexed_up_ok;
            return vertex_ok && index_ok && selective_software_vertex_processing_ready;
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
                DWORD create_behavior_flags = behavior_flags;
                if(this->active == this->on_12) {
                    // Full SWVP proved the geometry itself is sound, but costs too
                    // much CPU. Use a mixed device and choose SWVP/HWVP at each
                    // actual draw call, after any Halo state block has been applied.
                    create_behavior_flags &= ~(D3DCREATE_HARDWARE_VERTEXPROCESSING |
                                               D3DCREATE_MIXED_VERTEXPROCESSING |
                                               D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                                               D3DCREATE_PUREDEVICE);
                    create_behavior_flags |= D3DCREATE_MIXED_VERTEXPROCESSING;
                    mixed_vertex_processing_forced = true;
                    selective_software_vertex_processing_active = true;
                }

                auto result = this->active->CreateDevice(adapter,
                                                         device_type,
                                                         focus_window,
                                                         create_behavior_flags,
                                                         presentation_parameters,
                                                         device);

                if(SUCCEEDED(result) && this->active == this->on_12) {
                    const bool on_12_active = device && is_d3d9_on_12_device(*device);
                    backend_status = on_12_active
                                         ? D3D9BackendStatus::ON_12_ACTIVE
                                         : D3D9BackendStatus::NATIVE_FALLBACK;
                    if(on_12_active && device && *device) {
                        // Start safe. The draw hooks switch known-good environment
                        // and screen passes back to HWVP only at their draw boundary.
                        auto mode_result = (*device)->SetSoftwareVertexProcessing(TRUE);
                        selective_software_vertex_processing_active = SUCCEEDED(mode_result);
                        buffer_lock_compat_ready = install_buffer_lock_compat(*device);
                    }
                }

                // If 9On12 cannot create Halo's device, retry once using the
                // exact native D3D9 entrypoint that Halo originally imported.
                if(FAILED(result) && this->active == this->on_12 && this->native) {
                    if(device && *device) {
                        (*device)->Release();
                        *device = nullptr;
                    }
                    this->active = this->native;
                    mixed_vertex_processing_forced = false;
                    selective_software_vertex_processing_ready = false;
                    selective_software_vertex_processing_active = false;
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
                if(buffer_lock_compat_ready && selective_software_vertex_processing_ready && mixed_vertex_processing_forced) {
                    console_output("D3D9 backend: D3D9On12 is active with Halo buffer-lock compatibility and draw-time selective software vertex processing.");
                }
                else if(buffer_lock_compat_ready) {
                    console_output("D3D9 backend: D3D9On12 is active with Halo buffer-lock compatibility.");
                }
                else {
                    console_warning("D3D9 backend: D3D9On12 is active, but the selective Halo compatibility path could not be installed.");
                }
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
