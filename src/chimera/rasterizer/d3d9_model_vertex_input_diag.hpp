// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_MODEL_VERTEX_INPUT_DIAG_HPP
#define CHIMERA_D3D9_MODEL_VERTEX_INPUT_DIAG_HPP

#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "d3d9_model_shader_primary_v2.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../event/d3d9_end_scene.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace D3D9ModelVertexInputDiag {
        constexpr std::size_t DEVICE_DRAW_PRIMITIVE = 81;
        constexpr std::size_t DEVICE_DRAW_INDEXED_PRIMITIVE = 82;
        constexpr std::size_t MAX_SEEN_DECLARATIONS = 64;

        using DrawPrimitiveFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT
        );
        using DrawIndexedPrimitiveFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT
        );

        struct SeenDeclaration {
            IDirect3DVertexDeclaration9 *declaration = nullptr;
            std::uint32_t id = 0;
        };

        static DrawPrimitiveFunction original_draw_primitive = nullptr;
        static DrawIndexedPrimitiveFunction original_draw_indexed_primitive = nullptr;
        static IDirect3DDevice9 *installed_device = nullptr;
        static bool queued_announced = false;
        static bool installed_announced = false;
        static bool end_scene_retry_registered = false;
        static std::FILE *input_log = nullptr;
        static std::uint32_t log_flush_counter = 0;
        static unsigned long long draw_counter = 0;
        static SeenDeclaration seen_declarations[MAX_SEEN_DECLARATIONS] = {};
        static std::size_t seen_declaration_count = 0;

        static bool d3d9on12_requested() noexcept {
            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            return backend
                && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
        }

        static bool trace_enabled() noexcept {
            if(!d3d9on12_requested()) {
                return false;
            }

            auto *value = get_chimera().get_ini()->get_value("video_mode.d3d_model_vertex_input_test");
            return value && (
                _stricmp(value, "log") == 0
                || _stricmp(value, "trace") == 0
            );
        }

        static void ensure_input_log() noexcept {
            if(input_log || !trace_enabled()) {
                return;
            }

            input_log = std::fopen("chimera_d3d9_model_vertex_input.log", "w");
            if(input_log) {
                std::fprintf(
                    input_log,
                    "# D3D9_DRAW_INPUT diagnostic: observational only; captures model/effect/transparent DrawPrimitive and DrawIndexedPrimitive calls.\n"
                );
                std::fprintf(
                    input_log,
                    "# kind frame draw pass PS prim VB0 OFFSET0 STRIDE0 VB0_SIZE VB0_CAP VB1 OFFSET1 STRIDE1 VB1_SIZE IB IB_SIZE IB_FMT DECL DECL_ID DECL_STOCK FVF BASE_VERTEX MIN_VERTEX NUM_VERTICES START_INDEX START_VERTEX PRIMITIVE_COUNT RANGE_FIRST RANGE_LAST RANGE_OK ALPHABLEND SRCBLEND DESTBLEND ZWRITE CULLMODE\n"
                );
                std::fflush(input_log);
            }
            else {
                console_output(
                    "D3D9 draw-input diagnostic: could not create chimera_d3d9_model_vertex_input.log."
                );
            }
        }

        static const char *classify_vertex_shader(IDirect3DVertexShader9 *shader) noexcept {
            if(!shader) {
                return nullptr;
            }

            if(shader == D3D9ModelShaderPrimaryV2::model_shader) {
                return "PRIMARY_VS2_MODEL";
            }
            if(shader == D3D9ModelShaderPrimaryV2::fogged_shader) {
                return "PRIMARY_VS2_FOGGED";
            }
            if(shader == D3D9ModelShaderPrimaryV2::active_camo_shader) {
                return "PRIMARY_VS2_ACTIVE_CAMO";
            }
            if(shader == D3D9ModelShaderPrimaryV2::shadow_shader) {
                return "PRIMARY_VS2_SHADOW";
            }

            if(!vertex_shaders) {
                return nullptr;
            }

            struct Candidate {
                VertexShaderIndex index;
                const char *name;
            };

            static constexpr Candidate candidates[] = {
                {VSH_MODEL_FOGGED,                         "VSH_MODEL_FOGGED_STOCK"},
                {VSH_MODEL,                                "VSH_MODEL_STOCK"},
                {VSH_MODEL_FF,                             "VSH_MODEL_FF_STOCK"},
                {VSH_MODEL_FAST,                           "VSH_MODEL_FAST_STOCK"},
                {VSH_MODEL_SCENERY,                        "VSH_MODEL_SCENERY_STOCK"},
                {VSH_MODEL_ACTIVE_CAMOUFLAGE,              "VSH_MODEL_ACTIVE_CAMOUFLAGE_STOCK"},
                {VSH_MODEL_ACTIVE_CAMOUFLAGE_FF,           "VSH_MODEL_ACTIVE_CAMOUFLAGE_FF_STOCK"},
                {VSH_MODEL_FOG_SCREEN,                     "VSH_MODEL_FOG_SCREEN_STOCK"},
                {VSH_MODEL_SHADOW,                         "VSH_MODEL_SHADOW_STOCK"},
                {VSH_MODEL_ZBUFFER,                        "VSH_MODEL_ZBUFFER_STOCK"},

                {VSH_EFFECT,                               "VSH_EFFECT"},
                {VSH_EFFECT_MULTITEXTURE,                  "VSH_EFFECT_MULTITEXTURE"},
                {VSH_EFFECT_MULTITEXTURE_SCREENSPACE,      "VSH_EFFECT_MULTITEXTURE_SCREENSPACE"},
                {VSH_EFFECT_ZSPRITE,                       "VSH_EFFECT_ZSPRITE"},

                {VSH_TRANSPARENT_GENERIC,                  "VSH_TRANSPARENT_GENERIC"},
                {VSH_TRANSPARENT_GENERIC_LIT_M,            "VSH_TRANSPARENT_GENERIC_LIT_M"},
                {VSH_TRANSPARENT_GENERIC_M,                "VSH_TRANSPARENT_GENERIC_M"},
                {VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED,  "VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED"},
                {VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED_M,"VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED_M"},
                {VSH_TRANSPARENT_GENERIC_REFLECTION,       "VSH_TRANSPARENT_GENERIC_REFLECTION"},
                {VSH_TRANSPARENT_GENERIC_REFLECTION_M,     "VSH_TRANSPARENT_GENERIC_REFLECTION_M"},
                {VSH_TRANSPARENT_GENERIC_SCREENSPACE,      "VSH_TRANSPARENT_GENERIC_SCREENSPACE"},
                {VSH_TRANSPARENT_GENERIC_SCREENSPACE_M,    "VSH_TRANSPARENT_GENERIC_SCREENSPACE_M"},
                {VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED,  "VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED"},
                {VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M,"VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M"},
                {VSH_TRANSPARENT_GLASS_DIFFUSE_LIGHT,      "VSH_TRANSPARENT_GLASS_DIFFUSE_LIGHT"},
                {VSH_TRANSPARENT_GLASS_DIFFUSE_LIGHT_M,    "VSH_TRANSPARENT_GLASS_DIFFUSE_LIGHT_M"},
                {VSH_TRANSPARENT_GLASS_REFLECTION_BUMPED,  "VSH_TRANSPARENT_GLASS_REFLECTION_BUMPED"},
                {VSH_TRANSPARENT_GLASS_REFLECTION_BUMPED_M,"VSH_TRANSPARENT_GLASS_REFLECTION_BUMPED_M"},
                {VSH_TRANSPARENT_GLASS_REFLECTION_FLAT,    "VSH_TRANSPARENT_GLASS_REFLECTION_FLAT"},
                {VSH_TRANSPARENT_GLASS_REFLECTION_FLAT_M,  "VSH_TRANSPARENT_GLASS_REFLECTION_FLAT_M"},
                {VSH_TRANSPARENT_GLASS_REFLECTION_MIRROR,  "VSH_TRANSPARENT_GLASS_REFLECTION_MIRROR"},
                {VSH_TRANSPARENT_GLASS_TINT,               "VSH_TRANSPARENT_GLASS_TINT"},
                {VSH_TRANSPARENT_GLASS_TINT_M,             "VSH_TRANSPARENT_GLASS_TINT_M"},
                {VSH_TRANSPARENT_METER,                    "VSH_TRANSPARENT_METER"},
                {VSH_TRANSPARENT_METER_M,                  "VSH_TRANSPARENT_METER_M"},
                {VSH_TRANSPARENT_PLASMA_M,                 "VSH_TRANSPARENT_PLASMA_M"},
                {VSH_TRANSPARENT_WATER_OPACITY,            "VSH_TRANSPARENT_WATER_OPACITY"},
                {VSH_TRANSPARENT_WATER_OPACITY_M,          "VSH_TRANSPARENT_WATER_OPACITY_M"},
                {VSH_TRANSPARENT_WATER_REFLECTION,         "VSH_TRANSPARENT_WATER_REFLECTION"},
                {VSH_TRANSPARENT_WATER_REFLECTION_M,       "VSH_TRANSPARENT_WATER_REFLECTION_M"},
            };

            for(const auto &candidate : candidates) {
                if(vertex_shaders[candidate.index].shader
                    && shader == vertex_shaders[candidate.index].shader) {
                    return candidate.name;
                }
            }
            return nullptr;
        }

        static int find_stock_declaration(IDirect3DVertexDeclaration9 *declaration) noexcept {
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

        static const char *stock_declaration_name(int index) noexcept {
            switch(index) {
                case VERTEX_DECLARATION_MODEL_UNCOMPRESSED: return "MODEL_UNCOMPRESSED";
                case VERTEX_DECLARATION_MODEL_COMPRESSED: return "MODEL_COMPRESSED";
                case VERTEX_DECLARATION_MODEL_UNCOMPRESSED_FF: return "MODEL_UNCOMPRESSED_FF";
                case VERTEX_DECLARATION_MODEL_PROCESSED: return "MODEL_PROCESSED";
                case -1: return "UNKNOWN";
                default: return "NON_MODEL_DECL";
            }
        }

        static void log_declaration_definition(
            std::uint32_t id,
            IDirect3DVertexDeclaration9 *declaration
        ) noexcept {
            if(!input_log || !declaration) {
                return;
            }

            const int stock_index = find_stock_declaration(declaration);
            UINT count = 0;
            HRESULT result = declaration->GetDeclaration(nullptr, &count);
            if(FAILED(result) || count == 0 || count > 32) {
                std::fprintf(
                    input_log,
                    "DECL_DEF id=%lu ptr=%p stock=%d stock_name=%s GetDeclaration_hr=0x%08lX count=%u\n",
                    static_cast<unsigned long>(id),
                    static_cast<void *>(declaration),
                    stock_index,
                    stock_declaration_name(stock_index),
                    static_cast<unsigned long>(result),
                    count
                );
                return;
            }

            D3DVERTEXELEMENT9 elements[32] = {};
            UINT supplied = count;
            result = declaration->GetDeclaration(elements, &supplied);
            std::fprintf(
                input_log,
                "DECL_DEF id=%lu ptr=%p stock=%d stock_name=%s elements=%u hr=0x%08lX\n",
                static_cast<unsigned long>(id),
                static_cast<void *>(declaration),
                stock_index,
                stock_declaration_name(stock_index),
                supplied,
                static_cast<unsigned long>(result)
            );

            if(SUCCEEDED(result)) {
                for(UINT i = 0; i < supplied; i++) {
                    const auto &element = elements[i];
                    std::fprintf(
                        input_log,
                        "DECL_E id=%lu e=%u stream=%u offset=%u type=%u method=%u usage=%u usage_index=%u\n",
                        static_cast<unsigned long>(id),
                        i,
                        static_cast<unsigned>(element.Stream),
                        static_cast<unsigned>(element.Offset),
                        static_cast<unsigned>(element.Type),
                        static_cast<unsigned>(element.Method),
                        static_cast<unsigned>(element.Usage),
                        static_cast<unsigned>(element.UsageIndex)
                    );
                }
            }
            std::fflush(input_log);
        }

        static std::uint32_t declaration_id(IDirect3DVertexDeclaration9 *declaration) noexcept {
            if(!declaration) {
                return 0;
            }

            for(std::size_t i = 0; i < seen_declaration_count; i++) {
                if(seen_declarations[i].declaration == declaration) {
                    return seen_declarations[i].id;
                }
            }

            if(seen_declaration_count >= MAX_SEEN_DECLARATIONS) {
                return 0xFFFFFFFFu;
            }

            const std::uint32_t id = static_cast<std::uint32_t>(seen_declaration_count + 1);
            seen_declarations[seen_declaration_count++] = {declaration, id};
            log_declaration_definition(id, declaration);
            return id;
        }

        static UINT vertex_buffer_size(IDirect3DVertexBuffer9 *buffer) noexcept {
            if(!buffer) {
                return 0;
            }
            D3DVERTEXBUFFER_DESC desc = {};
            return SUCCEEDED(buffer->GetDesc(&desc)) ? desc.Size : 0;
        }

        static void index_buffer_description(
            IDirect3DIndexBuffer9 *buffer,
            UINT &size,
            D3DFORMAT &format
        ) noexcept {
            size = 0;
            format = D3DFMT_UNKNOWN;
            if(!buffer) {
                return;
            }
            D3DINDEXBUFFER_DESC desc = {};
            if(SUCCEEDED(buffer->GetDesc(&desc))) {
                size = desc.Size;
                format = desc.Format;
            }
        }

        static UINT primitive_vertex_count(D3DPRIMITIVETYPE type, UINT primitive_count) noexcept {
            switch(type) {
                case D3DPT_POINTLIST:     return primitive_count;
                case D3DPT_LINELIST:      return primitive_count * 2U;
                case D3DPT_LINESTRIP:     return primitive_count + 1U;
                case D3DPT_TRIANGLELIST:  return primitive_count * 3U;
                case D3DPT_TRIANGLESTRIP:
                case D3DPT_TRIANGLEFAN:   return primitive_count + 2U;
                default:                  return 0U;
            }
        }

        static void get_render_state(IDirect3DDevice9 *device, D3DRENDERSTATETYPE state, DWORD &value) noexcept {
            value = 0xFFFFFFFFu;
            if(device) {
                device->GetRenderState(state, &value);
            }
        }

        static void log_draw(
            IDirect3DDevice9 *device,
            const char *kind,
            const char *pass_name,
            D3DPRIMITIVETYPE primitive_type,
            INT base_vertex_index,
            UINT min_vertex_index,
            UINT num_vertices,
            UINT start_index,
            UINT start_vertex,
            UINT primitive_count,
            bool indexed
        ) noexcept {
            ensure_input_log();
            if(!input_log) {
                return;
            }

            IDirect3DVertexBuffer9 *vb0 = nullptr;
            IDirect3DVertexBuffer9 *vb1 = nullptr;
            IDirect3DIndexBuffer9 *ib = nullptr;
            IDirect3DVertexDeclaration9 *declaration = nullptr;
            IDirect3DPixelShader9 *pixel_shader = nullptr;
            UINT offset0 = 0;
            UINT stride0 = 0;
            UINT offset1 = 0;
            UINT stride1 = 0;
            DWORD fvf = 0;

            device->GetStreamSource(0, &vb0, &offset0, &stride0);
            device->GetStreamSource(1, &vb1, &offset1, &stride1);
            if(indexed) {
                device->GetIndices(&ib);
            }
            device->GetVertexDeclaration(&declaration);
            device->GetFVF(&fvf);
            device->GetPixelShader(&pixel_shader);

            const UINT vb0_size = vertex_buffer_size(vb0);
            const UINT vb1_size = vertex_buffer_size(vb1);
            const unsigned long long vb0_capacity = stride0 > 0 && vb0_size >= offset0
                ? static_cast<unsigned long long>((vb0_size - offset0) / stride0)
                : 0ULL;

            UINT ib_size = 0;
            D3DFORMAT ib_format = D3DFMT_UNKNOWN;
            index_buffer_description(ib, ib_size, ib_format);

            const std::uint32_t decl_id = declaration_id(declaration);
            const int stock_decl = find_stock_declaration(declaration);

            long long range_first = 0;
            long long range_last = -1;
            if(indexed) {
                range_first = static_cast<long long>(base_vertex_index)
                    + static_cast<long long>(min_vertex_index);
                range_last = num_vertices > 0
                    ? range_first + static_cast<long long>(num_vertices) - 1LL
                    : range_first;
            }
            else {
                range_first = static_cast<long long>(start_vertex);
                range_last = num_vertices > 0
                    ? range_first + static_cast<long long>(num_vertices) - 1LL
                    : range_first;
            }

            const bool range_ok = vb0_capacity > 0
                && range_first >= 0
                && range_last >= range_first
                && static_cast<unsigned long long>(range_last) < vb0_capacity;

            DWORD alpha_blend = 0;
            DWORD src_blend = 0;
            DWORD dest_blend = 0;
            DWORD zwrite = 0;
            DWORD cull_mode = 0;
            get_render_state(device, D3DRS_ALPHABLENDENABLE, alpha_blend);
            get_render_state(device, D3DRS_SRCBLEND, src_blend);
            get_render_state(device, D3DRS_DESTBLEND, dest_blend);
            get_render_state(device, D3DRS_ZWRITEENABLE, zwrite);
            get_render_state(device, D3DRS_CULLMODE, cull_mode);

            const long long frame = rasterizer_globals
                ? static_cast<long long>(rasterizer_globals->frame_index)
                : -1LL;
            const unsigned long long draw = ++draw_counter;

            std::fprintf(
                input_log,
                "D3D9_DRAW_INPUT kind=%s frame=%lld draw=%llu pass=%s PS=%p prim=%u "
                "VB0=%p OFFSET0=%u STRIDE0=%u VB0_SIZE=%u VB0_CAP=%llu "
                "VB1=%p OFFSET1=%u STRIDE1=%u VB1_SIZE=%u "
                "IB=%p IB_SIZE=%u IB_FMT=%u DECL=%p DECL_ID=%lu DECL_STOCK=%d DECL_NAME=%s FVF=0x%08lX "
                "BASE_VERTEX=%d MIN_VERTEX=%u NUM_VERTICES=%u START_INDEX=%u START_VERTEX=%u PRIMITIVE_COUNT=%u "
                "RANGE_FIRST=%lld RANGE_LAST=%lld RANGE_OK=%u "
                "ALPHABLEND=%lu SRCBLEND=%lu DESTBLEND=%lu ZWRITE=%lu CULLMODE=%lu\n",
                kind,
                frame,
                draw,
                pass_name,
                static_cast<void *>(pixel_shader),
                static_cast<unsigned>(primitive_type),
                static_cast<void *>(vb0),
                offset0,
                stride0,
                vb0_size,
                vb0_capacity,
                static_cast<void *>(vb1),
                offset1,
                stride1,
                vb1_size,
                static_cast<void *>(ib),
                ib_size,
                static_cast<unsigned>(ib_format),
                static_cast<void *>(declaration),
                static_cast<unsigned long>(decl_id),
                stock_decl,
                stock_declaration_name(stock_decl),
                static_cast<unsigned long>(fvf),
                base_vertex_index,
                min_vertex_index,
                num_vertices,
                start_index,
                start_vertex,
                primitive_count,
                range_first,
                range_last,
                range_ok ? 1U : 0U,
                static_cast<unsigned long>(alpha_blend),
                static_cast<unsigned long>(src_blend),
                static_cast<unsigned long>(dest_blend),
                static_cast<unsigned long>(zwrite),
                static_cast<unsigned long>(cull_mode)
            );

            if((++log_flush_counter & 63u) == 0u) {
                std::fflush(input_log);
            }

            if(pixel_shader) pixel_shader->Release();
            if(declaration) declaration->Release();
            if(ib) ib->Release();
            if(vb1) vb1->Release();
            if(vb0) vb0->Release();
        }

        static const char *current_pass(IDirect3DDevice9 *device) noexcept {
            IDirect3DVertexShader9 *vertex_shader = nullptr;
            if(!device || FAILED(device->GetVertexShader(&vertex_shader)) || !vertex_shader) {
                return nullptr;
            }
            const char *pass_name = classify_vertex_shader(vertex_shader);
            vertex_shader->Release();
            return pass_name;
        }

        static HRESULT STDMETHODCALLTYPE draw_primitive_hook(
            IDirect3DDevice9 *device,
            D3DPRIMITIVETYPE primitive_type,
            UINT start_vertex,
            UINT primitive_count
        ) noexcept {
            if(!original_draw_primitive) {
                return D3DERR_INVALIDCALL;
            }

            if(trace_enabled() && installed_device == device) {
                const char *pass_name = current_pass(device);
                if(pass_name) {
                    const UINT vertex_count = primitive_vertex_count(primitive_type, primitive_count);
                    log_draw(
                        device,
                        "DP",
                        pass_name,
                        primitive_type,
                        0,
                        0,
                        vertex_count,
                        0,
                        start_vertex,
                        primitive_count,
                        false
                    );
                }
            }

            return original_draw_primitive(device, primitive_type, start_vertex, primitive_count);
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

            if(trace_enabled() && installed_device == device) {
                const char *pass_name = current_pass(device);
                if(pass_name) {
                    log_draw(
                        device,
                        "DIP",
                        pass_name,
                        primitive_type,
                        base_vertex_index,
                        min_vertex_index,
                        num_vertices,
                        start_index,
                        0,
                        primitive_count,
                        true
                    );
                }
            }

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

        static bool patch_vtable_entry(
            ULONG_PTR *entry,
            ULONG_PTR replacement,
            ULONG_PTR &original
        ) noexcept {
            if(!entry) {
                return false;
            }
            if(*entry == replacement) {
                return original != 0;
            }

            original = *entry;
            if(!original) {
                return false;
            }

            DWORD old_protection = 0;
            if(!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE, &old_protection)) {
                original = 0;
                return false;
            }

            *entry = replacement;
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(*entry), old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(*entry));
            return true;
        }

        static bool install(IDirect3DDevice9 *device) noexcept {
            if(!device || !trace_enabled()) {
                return false;
            }

            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) {
                return false;
            }

            ULONG_PTR original_dp = reinterpret_cast<ULONG_PTR>(original_draw_primitive);
            ULONG_PTR original_dip = reinterpret_cast<ULONG_PTR>(original_draw_indexed_primitive);

            const bool dp_ok = patch_vtable_entry(
                &vtable[DEVICE_DRAW_PRIMITIVE],
                reinterpret_cast<ULONG_PTR>(draw_primitive_hook),
                original_dp
            );
            const bool dip_ok = patch_vtable_entry(
                &vtable[DEVICE_DRAW_INDEXED_PRIMITIVE],
                reinterpret_cast<ULONG_PTR>(draw_indexed_primitive_hook),
                original_dip
            );

            original_draw_primitive = reinterpret_cast<DrawPrimitiveFunction>(original_dp);
            original_draw_indexed_primitive = reinterpret_cast<DrawIndexedPrimitiveFunction>(original_dip);

            if(!dp_ok && !dip_ok) {
                return false;
            }

            installed_device = device;
            ensure_input_log();

            if(!installed_announced) {
                console_output(
                    "D3D9 backend: model/effect/transparent draw-input trace enabled on D3D9On12."
                );
                console_output(
                    "D3D9 draw-input trace -> chimera_d3d9_model_vertex_input.log."
                );
                installed_announced = true;
            }
            return true;
        }

        static void on_end_scene(IDirect3DDevice9 *device) noexcept {
            if(trace_enabled()) {
                install(device);
            }
        }

        static void set_up() noexcept {
            if(!trace_enabled()) {
                return;
            }

            if(!queued_announced) {
                console_output(
                    "D3D9 backend: model/effect/transparent draw-input diagnostic requested; waiting for live D3D9 device."
                );
                queued_announced = true;
            }

            if(!end_scene_retry_registered) {
                add_d3d9_end_scene_event(on_end_scene);
                end_scene_retry_registered = true;
            }

            if(global_d3d9_device && *global_d3d9_device) {
                install(*global_d3d9_device);
            }
        }
    }

    inline void set_up_d3d9_model_vertex_input_diag() noexcept {
        D3D9ModelVertexInputDiag::set_up();
    }
}

#endif
