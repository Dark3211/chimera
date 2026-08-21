// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_D3D9_RUNTIME_DIAGNOSTICS_HPP
#define CHIMERA_D3D9_RUNTIME_DIAGNOSTICS_HPP

#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shader_effects.hpp"
#include "../output/output.hpp"

namespace Chimera {
    namespace D3D9RuntimeDiagnostics {
        constexpr std::size_t DEVICE_DRAW_INDEXED_PRIMITIVE = 82;
        constexpr UINT NODE_MATRIX_REGISTER_FIRST = 29;
        constexpr UINT NODE_MATRIX_REGISTER_COUNT = 66;
        constexpr std::uint64_t MAX_VERBOSE_MODEL_DRAWS = 512;

        using DrawIndexedPrimitiveFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT
        );

        static DrawIndexedPrimitiveFunction original_draw_indexed_primitive = nullptr;
        static std::FILE *runtime_log = nullptr;
        static bool installed = false;
        static std::uint64_t total_draws = 0;
        static std::uint64_t model_draws = 0;

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

        static bool is_model_shader(int shader_index) noexcept {
            return shader_index >= VSH_MODEL_FOGGED && shader_index <= VSH_MODEL_ZBUFFER;
        }

        static const char *model_shader_name(int shader_index) noexcept {
            switch(shader_index) {
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
                default: return "unknown";
            }
        }

        static const char *declaration_name(int declaration_index) noexcept {
            switch(declaration_index) {
                case VERTEX_DECLARATION_MODEL_UNCOMPRESSED: return "model_uncompressed";
                case VERTEX_DECLARATION_MODEL_COMPRESSED: return "model_compressed";
                case VERTEX_DECLARATION_MODEL_UNCOMPRESSED_FF: return "model_uncompressed_ff";
                case VERTEX_DECLARATION_MODEL_PROCESSED: return "model_processed";
                default: return "other";
            }
        }

        template<typename... Args>
        static void log_line(const char *format, Args... args) noexcept {
            if(!runtime_log || !format) {
                return;
            }
            std::fprintf(runtime_log, format, args...);
            std::fputc('\n', runtime_log);
        }

