// SPDX-License-Identifier: GPL-3.0-only

#include <cstddef>

#include "rasterizer_transparent_geometry.hpp"
#include "../signature/hook.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_defs.hpp"
#include "../event/map_load.hpp"
#include "../event/d3d9_reset.hpp"

namespace Chimera {

    extern DynamicVertices *dynamic_vertices;
    extern "C" void *rasterizer_transparent_geometry_group_draw_func;

    namespace {
        constexpr std::size_t DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX = 82;
        constexpr std::size_t INDEX_BUFFER_LOCK_VTABLE_INDEX = 11;
        constexpr std::size_t MAX_DEVICE_VTABLE_HOOKS = 4;
        constexpr std::size_t MAX_INDEX_VTABLE_HOOKS = 4;

        using DrawIndexedPrimitiveFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT
        );
        using IndexBufferLockFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD
        );

        struct SuspectGroupContext {
            bool active = false;
        };

        struct DeviceVtableHook {
            void **vtable = nullptr;
            DrawIndexedPrimitiveFunction original = nullptr;
        };

        struct IndexVtableHook {
            void **vtable = nullptr;
            IndexBufferLockFunction original = nullptr;
        };

        struct TargetIndexBuffer {
            IDirect3DIndexBuffer9 *buffer = nullptr;
            bool dynamic = false;
            bool previous_lock_valid = false;
        };

        static SuspectGroupContext suspect_context = {};
        static TargetIndexBuffer target_index_buffer = {};
        static DeviceVtableHook device_vtable_hooks[MAX_DEVICE_VTABLE_HOOKS] = {};
        static std::size_t device_vtable_hook_count = 0;
        static IndexVtableHook index_vtable_hooks[MAX_INDEX_VTABLE_HOOKS] = {};
        static std::size_t index_vtable_hook_count = 0;

        static Hook group_draw_hook;
        static const void *group_draw_original = nullptr;

        static bool is_environment_chicago_dynamic_group(TransparentGeometryGroup *group) noexcept {
            if(!group || group->dynamic_triangle_buffer_index == -1 || !group->shader) {
                return false;
            }
            if(reinterpret_cast<_shader *>(group->shader)->type != SHADER_TYPE_TRANSPARENT_CHICAGO) {
                return false;
            }
            return rasterizer_transparent_geometry_get_primary_vertex_type(group)
                == RASTERIZER_VERTEX_TYPE_ENVIRONMENT_UNCOMPRESSED;
        }

        static DeviceVtableHook *find_device_vtable_hook(IDirect3DDevice9 *device) noexcept {
            if(!device) {
                return nullptr;
            }
            auto **vtable = *reinterpret_cast<void ***>(device);
            for(std::size_t i = 0; i < device_vtable_hook_count; i++) {
                if(device_vtable_hooks[i].vtable == vtable) {
                    return &device_vtable_hooks[i];
                }
            }
            return nullptr;
        }

        static IndexVtableHook *find_index_vtable_hook(IDirect3DIndexBuffer9 *buffer) noexcept {
            if(!buffer) {
                return nullptr;
            }
            auto **vtable = *reinterpret_cast<void ***>(buffer);
            for(std::size_t i = 0; i < index_vtable_hook_count; i++) {
                if(index_vtable_hooks[i].vtable == vtable) {
                    return &index_vtable_hooks[i];
                }
            }
            return nullptr;
        }

        static HRESULT STDMETHODCALLTYPE draw_indexed_primitive_fix(
            IDirect3DDevice9 *device,
            D3DPRIMITIVETYPE primitive_type,
            INT base_vertex_index,
            UINT min_vertex_index,
            UINT num_vertices,
            UINT start_index,
            UINT primitive_count
        ) noexcept;

        static HRESULT STDMETHODCALLTYPE index_buffer_lock_fix(
            IDirect3DIndexBuffer9 *buffer,
            UINT offset_to_lock,
            UINT size_to_lock,
            void **data,
            DWORD flags
        ) noexcept;

        static bool ensure_device_draw_hook() noexcept {
            if(!global_d3d9_device || !*global_d3d9_device) {
                return false;
            }

            auto *device = *global_d3d9_device;
            auto **vtable = *reinterpret_cast<void ***>(device);
            if(!vtable) {
                return false;
            }

            void *replacement = reinterpret_cast<void *>(draw_indexed_primitive_fix);
            if(auto *existing = find_device_vtable_hook(device)) {
                if(vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX] == replacement) {
                    return true;
                }
                if(vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX]
                    != reinterpret_cast<void *>(existing->original)) {
                    return false;
                }
                overwrite(&vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX], replacement);
                return vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX] == replacement;
            }

            if(device_vtable_hook_count >= MAX_DEVICE_VTABLE_HOOKS) {
                return false;
            }

            auto original = reinterpret_cast<DrawIndexedPrimitiveFunction>(
                vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX]
            );
            if(!original) {
                return false;
            }

            overwrite(&vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX], replacement);
            if(vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX] != replacement) {
                return false;
            }

            auto &hook = device_vtable_hooks[device_vtable_hook_count++];
            hook.vtable = vtable;
            hook.original = original;
            return true;
        }

        static void restore_device_draw_hook(IDirect3DDevice9 *device) noexcept {
            auto *hook = find_device_vtable_hook(device);
            if(!hook || !hook->vtable || !hook->original) {
                return;
            }

            void *replacement = reinterpret_cast<void *>(draw_indexed_primitive_fix);
            if(hook->vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX] == replacement) {
                void *original = reinterpret_cast<void *>(hook->original);
                overwrite(&hook->vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX], original);
            }
        }

        static bool ensure_index_buffer_lock_hook(IDirect3DIndexBuffer9 *buffer) noexcept {
            if(!buffer) {
                return false;
            }

            auto **vtable = *reinterpret_cast<void ***>(buffer);
            if(!vtable) {
                return false;
            }

            void *replacement = reinterpret_cast<void *>(index_buffer_lock_fix);
            if(auto *existing = find_index_vtable_hook(buffer)) {
                if(vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX] == replacement) {
                    return true;
                }
                if(vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX]
                    != reinterpret_cast<void *>(existing->original)) {
                    return false;
                }
                overwrite(&vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX], replacement);
                return vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX] == replacement;
            }

            if(index_vtable_hook_count >= MAX_INDEX_VTABLE_HOOKS) {
                return false;
            }

            auto original = reinterpret_cast<IndexBufferLockFunction>(
                vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX]
            );
            if(!original) {
                return false;
            }

            overwrite(&vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX], replacement);
            if(vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX] != replacement) {
                return false;
            }

            auto &hook = index_vtable_hooks[index_vtable_hook_count++];
            hook.vtable = vtable;
            hook.original = original;
            return true;
        }

        static bool register_target_index_buffer(IDirect3DIndexBuffer9 *buffer) noexcept {
            if(!buffer) {
                return false;
            }

            if(target_index_buffer.buffer == buffer) {
                return ensure_index_buffer_lock_hook(buffer);
            }

            D3DINDEXBUFFER_DESC desc = {};
            if(FAILED(buffer->GetDesc(&desc))) {
                return false;
            }

            if(!ensure_index_buffer_lock_hook(buffer)) {
                return false;
            }

            target_index_buffer.buffer = buffer;
            target_index_buffer.dynamic = (desc.Usage & D3DUSAGE_DYNAMIC) != 0;
            target_index_buffer.previous_lock_valid = false;
            return true;
        }

        static HRESULT STDMETHODCALLTYPE draw_indexed_primitive_fix(
            IDirect3DDevice9 *device,
            D3DPRIMITIVETYPE primitive_type,
            INT base_vertex_index,
            UINT min_vertex_index,
            UINT num_vertices,
            UINT start_index,
            UINT primitive_count
        ) noexcept {
            auto *hook = find_device_vtable_hook(device);
            if(!hook || !hook->original) {
                return D3DERR_INVALIDCALL;
            }

            if(suspect_context.active && !target_index_buffer.buffer) {
                IDirect3DIndexBuffer9 *bound_buffer = nullptr;
                if(SUCCEEDED(device->GetIndices(&bound_buffer)) && bound_buffer) {
                    const bool registered = register_target_index_buffer(bound_buffer);
                    bound_buffer->Release();
                    if(registered) {
                        restore_device_draw_hook(device);
                    }
                }
            }

            return hook->original(
                device,
                primitive_type,
                base_vertex_index,
                min_vertex_index,
                num_vertices,
                start_index,
                primitive_count
            );
        }

        static HRESULT STDMETHODCALLTYPE index_buffer_lock_fix(
            IDirect3DIndexBuffer9 *buffer,
            UINT offset_to_lock,
            UINT size_to_lock,
            void **data,
            DWORD flags
        ) noexcept {
            auto *hook = find_index_vtable_hook(buffer);
            if(!hook || !hook->original) {
                return D3DERR_INVALIDCALL;
            }

            if(buffer != target_index_buffer.buffer) {
                return hook->original(buffer, offset_to_lock, size_to_lock, data, flags);
            }

            DWORD effective_flags = flags;
            const bool discard = (flags & D3DLOCK_DISCARD) != 0;
            const bool nooverwrite = (flags & D3DLOCK_NOOVERWRITE) != 0;

            if(target_index_buffer.dynamic
                && target_index_buffer.previous_lock_valid
                && offset_to_lock == 0
                && nooverwrite
                && !discard) {
                effective_flags &= ~static_cast<DWORD>(D3DLOCK_NOOVERWRITE);
                effective_flags |= D3DLOCK_DISCARD;
            }

            const HRESULT result = hook->original(
                buffer,
                offset_to_lock,
                size_to_lock,
                data,
                effective_flags
            );

            if(SUCCEEDED(result)) {
                target_index_buffer.previous_lock_valid = true;
            }
            return result;
        }

        static void clear_target_index_buffer() noexcept {
            target_index_buffer = {};
            suspect_context = {};
        }

        static void clear_target_index_buffer_on_reset(
            LPDIRECT3DDEVICE9,
            D3DPRESENT_PARAMETERS *
        ) noexcept {
            clear_target_index_buffer();
        }

        static void group_draw_index_buffer_fix(TransparentGeometryGroup *group, bool is_dirty) noexcept {
            using GroupDrawFunction = void (*)(TransparentGeometryGroup *, bool);
            auto original = reinterpret_cast<GroupDrawFunction>(const_cast<void *>(group_draw_original));
            if(!original) {
                return;
            }

            if(target_index_buffer.buffer || !is_environment_chicago_dynamic_group(group)) {
                original(group, is_dirty);
                return;
            }

            if(!ensure_device_draw_hook()) {
                original(group, is_dirty);
                return;
            }

            const auto previous_context = suspect_context;
            suspect_context.active = true;
            original(group, is_dirty);
            suspect_context = previous_context;
        }
    }

    short rasterizer_dynamic_vertices_get_type(long dynamic_vertex_buffer_index) noexcept {
        if(dynamic_vertex_buffer_index < 0 || !dynamic_vertices || dynamic_vertex_buffer_index >= dynamic_vertices->buffer_count) {
            return -1;
        }
        return dynamic_vertices->buffers[dynamic_vertex_buffer_index].type;
    }

    short rasterizer_transparent_geometry_get_primary_vertex_type(TransparentGeometryGroup *group) noexcept {
        if(!group) {
            return -1;
        }
        if(group->vertex_buffers) {
            return group->vertex_buffers[0].type;
        }
        if(group->dynamic_vertex_buffer_index != -1) {
            return rasterizer_dynamic_vertices_get_type(group->dynamic_vertex_buffer_index);
        }
        return -1;
    }

    void set_up_environment_transparent_index_buffer_fix() noexcept {
        static bool installed = false;
        if(installed || !rasterizer_transparent_geometry_group_draw_func) {
            return;
        }

        write_function_override(
            rasterizer_transparent_geometry_group_draw_func,
            group_draw_hook,
            reinterpret_cast<const void *>(group_draw_index_buffer_fix),
            &group_draw_original
        );

        if(group_draw_original) {
            add_map_load_event(clear_target_index_buffer, EVENT_PRIORITY_BEFORE);
            add_d3d9_reset_event(clear_target_index_buffer_on_reset, EVENT_PRIORITY_BEFORE);
            installed = true;
        }
    }

}
