// SPDX-License-Identifier: GPL-3.0-only

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "rasterizer_transparent_geometry.hpp"
#include "../signature/hook.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_defs.hpp"
#include "../output/output.hpp"
#include "../event/command.hpp"


namespace Chimera {

    extern DynamicVertices *dynamic_vertices;
    extern "C" void *rasterizer_transparent_geometry_group_draw_func;

    namespace {
        constexpr std::size_t DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX = 82;
        constexpr std::size_t INDEX_BUFFER_LOCK_VTABLE_INDEX = 11;
        constexpr std::size_t INDEX_BUFFER_UNLOCK_VTABLE_INDEX = 12;
        constexpr std::size_t MAX_DEVICE_VTABLE_HOOKS = 4;
        constexpr std::size_t MAX_INDEX_VTABLE_HOOKS = 4;
        constexpr std::size_t MAX_TARGET_INDEX_BUFFERS = 8;

        using DrawIndexedPrimitiveFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT
        );
        using IndexBufferLockFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD
        );
        using IndexBufferUnlockFunction = HRESULT (STDMETHODCALLTYPE *)(IDirect3DIndexBuffer9 *);

        struct SuspectGroupContext {
            bool active = false;
            long dynamic_triangle_buffer_index = -1;
            long first_triangle_index = 0;
            long triangle_count = 0;
        };

        struct DeviceVtableHook {
            void **vtable = nullptr;
            DrawIndexedPrimitiveFunction original = nullptr;
        };

        struct IndexVtableHook {
            void **vtable = nullptr;
            IndexBufferLockFunction original_lock = nullptr;
            IndexBufferUnlockFunction original_unlock = nullptr;
        };

        struct TargetIndexBuffer {
            IDirect3DIndexBuffer9 *buffer = nullptr;
            bool desc_valid = false;
            D3DINDEXBUFFER_DESC desc = {};

            std::uint64_t draws = 0;
            std::uint64_t locks = 0;
            std::uint64_t unlocks = 0;
            long outstanding_locks = 0;
            long max_outstanding_locks = 0;

            bool last_draw_range_valid = false;
            std::uint64_t last_draw_byte_start = 0;
            std::uint64_t last_draw_byte_end = 0;
            std::int64_t last_draw_frame = -1;

            bool last_lock_range_valid = false;
            std::uint64_t last_lock_byte_start = 0;
            std::uint64_t last_lock_byte_end = 0;
            std::int64_t last_lock_frame = -1;
            DWORD last_lock_flags = 0;
        };

        struct ChicagoIndexStats {
            std::uint64_t group_draw_calls = 0;
            std::uint64_t suspect_group_calls = 0;
            std::uint64_t suspect_groups_without_dip = 0;
            std::uint64_t suspect_groups_multiple_dips = 0;

            std::uint64_t device_hook_failures = 0;
            std::uint64_t device_vtable_overflow = 0;
            std::uint64_t index_vtable_hook_failures = 0;
            std::uint64_t index_vtable_overflow = 0;

            std::uint64_t suspect_dip_calls = 0;
            std::uint64_t triangle_list_dip_calls = 0;
            std::uint64_t other_primitive_dip_calls = 0;
            std::uint64_t get_indices_failures = 0;
            std::uint64_t no_bound_index_buffer = 0;
            std::uint64_t target_overflow = 0;
            std::uint64_t target_desc_failures = 0;
            std::uint64_t bound_index_buffer_changes = 0;
            std::uint64_t draw_with_outstanding_lock = 0;

            std::uint64_t start_matches_first_triangle = 0;
            std::uint64_t start_matches_first_triangle_x3 = 0;
            std::uint64_t start_matches_neither = 0;
            std::uint64_t primitive_count_matches = 0;
            std::uint64_t primitive_count_mismatches = 0;
            std::uint64_t draw_range_out_of_bounds = 0;
            std::uint64_t draw_range_unknown = 0;

            bool dip_range_initialized = false;
            INT min_base_vertex = 0;
            INT max_base_vertex = 0;
            UINT min_start_index = 0;
            UINT max_start_index = 0;
            UINT min_primitive_count = 0;
            UINT max_primitive_count = 0;
            UINT min_num_vertices = 0;
            UINT max_num_vertices = 0;

            std::uint64_t target_lock_calls = 0;
            std::uint64_t target_unlock_calls = 0;
            std::uint64_t lock_failures = 0;
            std::uint64_t unlock_failures = 0;
            std::uint64_t discard_locks = 0;
            std::uint64_t nooverwrite_locks = 0;
            std::uint64_t discard_and_nooverwrite_locks = 0;
            std::uint64_t neither_streaming_flag_locks = 0;
            std::uint64_t readonly_locks = 0;
            std::uint64_t whole_buffer_locks = 0;
            std::uint64_t zero_size_nonzero_offset_locks = 0;
            std::uint64_t lock_range_out_of_bounds = 0;
            std::uint64_t lock_range_unknown = 0;
            std::uint64_t lock_overlaps_last_draw_same_frame = 0;
            std::uint64_t nooverwrite_overlaps_last_draw_same_frame = 0;
            std::uint64_t discard_overlaps_last_draw_same_frame = 0;
            std::uint64_t unlock_without_lock = 0;

            bool lock_range_initialized = false;
            UINT min_lock_offset = 0;
            UINT max_lock_offset = 0;
            UINT min_lock_size = 0;
            UINT max_lock_size = 0;
            std::uint64_t max_lock_end = 0;
        };

        static ChicagoIndexStats stats = {};
        static SuspectGroupContext suspect_context = {};
        static TargetIndexBuffer target_index_buffers[MAX_TARGET_INDEX_BUFFERS] = {};
        static std::size_t target_index_buffer_count = 0;
        static IDirect3DIndexBuffer9 *last_suspect_index_buffer = nullptr;

        static DeviceVtableHook device_vtable_hooks[MAX_DEVICE_VTABLE_HOOKS] = {};
        static std::size_t device_vtable_hook_count = 0;
        static IndexVtableHook index_vtable_hooks[MAX_INDEX_VTABLE_HOOKS] = {};
        static std::size_t index_vtable_hook_count = 0;

        static Hook group_draw_hook;
        static const void *group_draw_original = nullptr;

        static std::int64_t current_frame_index() noexcept {
            return rasterizer_globals ? rasterizer_globals->frame_index : -1;
        }

        static bool ranges_overlap(std::uint64_t a_start, std::uint64_t a_end,
                                   std::uint64_t b_start, std::uint64_t b_end) noexcept {
            return a_start < b_end && b_start < a_end;
        }

        static UINT index_element_size(D3DFORMAT format) noexcept {
            if(format == D3DFMT_INDEX16) {
                return 2;
            }
            if(format == D3DFMT_INDEX32) {
                return 4;
            }
            return 0;
        }

        static bool is_suspect_chicago_group(TransparentGeometryGroup *group) noexcept {
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

        static TargetIndexBuffer *find_target_index_buffer(IDirect3DIndexBuffer9 *buffer) noexcept {
            for(std::size_t i = 0; i < target_index_buffer_count; i++) {
                if(target_index_buffers[i].buffer == buffer) {
                    return &target_index_buffers[i];
                }
            }
            return nullptr;
        }

        static HRESULT STDMETHODCALLTYPE draw_indexed_primitive_diagnostic(
            IDirect3DDevice9 *device,
            D3DPRIMITIVETYPE primitive_type,
            INT base_vertex_index,
            UINT min_vertex_index,
            UINT num_vertices,
            UINT start_index,
            UINT primitive_count
        ) noexcept;

        static HRESULT STDMETHODCALLTYPE index_buffer_lock_diagnostic(
            IDirect3DIndexBuffer9 *buffer,
            UINT offset_to_lock,
            UINT size_to_lock,
            void **data,
            DWORD flags
        ) noexcept;

        static HRESULT STDMETHODCALLTYPE index_buffer_unlock_diagnostic(IDirect3DIndexBuffer9 *buffer) noexcept;

        static bool ensure_device_draw_hook() noexcept {
            if(!global_d3d9_device || !*global_d3d9_device) {
                stats.device_hook_failures++;
                return false;
            }

            auto *device = *global_d3d9_device;
            if(find_device_vtable_hook(device)) {
                return true;
            }

            if(device_vtable_hook_count >= MAX_DEVICE_VTABLE_HOOKS) {
                stats.device_vtable_overflow++;
                return false;
            }

            auto **vtable = *reinterpret_cast<void ***>(device);
            if(!vtable) {
                stats.device_hook_failures++;
                return false;
            }

            auto original = reinterpret_cast<DrawIndexedPrimitiveFunction>(
                vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX]
            );
            if(!original) {
                stats.device_hook_failures++;
                return false;
            }

            void *replacement = reinterpret_cast<void *>(draw_indexed_primitive_diagnostic);
            overwrite(&vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX], replacement);
            if(vtable[DEVICE_DRAW_INDEXED_PRIMITIVE_VTABLE_INDEX] != replacement) {
                stats.device_hook_failures++;
                return false;
            }

            auto &hook = device_vtable_hooks[device_vtable_hook_count++];
            hook.vtable = vtable;
            hook.original = original;
            return true;
        }

        static bool ensure_index_buffer_hooks(IDirect3DIndexBuffer9 *buffer) noexcept {
            if(!buffer) {
                return false;
            }
            if(find_index_vtable_hook(buffer)) {
                return true;
            }
            if(index_vtable_hook_count >= MAX_INDEX_VTABLE_HOOKS) {
                stats.index_vtable_overflow++;
                return false;
            }

            auto **vtable = *reinterpret_cast<void ***>(buffer);
            if(!vtable) {
                stats.index_vtable_hook_failures++;
                return false;
            }

            auto original_lock = reinterpret_cast<IndexBufferLockFunction>(vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX]);
            auto original_unlock = reinterpret_cast<IndexBufferUnlockFunction>(vtable[INDEX_BUFFER_UNLOCK_VTABLE_INDEX]);
            if(!original_lock || !original_unlock) {
                stats.index_vtable_hook_failures++;
                return false;
            }

            void *lock_replacement = reinterpret_cast<void *>(index_buffer_lock_diagnostic);
            void *unlock_replacement = reinterpret_cast<void *>(index_buffer_unlock_diagnostic);

            overwrite(&vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX], lock_replacement);
            if(vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX] != lock_replacement) {
                stats.index_vtable_hook_failures++;
                return false;
            }

            overwrite(&vtable[INDEX_BUFFER_UNLOCK_VTABLE_INDEX], unlock_replacement);
            if(vtable[INDEX_BUFFER_UNLOCK_VTABLE_INDEX] != unlock_replacement) {
                void *restore_lock = reinterpret_cast<void *>(original_lock);
                overwrite(&vtable[INDEX_BUFFER_LOCK_VTABLE_INDEX], restore_lock);
                stats.index_vtable_hook_failures++;
                return false;
            }

            auto &hook = index_vtable_hooks[index_vtable_hook_count++];
            hook.vtable = vtable;
            hook.original_lock = original_lock;
            hook.original_unlock = original_unlock;
            return true;
        }

        static TargetIndexBuffer *register_target_index_buffer(IDirect3DIndexBuffer9 *buffer) noexcept {
            if(auto *existing = find_target_index_buffer(buffer)) {
                return existing;
            }
            if(target_index_buffer_count >= MAX_TARGET_INDEX_BUFFERS) {
                stats.target_overflow++;
                return nullptr;
            }

            auto &target = target_index_buffers[target_index_buffer_count++];
            target = {};
            target.buffer = buffer;

            if(FAILED(buffer->GetDesc(&target.desc))) {
                stats.target_desc_failures++;
            }
            else {
                target.desc_valid = true;
            }

            if(!ensure_index_buffer_hooks(buffer)) {
                stats.index_vtable_hook_failures++;
            }

            return &target;
        }

        static void update_dip_ranges(INT base_vertex_index, UINT start_index,
                                      UINT primitive_count, UINT num_vertices) noexcept {
            if(!stats.dip_range_initialized) {
                stats.dip_range_initialized = true;
                stats.min_base_vertex = base_vertex_index;
                stats.max_base_vertex = base_vertex_index;
                stats.min_start_index = start_index;
                stats.max_start_index = start_index;
                stats.min_primitive_count = primitive_count;
                stats.max_primitive_count = primitive_count;
                stats.min_num_vertices = num_vertices;
                stats.max_num_vertices = num_vertices;
                return;
            }

            if(base_vertex_index < stats.min_base_vertex) stats.min_base_vertex = base_vertex_index;
            if(base_vertex_index > stats.max_base_vertex) stats.max_base_vertex = base_vertex_index;
            if(start_index < stats.min_start_index) stats.min_start_index = start_index;
            if(start_index > stats.max_start_index) stats.max_start_index = start_index;
            if(primitive_count < stats.min_primitive_count) stats.min_primitive_count = primitive_count;
            if(primitive_count > stats.max_primitive_count) stats.max_primitive_count = primitive_count;
            if(num_vertices < stats.min_num_vertices) stats.min_num_vertices = num_vertices;
            if(num_vertices > stats.max_num_vertices) stats.max_num_vertices = num_vertices;
        }

        static void record_draw_range(TargetIndexBuffer &target, D3DPRIMITIVETYPE primitive_type,
                                      UINT start_index, UINT primitive_count) noexcept {
            target.last_draw_range_valid = false;
            target.last_draw_frame = current_frame_index();

            if(!target.desc_valid || primitive_type != D3DPT_TRIANGLELIST) {
                stats.draw_range_unknown++;
                return;
            }

            const UINT element_size = index_element_size(target.desc.Format);
            if(element_size == 0) {
                stats.draw_range_unknown++;
                return;
            }

            const std::uint64_t index_start = start_index;
            const std::uint64_t index_end = index_start + static_cast<std::uint64_t>(primitive_count) * 3ULL;
            const std::uint64_t byte_start = index_start * element_size;
            const std::uint64_t byte_end = index_end * element_size;

            target.last_draw_byte_start = byte_start;
            target.last_draw_byte_end = byte_end;
            target.last_draw_range_valid = true;

            if(byte_end > target.desc.Size) {
                stats.draw_range_out_of_bounds++;
            }
        }

        static HRESULT STDMETHODCALLTYPE draw_indexed_primitive_diagnostic(
            IDirect3DDevice9 *device,
            D3DPRIMITIVETYPE primitive_type,
            INT base_vertex_index,
            UINT min_vertex_index,
            UINT num_vertices,
            UINT start_index,
            UINT primitive_count
        ) noexcept {
            auto *vtable_hook = find_device_vtable_hook(device);
            if(!vtable_hook || !vtable_hook->original) {
                return D3DERR_INVALIDCALL;
            }

            if(suspect_context.active) {
                stats.suspect_dip_calls++;
                update_dip_ranges(base_vertex_index, start_index, primitive_count, num_vertices);

                if(primitive_type == D3DPT_TRIANGLELIST) {
                    stats.triangle_list_dip_calls++;
                }
                else {
                    stats.other_primitive_dip_calls++;
                }

                if(suspect_context.first_triangle_index >= 0) {
                    const std::uint64_t first = static_cast<std::uint64_t>(suspect_context.first_triangle_index);
                    if(start_index == first) {
                        stats.start_matches_first_triangle++;
                    }
                    else if(start_index == first * 3ULL) {
                        stats.start_matches_first_triangle_x3++;
                    }
                    else {
                        stats.start_matches_neither++;
                    }
                }
                else {
                    stats.start_matches_neither++;
                }

                if(suspect_context.triangle_count >= 0
                    && primitive_count == static_cast<UINT>(suspect_context.triangle_count)) {
                    stats.primitive_count_matches++;
                }
                else {
                    stats.primitive_count_mismatches++;
                }

                IDirect3DIndexBuffer9 *bound_buffer = nullptr;
                const HRESULT get_indices_result = device->GetIndices(&bound_buffer);
                if(FAILED(get_indices_result)) {
                    stats.get_indices_failures++;
                }
                else if(!bound_buffer) {
                    stats.no_bound_index_buffer++;
                }
                else {
                    if(last_suspect_index_buffer && last_suspect_index_buffer != bound_buffer) {
                        stats.bound_index_buffer_changes++;
                    }
                    last_suspect_index_buffer = bound_buffer;

                    if(auto *target = register_target_index_buffer(bound_buffer)) {
                        target->draws++;
                        if(target->outstanding_locks > 0) {
                            stats.draw_with_outstanding_lock++;
                        }
                        record_draw_range(*target, primitive_type, start_index, primitive_count);
                    }

                    bound_buffer->Release();
                }
            }

            return vtable_hook->original(
                device,
                primitive_type,
                base_vertex_index,
                min_vertex_index,
                num_vertices,
                start_index,
                primitive_count
            );
        }

        static void update_lock_range_stats(UINT offset, UINT size, std::uint64_t end) noexcept {
            if(!stats.lock_range_initialized) {
                stats.lock_range_initialized = true;
                stats.min_lock_offset = offset;
                stats.max_lock_offset = offset;
                stats.min_lock_size = size;
                stats.max_lock_size = size;
                stats.max_lock_end = end;
                return;
            }

            if(offset < stats.min_lock_offset) stats.min_lock_offset = offset;
            if(offset > stats.max_lock_offset) stats.max_lock_offset = offset;
            if(size < stats.min_lock_size) stats.min_lock_size = size;
            if(size > stats.max_lock_size) stats.max_lock_size = size;
            if(end > stats.max_lock_end) stats.max_lock_end = end;
        }

        static HRESULT STDMETHODCALLTYPE index_buffer_lock_diagnostic(
            IDirect3DIndexBuffer9 *buffer,
            UINT offset_to_lock,
            UINT size_to_lock,
            void **data,
            DWORD flags
        ) noexcept {
            auto *vtable_hook = find_index_vtable_hook(buffer);
            if(!vtable_hook || !vtable_hook->original_lock) {
                return D3DERR_INVALIDCALL;
            }

            auto *target = find_target_index_buffer(buffer);
            if(!target) {
                return vtable_hook->original_lock(buffer, offset_to_lock, size_to_lock, data, flags);
            }

            stats.target_lock_calls++;
            target->locks++;

            const bool discard = (flags & D3DLOCK_DISCARD) != 0;
            const bool nooverwrite = (flags & D3DLOCK_NOOVERWRITE) != 0;
            if(discard) stats.discard_locks++;
            if(nooverwrite) stats.nooverwrite_locks++;
            if(discard && nooverwrite) stats.discard_and_nooverwrite_locks++;
            if(!discard && !nooverwrite) stats.neither_streaming_flag_locks++;
            if(flags & D3DLOCK_READONLY) stats.readonly_locks++;

            bool lock_range_valid = false;
            std::uint64_t lock_start = offset_to_lock;
            std::uint64_t lock_end = lock_start;

            if(offset_to_lock == 0 && size_to_lock == 0) {
                stats.whole_buffer_locks++;
                if(target->desc_valid) {
                    lock_start = 0;
                    lock_end = target->desc.Size;
                    lock_range_valid = true;
                }
            }
            else if(size_to_lock == 0) {
                stats.zero_size_nonzero_offset_locks++;
                stats.lock_range_unknown++;
            }
            else {
                lock_end = lock_start + size_to_lock;
                lock_range_valid = true;
            }

            if(lock_range_valid) {
                update_lock_range_stats(offset_to_lock, size_to_lock, lock_end);
                if(target->desc_valid && lock_end > target->desc.Size) {
                    stats.lock_range_out_of_bounds++;
                }

                if(target->last_draw_range_valid
                    && target->last_draw_frame == current_frame_index()
                    && ranges_overlap(lock_start, lock_end,
                                      target->last_draw_byte_start, target->last_draw_byte_end)) {
                    stats.lock_overlaps_last_draw_same_frame++;
                    if(nooverwrite) {
                        stats.nooverwrite_overlaps_last_draw_same_frame++;
                    }
                    if(discard) {
                        stats.discard_overlaps_last_draw_same_frame++;
                    }
                }
            }

            const HRESULT result = vtable_hook->original_lock(buffer, offset_to_lock, size_to_lock, data, flags);
            if(FAILED(result)) {
                stats.lock_failures++;
                return result;
            }

            target->outstanding_locks++;
            if(target->outstanding_locks > target->max_outstanding_locks) {
                target->max_outstanding_locks = target->outstanding_locks;
            }
            target->last_lock_range_valid = lock_range_valid;
            target->last_lock_byte_start = lock_start;
            target->last_lock_byte_end = lock_end;
            target->last_lock_frame = current_frame_index();
            target->last_lock_flags = flags;
            return result;
        }

        static HRESULT STDMETHODCALLTYPE index_buffer_unlock_diagnostic(IDirect3DIndexBuffer9 *buffer) noexcept {
            auto *vtable_hook = find_index_vtable_hook(buffer);
            if(!vtable_hook || !vtable_hook->original_unlock) {
                return D3DERR_INVALIDCALL;
            }

            auto *target = find_target_index_buffer(buffer);
            if(!target) {
                return vtable_hook->original_unlock(buffer);
            }

            stats.target_unlock_calls++;
            target->unlocks++;
            if(target->outstanding_locks <= 0) {
                stats.unlock_without_lock++;
            }

            const HRESULT result = vtable_hook->original_unlock(buffer);
            if(FAILED(result)) {
                stats.unlock_failures++;
                return result;
            }

            if(target->outstanding_locks > 0) {
                target->outstanding_locks--;
            }
            return result;
        }

        static void reset_stats() noexcept {
            stats = {};
            suspect_context = {};
            last_suspect_index_buffer = nullptr;
            target_index_buffer_count = 0;
            for(auto &target : target_index_buffers) {
                target = {};
            }
        }

        static void print_stats() noexcept {
            console_output("chimera_debug_chicago_index: device_hooks=%u index_hooks=%u targets=%u",
                static_cast<unsigned int>(device_vtable_hook_count),
                static_cast<unsigned int>(index_vtable_hook_count),
                static_cast<unsigned int>(target_index_buffer_count));

            console_output("groups: total=%llu suspect=%llu no_dip=%llu multi_dip=%llu",
                static_cast<unsigned long long>(stats.group_draw_calls),
                static_cast<unsigned long long>(stats.suspect_group_calls),
                static_cast<unsigned long long>(stats.suspect_groups_without_dip),
                static_cast<unsigned long long>(stats.suspect_groups_multiple_dips));

            console_output("hooks: device_fail=%llu device_overflow=%llu index_fail=%llu index_overflow=%llu target_overflow=%llu",
                static_cast<unsigned long long>(stats.device_hook_failures),
                static_cast<unsigned long long>(stats.device_vtable_overflow),
                static_cast<unsigned long long>(stats.index_vtable_hook_failures),
                static_cast<unsigned long long>(stats.index_vtable_overflow),
                static_cast<unsigned long long>(stats.target_overflow));

            console_output("dip: suspect=%llu triangle_list=%llu other=%llu get_indices_fail=%llu no_ib=%llu ib_changes=%llu outstanding_lock=%llu",
                static_cast<unsigned long long>(stats.suspect_dip_calls),
                static_cast<unsigned long long>(stats.triangle_list_dip_calls),
                static_cast<unsigned long long>(stats.other_primitive_dip_calls),
                static_cast<unsigned long long>(stats.get_indices_failures),
                static_cast<unsigned long long>(stats.no_bound_index_buffer),
                static_cast<unsigned long long>(stats.bound_index_buffer_changes),
                static_cast<unsigned long long>(stats.draw_with_outstanding_lock));

            console_output("mapping: start=first:%llu first_x3:%llu other:%llu primitive_match=%llu mismatch=%llu oob=%llu unknown=%llu",
                static_cast<unsigned long long>(stats.start_matches_first_triangle),
                static_cast<unsigned long long>(stats.start_matches_first_triangle_x3),
                static_cast<unsigned long long>(stats.start_matches_neither),
                static_cast<unsigned long long>(stats.primitive_count_matches),
                static_cast<unsigned long long>(stats.primitive_count_mismatches),
                static_cast<unsigned long long>(stats.draw_range_out_of_bounds),
                static_cast<unsigned long long>(stats.draw_range_unknown));

            if(stats.dip_range_initialized) {
                console_output("dip_ranges: base=%d..%d start=%u..%u primitives=%u..%u vertices=%u..%u",
                    stats.min_base_vertex,
                    stats.max_base_vertex,
                    stats.min_start_index,
                    stats.max_start_index,
                    stats.min_primitive_count,
                    stats.max_primitive_count,
                    stats.min_num_vertices,
                    stats.max_num_vertices);
            }

            console_output("locks: lock=%llu unlock=%llu lock_fail=%llu unlock_fail=%llu discard=%llu nooverwrite=%llu both=%llu neither=%llu readonly=%llu",
                static_cast<unsigned long long>(stats.target_lock_calls),
                static_cast<unsigned long long>(stats.target_unlock_calls),
                static_cast<unsigned long long>(stats.lock_failures),
                static_cast<unsigned long long>(stats.unlock_failures),
                static_cast<unsigned long long>(stats.discard_locks),
                static_cast<unsigned long long>(stats.nooverwrite_locks),
                static_cast<unsigned long long>(stats.discard_and_nooverwrite_locks),
                static_cast<unsigned long long>(stats.neither_streaming_flag_locks),
                static_cast<unsigned long long>(stats.readonly_locks));

            console_output("lock_ranges: whole=%llu zero_size_offset=%llu oob=%llu unknown=%llu overlap_last_draw=%llu nooverwrite_overlap=%llu discard_overlap=%llu unlock_without_lock=%llu",
                static_cast<unsigned long long>(stats.whole_buffer_locks),
                static_cast<unsigned long long>(stats.zero_size_nonzero_offset_locks),
                static_cast<unsigned long long>(stats.lock_range_out_of_bounds),
                static_cast<unsigned long long>(stats.lock_range_unknown),
                static_cast<unsigned long long>(stats.lock_overlaps_last_draw_same_frame),
                static_cast<unsigned long long>(stats.nooverwrite_overlaps_last_draw_same_frame),
                static_cast<unsigned long long>(stats.discard_overlaps_last_draw_same_frame),
                static_cast<unsigned long long>(stats.unlock_without_lock));

            if(stats.lock_range_initialized) {
                console_output("lock_span: offset=%u..%u size=%u..%u max_end=%llu",
                    stats.min_lock_offset,
                    stats.max_lock_offset,
                    stats.min_lock_size,
                    stats.max_lock_size,
                    static_cast<unsigned long long>(stats.max_lock_end));
            }

            if(stats.target_desc_failures) {
                console_output("target_desc_failures=%llu",
                    static_cast<unsigned long long>(stats.target_desc_failures));
            }

            for(std::size_t i = 0; i < target_index_buffer_count; i++) {
                const auto &target = target_index_buffers[i];
                if(target.desc_valid) {
                    console_output("target.%u ib=%p draws=%llu locks=%llu unlocks=%llu outstanding=%ld max_outstanding=%ld size=%u format=%lu usage=0x%08lX pool=%lu last_flags=0x%08lX",
                        static_cast<unsigned int>(i),
                        reinterpret_cast<void *>(target.buffer),
                        static_cast<unsigned long long>(target.draws),
                        static_cast<unsigned long long>(target.locks),
                        static_cast<unsigned long long>(target.unlocks),
                        target.outstanding_locks,
                        target.max_outstanding_locks,
                        target.desc.Size,
                        static_cast<unsigned long>(target.desc.Format),
                        static_cast<unsigned long>(target.desc.Usage),
                        static_cast<unsigned long>(target.desc.Pool),
                        static_cast<unsigned long>(target.last_lock_flags));
                }
                else {
                    console_output("target.%u ib=%p draws=%llu locks=%llu unlocks=%llu outstanding=%ld max_outstanding=%ld no_desc last_flags=0x%08lX",
                        static_cast<unsigned int>(i),
                        reinterpret_cast<void *>(target.buffer),
                        static_cast<unsigned long long>(target.draws),
                        static_cast<unsigned long long>(target.locks),
                        static_cast<unsigned long long>(target.unlocks),
                        target.outstanding_locks,
                        target.max_outstanding_locks,
                        static_cast<unsigned long>(target.last_lock_flags));
                }
            }
        }

        static bool chicago_index_diagnostic_command(const char *command) noexcept {
            if(!command) {
                return true;
            }

            static constexpr char command_name[] = "chimera_debug_chicago_index";
            static constexpr std::size_t command_name_length = sizeof(command_name) - 1;
            if(std::strncmp(command, command_name, command_name_length) != 0) {
                return true;
            }

            const char *argument = command + command_name_length;
            if(*argument != '\0' && !std::isspace(static_cast<unsigned char>(*argument))) {
                return true;
            }
            while(std::isspace(static_cast<unsigned char>(*argument))) {
                argument++;
            }

            if(*argument == '\0' || std::strcmp(argument, "stats") == 0) {
                print_stats();
                console_output("usage: chimera_debug_chicago_index [stats|reset]");
                return false;
            }

            if(std::strcmp(argument, "reset") == 0) {
                reset_stats();
                console_output("chimera_debug_chicago_index: counters reset");
                return false;
            }

            console_error("chimera_debug_chicago_index: expected stats or reset");
            return false;
        }

        static void group_draw_diagnostic(TransparentGeometryGroup *group, bool is_dirty) noexcept {
            using GroupDrawFunction = void (*)(TransparentGeometryGroup *, bool);
            auto original = reinterpret_cast<GroupDrawFunction>(const_cast<void *>(group_draw_original));
            if(!original) {
                return;
            }

            stats.group_draw_calls++;
            if(!is_suspect_chicago_group(group)) {
                original(group, is_dirty);
                return;
            }

            stats.suspect_group_calls++;
            ensure_device_draw_hook();

            const auto previous_context = suspect_context;
            suspect_context.active = true;
            suspect_context.dynamic_triangle_buffer_index = group->dynamic_triangle_buffer_index;
            suspect_context.first_triangle_index = group->first_triangle_index;
            suspect_context.triangle_count = group->triangle_count;

            const auto dip_before = stats.suspect_dip_calls;
            original(group, is_dirty);
            const auto dip_delta = stats.suspect_dip_calls - dip_before;

            if(dip_delta == 0) {
                stats.suspect_groups_without_dip++;
            }
            else if(dip_delta > 1) {
                stats.suspect_groups_multiple_dips++;
            }

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

    void set_up_environment_transparent_diagnostic() noexcept {
        static bool installed = false;
        if(installed || !rasterizer_transparent_geometry_group_draw_func) {
            return;
        }

        write_function_override(
            rasterizer_transparent_geometry_group_draw_func,
            group_draw_hook,
            reinterpret_cast<const void *>(group_draw_diagnostic),
            &group_draw_original
        );

        if(group_draw_original) {
            add_command_event(chicago_index_diagnostic_command, EVENT_PRIORITY_BEFORE);
            installed = true;
        }
    }

}