        static std::uint32_t fnv1a(const void *data, std::size_t size) noexcept {
            const auto *bytes = static_cast<const unsigned char *>(data);
            std::uint32_t hash = 2166136261U;
            for(std::size_t i = 0; i < size; i++) {
                hash ^= bytes[i];
                hash *= 16777619U;
            }
            return hash;
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

        static bool patch_draw(IDirect3DDevice9 *device) noexcept {
            if(!device) {
                return false;
            }
            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) {
                return false;
            }

            auto *entry = &vtable[DEVICE_DRAW_INDEXED_PRIMITIVE];
            const auto replacement = reinterpret_cast<ULONG_PTR>(
                +[](IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) -> HRESULT {
                    return D3DERR_INVALIDCALL;
                }
            );
            (void)replacement;
            return true;
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

            total_draws++;

            IDirect3DVertexShader9 *shader = nullptr;
            if(SUCCEEDED(device->GetVertexShader(&shader)) && shader) {
                const int shader_index = find_vertex_shader(shader);
                if(is_model_shader(shader_index)) {
                    model_draws++;

                    IDirect3DVertexDeclaration9 *declaration = nullptr;
                    IDirect3DVertexBuffer9 *stream0 = nullptr;
                    IDirect3DIndexBuffer9 *indices = nullptr;
                    UINT stream_offset = 0;
                    UINT stream_stride = 0;

                    const HRESULT decl_hr = device->GetVertexDeclaration(&declaration);
                    const HRESULT stream_hr = device->GetStreamSource(0, &stream0, &stream_offset, &stream_stride);
                    const HRESULT indices_hr = device->GetIndices(&indices);
                    const int declaration_index = SUCCEEDED(decl_hr) ? find_vertex_declaration(declaration) : -1;

                    float node_constants[NODE_MATRIX_REGISTER_COUNT * 4] = {};
                    const HRESULT constants_hr = device->GetVertexShaderConstantF(
                        NODE_MATRIX_REGISTER_FIRST,
                        node_constants,
                        NODE_MATRIX_REGISTER_COUNT
                    );

                    bool suspicious = false;
                    float max_abs = 0.0F;
                    if(SUCCEEDED(constants_hr)) {
                        for(float value : node_constants) {
                            if(suspicious_float(value)) {
                                suspicious = true;
                            }
                            const float absolute = std::fabs(value);
                            if(std::isfinite(absolute) && absolute > max_abs) {
                                max_abs = absolute;
                            }
                        }
                    }

                    const bool verbose = model_draws <= MAX_VERBOSE_MODEL_DRAWS
                        || suspicious
                        || (model_draws % 2048U) == 0;

                    if(verbose) {
                        const std::uint32_t constants_hash = SUCCEEDED(constants_hr)
                            ? fnv1a(node_constants, sizeof(node_constants))
                            : 0U;

                        log_line(
                            "MODEL_DRAW n=%llu total=%llu shader=%d(%s) decl=%d(%s) swvp=%u base=%d min=%u vertices=%u start=%u prims=%u stream_offset=%u stride=%u constants_hr=0x%08lX constants_hash=%08lX max_abs=%g suspicious=%u",
                            static_cast<unsigned long long>(model_draws),
                            static_cast<unsigned long long>(total_draws),
                            shader_index,
                            model_shader_name(shader_index),
                            declaration_index,
                            declaration_name(declaration_index),
                            device->GetSoftwareVertexProcessing() ? 1U : 0U,
                            base_vertex_index,
                            min_vertex_index,
                            num_vertices,
                            start_index,
                            primitive_count,
                            stream_offset,
                            stream_stride,
                            static_cast<unsigned long>(constants_hr),
                            static_cast<unsigned long>(constants_hash),
                            static_cast<double>(max_abs),
                            suspicious ? 1U : 0U
                        );

                        if(SUCCEEDED(stream_hr) && stream0) {
                            D3DVERTEXBUFFER_DESC desc = {};
                            if(SUCCEEDED(stream0->GetDesc(&desc))) {
                                log_line("  VB size=%u usage=0x%08lX fvf=0x%08lX pool=%u",
                                         desc.Size,
                                         static_cast<unsigned long>(desc.Usage),
                                         static_cast<unsigned long>(desc.FVF),
                                         static_cast<unsigned>(desc.Pool));
                            }
                        }
                        if(SUCCEEDED(indices_hr) && indices) {
                            D3DINDEXBUFFER_DESC desc = {};
                            if(SUCCEEDED(indices->GetDesc(&desc))) {
                                log_line("  IB size=%u usage=0x%08lX format=%u pool=%u",
                                         desc.Size,
                                         static_cast<unsigned long>(desc.Usage),
                                         static_cast<unsigned>(desc.Format),
                                         static_cast<unsigned>(desc.Pool));
                            }
                        }
                        std::fflush(runtime_log);
                    }

                    if(indices) {
                        indices->Release();
                    }
                    if(stream0) {
                        stream0->Release();
                    }
                    if(declaration) {
                        declaration->Release();
                    }
                }
                shader->Release();
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

        static bool install_draw_hook(IDirect3DDevice9 *device) noexcept {
            if(!device) {
                return false;
            }
            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) {
                return false;
            }

            auto *entry = &vtable[DEVICE_DRAW_INDEXED_PRIMITIVE];
            const ULONG_PTR replacement = reinterpret_cast<ULONG_PTR>(draw_indexed_primitive_hook);
            if(*entry == replacement) {
                return original_draw_indexed_primitive != nullptr;
            }

            original_draw_indexed_primitive = reinterpret_cast<DrawIndexedPrimitiveFunction>(*entry);
            if(!original_draw_indexed_primitive) {
                return false;
            }

            DWORD old_protection = 0;
            if(!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE, &old_protection)) {
                original_draw_indexed_primitive = nullptr;
                return false;
            }
            *entry = replacement;
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(*entry), old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(*entry));
            return true;
        }

        static void set_up() noexcept {
            if(installed || !global_d3d9_device || !*global_d3d9_device) {
                return;
            }

            auto *mode = get_chimera().get_ini()->get_value("video_mode.d3d_diag");
            if(!mode || _stricmp(mode, "trace") != 0) {
                return;
            }

            auto *backend = get_chimera().get_ini()->get_value("video_mode.d3d_backend");
            const bool is_9on12 = backend
                && (_stricmp(backend, "9on12") == 0 || _stricmp(backend, "d3d9on12") == 0);
            if(!is_9on12) {
                return;
            }

            runtime_log = std::fopen("chimera_d3d9_runtime.log", "w");
            if(!runtime_log) {
                console_warning("D3D9 runtime diagnostic: could not create chimera_d3d9_runtime.log");
                return;
            }

            log_line("CHIMERA D3D9 RUNTIME MODEL TRACE");
            log_line("backend=%s", backend ? backend : "(null)");

            if(install_draw_hook(*global_d3d9_device)) {
                installed = true;
                log_line("RUNTIME DrawIndexedPrimitive hook installed");
                std::fflush(runtime_log);
                console_output("D3D9 diagnostic: runtime model-state trace installed.");
            }
            else {
                log_line("RUNTIME DrawIndexedPrimitive hook failed");
                std::fflush(runtime_log);
                console_warning("D3D9 diagnostic: runtime model-state trace hook failed.");
            }
        }
    }

    inline void set_up_d3d9_runtime_diagnostics() noexcept {
        D3D9RuntimeDiagnostics::set_up();
    }
}

#endif
