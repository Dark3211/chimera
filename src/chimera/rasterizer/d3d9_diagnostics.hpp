// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_DIAGNOSTICS_HPP
#define CHIMERA_D3D9_DIAGNOSTICS_HPP

#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace D3D9Diagnostics {
        constexpr std::size_t DEVICE_DRAW_INDEXED_PRIMITIVE = 82;
        constexpr std::size_t DEVICE_SET_VERTEX_DECLARATION = 87;
        constexpr std::size_t DEVICE_SET_VERTEX_SHADER = 92;
        constexpr std::size_t DEVICE_SET_VERTEX_SHADER_CONSTANT_F = 94;
        constexpr std::size_t DEVICE_SET_STREAM_SOURCE = 100;
        constexpr std::size_t DEVICE_SET_INDICES = 104;
        constexpr UINT NODE_MATRIX_REGISTER_FIRST = 29;
        constexpr UINT NODE_MATRIX_REGISTER_COUNT = 66;
        constexpr std::size_t MAX_LOGGED_NODE_WRITES = 96;

        using DrawIndexedPrimitiveFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT
        );
        using SetVertexDeclarationFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *
        );
        using SetVertexShaderFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, IDirect3DVertexShader9 *
        );
        using SetVertexShaderConstantFFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, UINT, const float *, UINT
        );
        using SetStreamSourceFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, UINT, IDirect3DVertexBuffer9 *, UINT, UINT
        );
        using SetIndicesFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, IDirect3DIndexBuffer9 *
        );

        static DrawIndexedPrimitiveFunction original_draw_indexed_primitive = nullptr;
        static SetVertexDeclarationFunction original_set_vertex_declaration = nullptr;
        static SetVertexShaderFunction original_set_vertex_shader = nullptr;
        static SetVertexShaderConstantFFunction original_set_vertex_shader_constant_f = nullptr;
        static SetStreamSourceFunction original_set_stream_source = nullptr;
        static SetIndicesFunction original_set_indices = nullptr;

        static IDirect3DVertexDeclaration9 *current_declaration = nullptr;
        static IDirect3DVertexShader9 *current_vertex_shader = nullptr;
        static IDirect3DVertexBuffer9 *current_stream0 = nullptr;
        static IDirect3DIndexBuffer9 *current_indices = nullptr;
        static UINT current_stream0_offset = 0;
        static UINT current_stream0_stride = 0;
        static std::FILE *log_file = nullptr;
        static std::size_t logged_node_writes = 0;
        static bool seen_model_draw[NUM_OF_VERTEX_SHADERS][NUM_OF_VERTEX_DECLARATIONS] = {};
        static bool seen_unknown_model_draw = false;
        static bool installed = false;

        static const char *vertex_declaration_name(int index) noexcept {
            switch(index) {
                case VERTEX_DECLARATION_ENVIRONMENT_UNCOMPRESSED: return "environment_uncompressed";
                case VERTEX_DECLARATION_ENVIRONMENT_COMPRESSED: return "environment_compressed";
                case VERTEX_DECLARATION_ENVIRONMENT_LIGHTMAP_UNCOMPRESSED: return "environment_lightmap_uncompressed";
                case VERTEX_DECLARATION_ENVIRONMENT_LIGHTMAP_COMPRESSED: return "environment_lightmap_compressed";
                case VERTEX_DECLARATION_MODEL_UNCOMPRESSED: return "model_uncompressed";
                case VERTEX_DECLARATION_MODEL_COMPRESSED: return "model_compressed";
                case VERTEX_DECLARATION_UNLIT: return "unlit";
                case VERTEX_DECLARATION_DYNAMIC_UNLIT: return "dynamic_unlit";
                case VERTEX_DECLARATION_DYNAMIC_SCREEN: return "dynamic_screen";
                case VERTEX_DECLARATION_DEBUG: return "debug";
                case VERTEX_DECLARATION_DECAL: return "decal";
                case VERTEX_DECLARATION_DETAIL_OBJECT: return "detail_object";
                case VERTEX_DECLARATION_ENVIRONMENT_UNCOMPRESSED_FF: return "environment_uncompressed_ff";
                case VERTEX_DECLARATION_ENVIRONMENT_LIGHTMAP_UNCOMPRESSED_FF: return "environment_lightmap_uncompressed_ff";
                case VERTEX_DECLARATION_MODEL_UNCOMPRESSED_FF: return "model_uncompressed_ff";
                case VERTEX_DECLARATION_MODEL_PROCESSED: return "model_processed";
                case VERTEX_DECLARATION_UNLIT_ZSPRITE: return "unlit_zsprite";
                case VERTEX_DECLARATION_SCREEN_TRANSFORMED_LIT: return "screen_transformed_lit";
                case VERTEX_DECLARATION_SCREEN_TRANSFORMED_LIT_SPECULAR: return "screen_transformed_lit_specular";
                case VERTEX_DECLARATION_ENVIRONMENT_SINGLE_STREAM_FF: return "environment_single_stream_ff";
                default: return "unknown";
            }
        }

        static const char *vertex_shader_name(int index) noexcept {
            switch(index) {
                case VSH_MODEL_FOGGED: return "model_fogged";
                case VSH_MODEL: return "model";
                case VSH_MODEL_FF: return "model_ff";
                case VSH_MODEL_FAST: return "model_fast";
                case VSH_MODEL_SCENERY: return "model_scenery";
                case VSH_MODEL_ACTIVE_CAMOUFLAGE: return "model_active_camouflage";
                case VSH_MODEL_ACTIVE_CAMOUFLAGE_FF: return "model_active_camouflage_ff";
                case VSH_MODEL_FOG_SCREEN: return "model_fog_screen";
                case VSH_MODEL_SHADOW: return "model_shadow";
                case VSH_MODEL_ZBUFFER: return "model_zbuffer";
                default: return "non_model";
            }
        }

        template<typename... Args>
        static void log_line(const char *format, Args... args) noexcept {
            if(!log_file || !format) {
                return;
            }
            std::fprintf(log_file, format, args...);
            std::fputc('\n', log_file);
        }

        static int find_vertex_declaration(IDirect3DVertexDeclaration9 *declaration) noexcept {
            if(!declaration || !vertex_declarations) {
                return -1;
            }
            for(int i = 0; i < NUM_OF_VERTEX_DECLARATIONS; i++) {
                if(vertex_declarations[i].declaration == declaration) {
                    return i;
                }
            }
            return -1;
        }

        static int find_vertex_shader(IDirect3DVertexShader9 *shader) noexcept {
            if(!shader || !vertex_shaders) {
                return -1;
            }
            for(int i = 0; i < NUM_OF_VERTEX_SHADERS; i++) {
                if(vertex_shaders[i].shader == shader) {
                    return i;
                }
            }
            return -1;
        }

        static bool is_model_shader(int index) noexcept {
            return index >= VSH_MODEL_FOGGED && index <= VSH_MODEL_ZBUFFER;
        }

        static bool suspicious_float(float value) noexcept {
            std::uint32_t bits = 0;
            static_assert(sizeof(bits) == sizeof(value));
            std::memcpy(&bits, &value, sizeof(bits));
            if((bits & 0x7F800000U) == 0x7F800000U) {
                return true;
            }
            return value > 100000000.0F || value < -100000000.0F;
        }

        static std::uint32_t fnv1a(const std::byte *data, std::size_t size) noexcept {
            std::uint32_t hash = 2166136261U;
            for(std::size_t i = 0; i < size; i++) {
                hash ^= static_cast<std::uint8_t>(data[i]);
                hash *= 16777619U;
            }
            return hash;
        }

        static void log_declaration(int index, IDirect3DVertexDeclaration9 *declaration) noexcept {
            if(!declaration) {
                log_line("DECL %d %s null", index, vertex_declaration_name(index));
                return;
            }

            UINT count = 0;
            if(FAILED(declaration->GetDeclaration(nullptr, &count)) || count == 0 || count > 64) {
                log_line("DECL %d %s GetDeclaration failed/count=%u", index, vertex_declaration_name(index), count);
                return;
            }

            auto *elements = new(std::nothrow) D3DVERTEXELEMENT9[count];
            if(!elements) {
                log_line("DECL %d %s allocation failed", index, vertex_declaration_name(index));
                return;
            }

            UINT supplied = count;
            const HRESULT result = declaration->GetDeclaration(elements, &supplied);
            if(FAILED(result)) {
                log_line("DECL %d %s GetDeclaration hr=0x%08lX", index, vertex_declaration_name(index), static_cast<unsigned long>(result));
                delete[] elements;
                return;
            }

            log_line("DECL %d %s elements=%u fvf=0x%08lX vp_method=0x%08lX",
                     index,
                     vertex_declaration_name(index),
                     supplied,
                     vertex_declarations ? static_cast<unsigned long>(vertex_declarations[index].fvf) : 0UL,
                     vertex_declarations ? static_cast<unsigned long>(vertex_declarations[index].vertex_processing_method) : 0UL);
            for(UINT i = 0; i < supplied; i++) {
                const auto &e = elements[i];
                log_line("  E%u stream=%u offset=%u type=%u method=%u usage=%u usage_index=%u",
                         i,
                         static_cast<unsigned>(e.Stream),
                         static_cast<unsigned>(e.Offset),
                         static_cast<unsigned>(e.Type),
                         static_cast<unsigned>(e.Method),
                         static_cast<unsigned>(e.Usage),
                         static_cast<unsigned>(e.UsageIndex));
            }
            delete[] elements;
        }

        static void log_shader(int index, IDirect3DVertexShader9 *shader) noexcept {
            if(!shader) {
                log_line("VSH %d %s null", index, vertex_shader_name(index));
                return;
            }

            UINT size = 0;
            if(FAILED(shader->GetFunction(nullptr, &size)) || size == 0) {
                log_line("VSH %d %s GetFunction failed/size=%u", index, vertex_shader_name(index), size);
                return;
            }

            auto *bytes = new(std::nothrow) std::byte[size];
            if(!bytes) {
                log_line("VSH %d %s size=%u allocation_failed", index, vertex_shader_name(index), size);
                return;
            }
            UINT supplied = size;
            const HRESULT result = shader->GetFunction(bytes, &supplied);
            if(SUCCEEDED(result)) {
                std::uint32_t version = 0;
                if(supplied >= sizeof(version)) {
                    std::memcpy(&version, bytes, sizeof(version));
                }
                log_line("VSH %d %s size=%u version=0x%08lX hash=%08lX",
                         index,
                         vertex_shader_name(index),
                         supplied,
                         static_cast<unsigned long>(version),
                         static_cast<unsigned long>(fnv1a(bytes, supplied)));
            }
            else {
                log_line("VSH %d %s GetFunction hr=0x%08lX", index, vertex_shader_name(index), static_cast<unsigned long>(result));
            }
            delete[] bytes;
        }

        static void write_snapshot(IDirect3DDevice9 *device, const char *backend, const char *mode) noexcept {
            if(!log_file || !device) {
                return;
            }

            log_line("CHIMERA D3D9 DIAGNOSTIC SNAPSHOT");
            log_line("backend=%s mode=%s", backend ? backend : "(null)", mode ? mode : "(null)");

            D3DCAPS9 caps = {};
            if(SUCCEEDED(device->GetDeviceCaps(&caps))) {
                log_line("CAPS AdapterOrdinal=%u DeviceType=%u DevCaps=0x%08lX VertexProcessingCaps=0x%08lX",
                         caps.AdapterOrdinal,
                         static_cast<unsigned>(caps.DeviceType),
                         static_cast<unsigned long>(caps.DevCaps),
                         static_cast<unsigned long>(caps.VertexProcessingCaps));
                log_line("CAPS VS=0x%08lX PS=0x%08lX MaxVertexShaderConst=%lu MaxStreams=%lu MaxStreamStride=%lu",
                         static_cast<unsigned long>(caps.VertexShaderVersion),
                         static_cast<unsigned long>(caps.PixelShaderVersion),
                         static_cast<unsigned long>(caps.MaxVertexShaderConst),
                         static_cast<unsigned long>(caps.MaxStreams),
                         static_cast<unsigned long>(caps.MaxStreamStride));
            }
            else {
                log_line("CAPS GetDeviceCaps failed");
            }

            if(rasterizer_globals) {
                log_line("HALO maximum_nodes_per_model=%d using_software_vertex_processing=%u model_quality=%d",
                         static_cast<int>(rasterizer_globals->maximum_nodes_per_model),
                         rasterizer_globals->using_software_vertex_processing ? 1U : 0U,
                         rasterizer_debug_options ? static_cast<int>(rasterizer_debug_options->rasterizer_model_quality_level) : -1);
            }

            log_line("-- STOCK VERTEX DECLARATIONS --");
            if(vertex_declarations) {
                for(int i = 0; i < NUM_OF_VERTEX_DECLARATIONS; i++) {
                    log_declaration(i, vertex_declarations[i].declaration);
                }
            }
            else {
                log_line("vertex_declarations=null");
            }

            log_line("-- STOCK VERTEX SHADERS --");
            if(vertex_shaders) {
                for(int i = 0; i < NUM_OF_VERTEX_SHADERS; i++) {
                    log_shader(i, vertex_shaders[i].shader);
                }
            }
            else {
                log_line("vertex_shaders=null");
            }
            std::fflush(log_file);
        }

        template<typename Function>
        static bool patch_device_function(IDirect3DDevice9 *device,
                                          std::size_t index,
                                          Function replacement,
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

        static HRESULT STDMETHODCALLTYPE set_vertex_declaration_hook(
            IDirect3DDevice9 *device,
            IDirect3DVertexDeclaration9 *declaration
        ) noexcept {
            if(!original_set_vertex_declaration) {
                return D3DERR_INVALIDCALL;
            }
            const HRESULT result = original_set_vertex_declaration(device, declaration);
            if(SUCCEEDED(result)) {
                current_declaration = declaration;
            }
            return result;
        }

        static HRESULT STDMETHODCALLTYPE set_vertex_shader_hook(
            IDirect3DDevice9 *device,
            IDirect3DVertexShader9 *shader
        ) noexcept {
            if(!original_set_vertex_shader) {
                return D3DERR_INVALIDCALL;
            }
            const HRESULT result = original_set_vertex_shader(device, shader);
            if(SUCCEEDED(result)) {
                current_vertex_shader = shader;
            }
            return result;
        }

        static HRESULT STDMETHODCALLTYPE set_vertex_shader_constant_f_hook(
            IDirect3DDevice9 *device,
            UINT start_register,
            const float *constant_data,
            UINT vector4f_count
        ) noexcept {
            if(!original_set_vertex_shader_constant_f) {
                return D3DERR_INVALIDCALL;
            }

            const UINT end_register = start_register + vector4f_count;
            const UINT node_end = NODE_MATRIX_REGISTER_FIRST + NODE_MATRIX_REGISTER_COUNT;
            if(log_file && constant_data && vector4f_count > 0
                && start_register < node_end && end_register > NODE_MATRIX_REGISTER_FIRST) {
                bool suspicious = false;
                const std::size_t float_count = static_cast<std::size_t>(vector4f_count) * 4U;
                for(std::size_t i = 0; i < float_count; i++) {
                    if(suspicious_float(constant_data[i])) {
                        suspicious = true;
                        break;
                    }
                }

                if(suspicious || logged_node_writes < MAX_LOGGED_NODE_WRITES) {
                    log_line("NODE_CONST start=%u count=%u suspicious=%u shader=%d decl=%d",
                             start_register,
                             vector4f_count,
                             suspicious ? 1U : 0U,
                             find_vertex_shader(current_vertex_shader),
                             find_vertex_declaration(current_declaration));
                    if(suspicious) {
                        const std::size_t limit = float_count < 16U ? float_count : 16U;
                        for(std::size_t i = 0; i < limit; i++) {
                            log_line("  C[%u].%u=%g",
                                     start_register + static_cast<UINT>(i / 4U),
                                     static_cast<unsigned>(i % 4U),
                                     static_cast<double>(constant_data[i]));
                        }
                    }
                    if(logged_node_writes < MAX_LOGGED_NODE_WRITES) {
                        logged_node_writes++;
                    }
                }
            }

            return original_set_vertex_shader_constant_f(device, start_register, constant_data, vector4f_count);
        }

        static HRESULT STDMETHODCALLTYPE set_stream_source_hook(
            IDirect3DDevice9 *device,
            UINT stream_number,
            IDirect3DVertexBuffer9 *stream_data,
            UINT offset_in_bytes,
            UINT stride
        ) noexcept {
            if(!original_set_stream_source) {
                return D3DERR_INVALIDCALL;
            }
            const HRESULT result = original_set_stream_source(device, stream_number, stream_data, offset_in_bytes, stride);
            if(SUCCEEDED(result) && stream_number == 0) {
                current_stream0 = stream_data;
                current_stream0_offset = offset_in_bytes;
                current_stream0_stride = stride;
            }
            return result;
        }

        static HRESULT STDMETHODCALLTYPE set_indices_hook(
            IDirect3DDevice9 *device,
            IDirect3DIndexBuffer9 *index_data
        ) noexcept {
            if(!original_set_indices) {
                return D3DERR_INVALIDCALL;
            }
            const HRESULT result = original_set_indices(device, index_data);
            if(SUCCEEDED(result)) {
                current_indices = index_data;
            }
            return result;
        }

        static void log_model_draw_once(INT base_vertex_index,
                                        UINT min_vertex_index,
                                        UINT num_vertices,
                                        UINT start_index,
                                        UINT primitive_count) noexcept {
            const int shader_index = find_vertex_shader(current_vertex_shader);
            if(!is_model_shader(shader_index)) {
                return;
            }
            const int declaration_index = find_vertex_declaration(current_declaration);

            if(declaration_index >= 0 && declaration_index < NUM_OF_VERTEX_DECLARATIONS) {
                if(seen_model_draw[shader_index][declaration_index]) {
                    return;
                }
                seen_model_draw[shader_index][declaration_index] = true;
            }
            else {
                if(seen_unknown_model_draw) {
                    return;
                }
                seen_unknown_model_draw = true;
            }

            log_line("MODEL_DRAW shader=%d(%s) decl=%d(%s) base=%d min=%u vertices=%u start=%u prims=%u stream0_offset=%u stride=%u",
                     shader_index,
                     vertex_shader_name(shader_index),
                     declaration_index,
                     vertex_declaration_name(declaration_index),
                     base_vertex_index,
                     min_vertex_index,
                     num_vertices,
                     start_index,
                     primitive_count,
                     current_stream0_offset,
                     current_stream0_stride);

            if(current_stream0) {
                D3DVERTEXBUFFER_DESC desc = {};
                if(SUCCEEDED(current_stream0->GetDesc(&desc))) {
                    log_line("  VB size=%u usage=0x%08lX fvf=0x%08lX pool=%u",
                             desc.Size,
                             static_cast<unsigned long>(desc.Usage),
                             static_cast<unsigned long>(desc.FVF),
                             static_cast<unsigned>(desc.Pool));
                }
            }
            if(current_indices) {
                D3DINDEXBUFFER_DESC desc = {};
                if(SUCCEEDED(current_indices->GetDesc(&desc))) {
                    log_line("  IB size=%u usage=0x%08lX format=%u pool=%u",
                             desc.Size,
                             static_cast<unsigned long>(desc.Usage),
                             static_cast<unsigned>(desc.Format),
                             static_cast<unsigned>(desc.Pool));
                }
            }
            std::fflush(log_file);
        }

        static HRESULT STDMETHODCALLTYPE draw_indexed_primitive_hook(
            IDirect3DDevice9 *device,
            D3DPRIMITIVETYPE primitive_type,
            INT base_vertex_index,
            UINT min_vertex_index,
            UINT num_vertices,
            UINT start_index,
            UINT primitive_count
        ) noexcept {
            if(!original_draw_indexed_primitive) {
                return D3DERR_INVALIDCALL;
            }
            log_model_draw_once(base_vertex_index, min_vertex_index, num_vertices, start_index, primitive_count);
            return original_draw_indexed_primitive(
                device,
                primitive_type,
                base_vertex_index,
                min_vertex_index,
                num_vertices,
                start_index,
                primitive_count
            );
        }

        static bool install_trace_hooks(IDirect3DDevice9 *device) noexcept {
            bool ok = true;
            ok &= patch_device_function(device, DEVICE_SET_VERTEX_DECLARATION, set_vertex_declaration_hook, original_set_vertex_declaration);
            ok &= patch_device_function(device, DEVICE_SET_VERTEX_SHADER, set_vertex_shader_hook, original_set_vertex_shader);
            ok &= patch_device_function(device, DEVICE_SET_VERTEX_SHADER_CONSTANT_F, set_vertex_shader_constant_f_hook, original_set_vertex_shader_constant_f);
            ok &= patch_device_function(device, DEVICE_SET_STREAM_SOURCE, set_stream_source_hook, original_set_stream_source);
            ok &= patch_device_function(device, DEVICE_SET_INDICES, set_indices_hook, original_set_indices);
            ok &= patch_device_function(device, DEVICE_DRAW_INDEXED_PRIMITIVE, draw_indexed_primitive_hook, original_draw_indexed_primitive);
            return ok;
        }

        static void set_up() noexcept {
            if(installed || !global_d3d9_device || !*global_d3d9_device) {
                return;
            }

            auto *mode = get_chimera().get_ini()->get_value("video_mode.d3d_diag");
            if(!mode || !*mode || _stricmp(mode, "off") == 0) {
                return;
            }

            const bool snapshot_mode = _stricmp(mode, "snapshot") == 0;
            const bool trace_mode = _stricmp(mode, "trace") == 0;
            if(!snapshot_mode && !trace_mode) {
                console_warning("D3D9 diagnostic: unknown video_mode.d3d_diag value '%s'; use off, snapshot, or trace.", mode);
                return;
            }

            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            log_file = std::fopen("chimera_d3d9_diag.log", "w");
            if(!log_file) {
                console_warning("D3D9 diagnostic: could not create chimera_d3d9_diag.log");
                return;
            }

            write_snapshot(*global_d3d9_device, backend, mode);
            installed = true;
            console_output("D3D9 diagnostic: snapshot written to chimera_d3d9_diag.log.");

            if(trace_mode) {
                const bool is_9on12 = backend
                    && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
                if(!is_9on12) {
                    log_line("TRACE skipped: runtime hooks are restricted to the experimental 9On12 backend to avoid interacting with native9 fixes.");
                    std::fflush(log_file);
                    console_warning("D3D9 diagnostic: trace hooks are only installed for d3d_backend=9on12; native9 snapshot is still valid.");
                    return;
                }

                if(install_trace_hooks(*global_d3d9_device)) {
                    log_line("TRACE hooks installed");
                    std::fflush(log_file);
                    console_output("D3D9 diagnostic: model pipeline trace hooks installed.");
                }
                else {
                    log_line("TRACE hook installation incomplete");
                    std::fflush(log_file);
                    console_warning("D3D9 diagnostic: one or more trace hooks could not be installed.");
                }
            }
        }
    }

    inline void set_up_d3d9_diagnostics() noexcept {
        D3D9Diagnostics::set_up();
    }
}

#endif
