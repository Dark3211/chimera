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
        constexpr std::size_t MAX_SEEN_SIGNATURES = 1024;
        constexpr long long HEARTBEAT_FRAME_INTERVAL = 100;
        constexpr std::size_t RECENT_FRAME_COUNT = 64;

        using DrawPrimitiveFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT
        );
        using DrawIndexedPrimitiveFunction = HRESULT (STDMETHODCALLTYPE *)(
            IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT
        );

        enum ProbeGroup : std::uint32_t {
            PROBE_GENERIC_M = 0,
            PROBE_GENERIC,
            PROBE_OBJECT_CENTERED,
            PROBE_VIEWER_CENTERED,
            PROBE_REFLECTION,
            PROBE_SCREENSPACE,
            PROBE_EFFECT,
            PROBE_GLASS,
            PROBE_METER,
            PROBE_PLASMA,
            PROBE_PRIMARY_MODEL,
            PROBE_SHADOW,
            PROBE_SCENERY,
            PROBE_GROUP_COUNT
        };

        constexpr std::size_t PROBE_CYCLE_COUNT = 10;
        static constexpr ProbeGroup probe_cycle[PROBE_CYCLE_COUNT] = {
            PROBE_GENERIC_M,
            PROBE_GENERIC,
            PROBE_OBJECT_CENTERED,
            PROBE_VIEWER_CENTERED,
            PROBE_REFLECTION,
            PROBE_SCREENSPACE,
            PROBE_EFFECT,
            PROBE_GLASS,
            PROBE_METER,
            PROBE_PLASMA
        };

        struct SeenSignature {
            std::uint64_t hash = 0;
            std::uint32_t id = 0;
            long long last_log_frame = -1;
            unsigned long long hits_since_log = 0;
        };

        struct RecentFrame {
            long long frame = -1;
            unsigned long long draws[PROBE_GROUP_COUNT] = {};
            unsigned long long skipped[PROBE_GROUP_COUNT] = {};
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
        static SeenSignature seen_signatures[MAX_SEEN_SIGNATURES] = {};
        static std::size_t seen_signature_count = 0;

        static std::uint32_t probe_disabled_mask = 0;
        static int probe_cycle_index = -1;
        static unsigned long long probe_skipped_total[PROBE_GROUP_COUNT] = {};
        static RecentFrame recent_frames[RECENT_FRAME_COUNT] = {};
        static std::size_t recent_frame_cursor = 0;
        static bool recent_frame_initialized = false;

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
                || _stricmp(value, "compact") == 0
            );
        }

        static long long current_frame() noexcept {
            return rasterizer_globals
                ? static_cast<long long>(rasterizer_globals->frame_index)
                : -1LL;
        }

        static const char *probe_group_name(ProbeGroup group) noexcept {
            switch(group) {
                case PROBE_GENERIC_M:       return "generic_m";
                case PROBE_GENERIC:         return "generic";
                case PROBE_OBJECT_CENTERED: return "object_centered";
                case PROBE_VIEWER_CENTERED: return "viewer_centered";
                case PROBE_REFLECTION:      return "reflection";
                case PROBE_SCREENSPACE:     return "screenspace";
                case PROBE_EFFECT:          return "effect";
                case PROBE_GLASS:           return "glass";
                case PROBE_METER:           return "meter";
                case PROBE_PLASMA:          return "plasma";
                case PROBE_PRIMARY_MODEL:   return "primary_model";
                case PROBE_SHADOW:          return "shadow";
                case PROBE_SCENERY:         return "scenery";
                default:                    return "unknown";
            }
        }

        static bool starts_with(const char *value, const char *prefix) noexcept {
            if(!value || !prefix) {
                return false;
            }
            return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
        }

        static bool pass_matches_group(const char *pass_name, ProbeGroup group) noexcept {
            if(!pass_name) {
                return false;
            }
            switch(group) {
                case PROBE_GENERIC_M:
                    return std::strcmp(pass_name, "VSH_TRANSPARENT_GENERIC_M") == 0;
                case PROBE_GENERIC:
                    return std::strcmp(pass_name, "VSH_TRANSPARENT_GENERIC") == 0;
                case PROBE_OBJECT_CENTERED:
                    return std::strcmp(pass_name, "VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED") == 0
                        || std::strcmp(pass_name, "VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED_M") == 0;
                case PROBE_VIEWER_CENTERED:
                    return std::strcmp(pass_name, "VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED") == 0
                        || std::strcmp(pass_name, "VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M") == 0;
                case PROBE_REFLECTION:
                    return std::strcmp(pass_name, "VSH_TRANSPARENT_GENERIC_REFLECTION") == 0
                        || std::strcmp(pass_name, "VSH_TRANSPARENT_GENERIC_REFLECTION_M") == 0;
                case PROBE_SCREENSPACE:
                    return std::strcmp(pass_name, "VSH_TRANSPARENT_GENERIC_SCREENSPACE") == 0
                        || std::strcmp(pass_name, "VSH_TRANSPARENT_GENERIC_SCREENSPACE_M") == 0;
                case PROBE_EFFECT:
                    return starts_with(pass_name, "VSH_EFFECT");
                case PROBE_GLASS:
                    return starts_with(pass_name, "VSH_TRANSPARENT_GLASS_");
                case PROBE_METER:
                    return starts_with(pass_name, "VSH_TRANSPARENT_METER");
                case PROBE_PLASMA:
                    return std::strcmp(pass_name, "VSH_TRANSPARENT_PLASMA_M") == 0;
                case PROBE_PRIMARY_MODEL:
                    return std::strcmp(pass_name, "PRIMARY_VS2_MODEL") == 0
                        || std::strcmp(pass_name, "PRIMARY_VS2_FOGGED") == 0
                        || std::strcmp(pass_name, "PRIMARY_VS2_ACTIVE_CAMO") == 0;
                case PROBE_SHADOW:
                    return std::strcmp(pass_name, "PRIMARY_VS2_SHADOW") == 0;
                case PROBE_SCENERY:
                    return std::strcmp(pass_name, "VSH_MODEL_SCENERY_STOCK") == 0;
                default:
                    return false;
            }
        }

        static int probe_group_for_pass(const char *pass_name) noexcept {
            for(std::uint32_t i = 0; i < PROBE_GROUP_COUNT; i++) {
                if(pass_matches_group(pass_name, static_cast<ProbeGroup>(i))) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        static ProbeGroup probe_group_from_name(const char *name, bool &ok) noexcept {
            ok = true;
            if(!name) {
                ok = false;
                return PROBE_GENERIC_M;
            }
            for(std::uint32_t i = 0; i < PROBE_GROUP_COUNT; i++) {
                const auto group = static_cast<ProbeGroup>(i);
                if(_stricmp(name, probe_group_name(group)) == 0) {
                    return group;
                }
            }
            if(_stricmp(name, "model") == 0) return PROBE_PRIMARY_MODEL;
            if(_stricmp(name, "generic-object") == 0) return PROBE_OBJECT_CENTERED;
            if(_stricmp(name, "generic-viewer") == 0) return PROBE_VIEWER_CENTERED;
            ok = false;
            return PROBE_GENERIC_M;
        }

        static bool probe_group_disabled(ProbeGroup group) noexcept {
            return (probe_disabled_mask & (1u << static_cast<std::uint32_t>(group))) != 0;
        }

        static void set_probe_group(ProbeGroup group, bool disabled) noexcept {
            const auto bit = 1u << static_cast<std::uint32_t>(group);
            if(disabled) {
                probe_disabled_mask |= bit;
            }
            else {
                probe_disabled_mask &= ~bit;
            }
        }

        static void ensure_recent_frame(long long frame) noexcept {
            if(frame < 0) {
                return;
            }
            if(!recent_frame_initialized) {
                recent_frame_initialized = true;
                recent_frame_cursor = 0;
                recent_frames[0] = {};
                recent_frames[0].frame = frame;
                return;
            }
            if(recent_frames[recent_frame_cursor].frame == frame) {
                return;
            }
            recent_frame_cursor = (recent_frame_cursor + 1) % RECENT_FRAME_COUNT;
            recent_frames[recent_frame_cursor] = {};
            recent_frames[recent_frame_cursor].frame = frame;
        }

        static void record_probe_activity(const char *pass_name, bool skipped) noexcept {
            const int group_index = probe_group_for_pass(pass_name);
            if(group_index < 0) {
                return;
            }
            const long long frame = current_frame();
            ensure_recent_frame(frame);
            if(frame >= 0 && recent_frame_initialized) {
                recent_frames[recent_frame_cursor].draws[group_index]++;
                if(skipped) {
                    recent_frames[recent_frame_cursor].skipped[group_index]++;
                }
            }
            if(skipped) {
                probe_skipped_total[group_index]++;
            }
        }

        static bool should_skip_pass(const char *pass_name) noexcept {
            if(!pass_name || probe_disabled_mask == 0) {
                return false;
            }
            for(std::uint32_t i = 0; i < PROBE_GROUP_COUNT; i++) {
                const auto group = static_cast<ProbeGroup>(i);
                if(probe_group_disabled(group) && pass_matches_group(pass_name, group)) {
                    return true;
                }
            }
            return false;
        }

        static void probe_log_event(const char *action, const char *label = nullptr) noexcept {
            std::FILE *log = std::fopen("chimera_d3d9_probe.log", "a");
            if(!log) {
                return;
            }
            std::fprintf(
                log,
                "PROBE_EVENT frame=%lld action=%s label=%s mask=0x%08lX\n",
                current_frame(),
                action ? action : "unknown",
                label ? label : "-",
                static_cast<unsigned long>(probe_disabled_mask)
            );
            std::fclose(log);
        }

        static void dump_recent_frames(const char *label) noexcept {
            std::FILE *log = std::fopen("chimera_d3d9_probe.log", "a");
            if(!log) {
                console_error("D3D9 probe: could not open chimera_d3d9_probe.log");
                return;
            }

            std::fprintf(
                log,
                "MARK frame=%lld label=%s mask=0x%08lX recent_frames=%lu\n",
                current_frame(),
                label ? label : "manual",
                static_cast<unsigned long>(probe_disabled_mask),
                static_cast<unsigned long>(RECENT_FRAME_COUNT)
            );

            if(recent_frame_initialized) {
                const std::size_t oldest = (recent_frame_cursor + 1) % RECENT_FRAME_COUNT;
                for(std::size_t offset = 0; offset < RECENT_FRAME_COUNT; offset++) {
                    const std::size_t index = (oldest + offset) % RECENT_FRAME_COUNT;
                    const auto &entry = recent_frames[index];
                    if(entry.frame < 0) {
                        continue;
                    }
                    std::fprintf(log, "RECENT frame=%lld", entry.frame);
                    for(std::uint32_t g = 0; g < PROBE_GROUP_COUNT; g++) {
                        if(entry.draws[g] || entry.skipped[g]) {
                            std::fprintf(
                                log,
                                " %s=%llu/%llu",
                                probe_group_name(static_cast<ProbeGroup>(g)),
                                entry.draws[g],
                                entry.skipped[g]
                            );
                        }
                    }
                    std::fprintf(log, "\n");
                }
            }
            std::fprintf(log, "END_MARK\n");
            std::fclose(log);
            console_output("D3D9 probe: marked recent frames -> chimera_d3d9_probe.log");
        }

        static void print_probe_status() noexcept {
            console_output("D3D9 probe status: disabled mask=0x%08lX", static_cast<unsigned long>(probe_disabled_mask));
            if(probe_disabled_mask == 0) {
                console_output("D3D9 probe: all passes enabled (normal rendering).");
            }
            for(std::uint32_t i = 0; i < PROBE_GROUP_COUNT; i++) {
                const auto group = static_cast<ProbeGroup>(i);
                if(probe_group_disabled(group)) {
                    console_output(
                        "  disabled: %s (skipped=%llu)",
                        probe_group_name(group),
                        probe_skipped_total[i]
                    );
                }
            }
        }

        static void print_probe_help() noexcept {
            console_output("chimera_d3d9_probe status");
            console_output("chimera_d3d9_probe next");
            console_output("chimera_d3d9_probe reset");
            console_output("chimera_d3d9_probe toggle <pass>");
            console_output("chimera_d3d9_probe disable <pass>");
            console_output("chimera_d3d9_probe enable <pass>");
            console_output("chimera_d3d9_probe mark [label]");
            console_output("passes: generic_m generic object_centered viewer_centered reflection screenspace effect glass meter plasma primary_model shadow scenery");
        }

        static bool command(int argc, const char **argv) noexcept {
            if(!d3d9on12_requested()) {
                console_error("D3D9 probe is only available when video_mode.d3d_backend=9on12.");
                return false;
            }

            if(argc == 0 || _stricmp(argv[0], "status") == 0) {
                print_probe_status();
                return true;
            }
            if(_stricmp(argv[0], "help") == 0) {
                print_probe_help();
                return true;
            }
            if(_stricmp(argv[0], "reset") == 0) {
                probe_disabled_mask = 0;
                probe_cycle_index = -1;
                probe_log_event("reset");
                console_output("D3D9 probe: reset; all passes enabled.");
                return true;
            }
            if(_stricmp(argv[0], "next") == 0) {
                probe_cycle_index = (probe_cycle_index + 1) % static_cast<int>(PROBE_CYCLE_COUNT);
                probe_disabled_mask = 0;
                const ProbeGroup group = probe_cycle[probe_cycle_index];
                set_probe_group(group, true);
                probe_log_event("next", probe_group_name(group));
                console_output(
                    "D3D9 PROBE %d/%lu: disabled %s",
                    probe_cycle_index + 1,
                    static_cast<unsigned long>(PROBE_CYCLE_COUNT),
                    probe_group_name(group)
                );
                return true;
            }
            if(_stricmp(argv[0], "mark") == 0) {
                const char *label = argc >= 2 ? argv[1] : "manual";
                dump_recent_frames(label);
                return true;
            }

            const bool is_toggle = _stricmp(argv[0], "toggle") == 0;
            const bool is_disable = _stricmp(argv[0], "disable") == 0;
            const bool is_enable = _stricmp(argv[0], "enable") == 0;
            if(is_toggle || is_disable || is_enable) {
                if(argc < 2) {
                    console_error("D3D9 probe: missing pass name.");
                    print_probe_help();
                    return false;
                }
                bool ok = false;
                const ProbeGroup group = probe_group_from_name(argv[1], ok);
                if(!ok) {
                    console_error("D3D9 probe: unknown pass '%s'.", argv[1]);
                    print_probe_help();
                    return false;
                }
                const bool disabled = is_toggle ? !probe_group_disabled(group) : is_disable;
                set_probe_group(group, disabled);
                probe_cycle_index = -1;
                probe_log_event(disabled ? "disable" : "enable", probe_group_name(group));
                console_output(
                    "D3D9 probe: %s %s",
                    disabled ? "disabled" : "enabled",
                    probe_group_name(group)
                );
                return true;
            }

            console_error("D3D9 probe: unknown action '%s'.", argv[0]);
            print_probe_help();
            return false;
        }

        static void ensure_input_log() noexcept {
            if(input_log || !trace_enabled()) {
                return;
            }
            input_log = std::fopen("chimera_d3d9_model_vertex_input.log", "w");
            if(input_log) {
                std::fprintf(input_log, "# D3D9 compact suspect-pass trace. Observational unless live probe disables a pass.\n");
                std::fprintf(input_log, "# SIG_DEF first structural signature; SIG_HIT heartbeat; RANGE_BAD invalid declared vertex range.\n");
                std::fflush(input_log);
            }
        }

        static const char *classify_vertex_shader(IDirect3DVertexShader9 *shader) noexcept {
            if(!shader) return nullptr;

            if(shader == D3D9ModelShaderPrimaryV2::model_shader) return "PRIMARY_VS2_MODEL";
            if(shader == D3D9ModelShaderPrimaryV2::fogged_shader) return "PRIMARY_VS2_FOGGED";
            if(shader == D3D9ModelShaderPrimaryV2::active_camo_shader) return "PRIMARY_VS2_ACTIVE_CAMO";
            if(shader == D3D9ModelShaderPrimaryV2::shadow_shader) return "PRIMARY_VS2_SHADOW";
            if(!vertex_shaders) return nullptr;

            struct Candidate { VertexShaderIndex index; const char *name; };
            static constexpr Candidate candidates[] = {
                {VSH_MODEL_FAST, "VSH_MODEL_FAST_STOCK"},
                {VSH_MODEL_SCENERY, "VSH_MODEL_SCENERY_STOCK"},
                {VSH_MODEL_FF, "VSH_MODEL_FF_STOCK"},
                {VSH_MODEL_ACTIVE_CAMOUFLAGE_FF, "VSH_MODEL_ACTIVE_CAMOUFLAGE_FF_STOCK"},
                {VSH_MODEL_FOG_SCREEN, "VSH_MODEL_FOG_SCREEN_STOCK"},
                {VSH_MODEL_ZBUFFER, "VSH_MODEL_ZBUFFER_STOCK"},
                {VSH_EFFECT, "VSH_EFFECT"},
                {VSH_EFFECT_MULTITEXTURE, "VSH_EFFECT_MULTITEXTURE"},
                {VSH_EFFECT_MULTITEXTURE_SCREENSPACE, "VSH_EFFECT_MULTITEXTURE_SCREENSPACE"},
                {VSH_EFFECT_ZSPRITE, "VSH_EFFECT_ZSPRITE"},
                {VSH_TRANSPARENT_GENERIC, "VSH_TRANSPARENT_GENERIC"},
                {VSH_TRANSPARENT_GENERIC_LIT_M, "VSH_TRANSPARENT_GENERIC_LIT_M"},
                {VSH_TRANSPARENT_GENERIC_M, "VSH_TRANSPARENT_GENERIC_M"},
                {VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED, "VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED"},
                {VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED_M, "VSH_TRANSPARENT_GENERIC_OBJECT_CENTERED_M"},
                {VSH_TRANSPARENT_GENERIC_REFLECTION, "VSH_TRANSPARENT_GENERIC_REFLECTION"},
                {VSH_TRANSPARENT_GENERIC_REFLECTION_M, "VSH_TRANSPARENT_GENERIC_REFLECTION_M"},
                {VSH_TRANSPARENT_GENERIC_SCREENSPACE, "VSH_TRANSPARENT_GENERIC_SCREENSPACE"},
                {VSH_TRANSPARENT_GENERIC_SCREENSPACE_M, "VSH_TRANSPARENT_GENERIC_SCREENSPACE_M"},
                {VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED, "VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED"},
                {VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M, "VSH_TRANSPARENT_GENERIC_VIEWER_CENTERED_M"},
                {VSH_TRANSPARENT_GLASS_DIFFUSE_LIGHT, "VSH_TRANSPARENT_GLASS_DIFFUSE_LIGHT"},
                {VSH_TRANSPARENT_GLASS_DIFFUSE_LIGHT_M, "VSH_TRANSPARENT_GLASS_DIFFUSE_LIGHT_M"},
                {VSH_TRANSPARENT_GLASS_REFLECTION_BUMPED, "VSH_TRANSPARENT_GLASS_REFLECTION_BUMPED"},
                {VSH_TRANSPARENT_GLASS_REFLECTION_BUMPED_M, "VSH_TRANSPARENT_GLASS_REFLECTION_BUMPED_M"},
                {VSH_TRANSPARENT_GLASS_REFLECTION_FLAT, "VSH_TRANSPARENT_GLASS_REFLECTION_FLAT"},
                {VSH_TRANSPARENT_GLASS_REFLECTION_FLAT_M, "VSH_TRANSPARENT_GLASS_REFLECTION_FLAT_M"},
                {VSH_TRANSPARENT_GLASS_REFLECTION_MIRROR, "VSH_TRANSPARENT_GLASS_REFLECTION_MIRROR"},
                {VSH_TRANSPARENT_GLASS_TINT, "VSH_TRANSPARENT_GLASS_TINT"},
                {VSH_TRANSPARENT_GLASS_TINT_M, "VSH_TRANSPARENT_GLASS_TINT_M"},
                {VSH_TRANSPARENT_METER, "VSH_TRANSPARENT_METER"},
                {VSH_TRANSPARENT_METER_M, "VSH_TRANSPARENT_METER_M"},
                {VSH_TRANSPARENT_PLASMA_M, "VSH_TRANSPARENT_PLASMA_M"}
            };
            for(const auto &candidate : candidates) {
                if(vertex_shaders[candidate.index].shader && shader == vertex_shaders[candidate.index].shader) {
                    return candidate.name;
                }
            }
            return nullptr;
        }

        static UINT vertex_buffer_size(IDirect3DVertexBuffer9 *buffer) noexcept {
            if(!buffer) return 0;
            D3DVERTEXBUFFER_DESC desc = {};
            return SUCCEEDED(buffer->GetDesc(&desc)) ? desc.Size : 0;
        }

        static void index_buffer_description(IDirect3DIndexBuffer9 *buffer, UINT &size, D3DFORMAT &format) noexcept {
            size = 0;
            format = D3DFMT_UNKNOWN;
            if(!buffer) return;
            D3DINDEXBUFFER_DESC desc = {};
            if(SUCCEEDED(buffer->GetDesc(&desc))) {
                size = desc.Size;
                format = desc.Format;
            }
        }

        static UINT primitive_vertex_count(D3DPRIMITIVETYPE type, UINT primitive_count) noexcept {
            switch(type) {
                case D3DPT_POINTLIST: return primitive_count;
                case D3DPT_LINELIST: return primitive_count * 2U;
                case D3DPT_LINESTRIP: return primitive_count + 1U;
                case D3DPT_TRIANGLELIST: return primitive_count * 3U;
                case D3DPT_TRIANGLESTRIP:
                case D3DPT_TRIANGLEFAN: return primitive_count + 2U;
                default: return 0U;
            }
        }

        static void hash_bytes(std::uint64_t &hash, const void *data, std::size_t size) noexcept {
            const auto *bytes = static_cast<const unsigned char *>(data);
            for(std::size_t i = 0; i < size; i++) {
                hash ^= static_cast<std::uint64_t>(bytes[i]);
                hash *= 1099511628211ULL;
            }
        }

        template<typename T>
        static void hash_value(std::uint64_t &hash, const T &value) noexcept {
            hash_bytes(hash, &value, sizeof(value));
        }

        static void hash_string(std::uint64_t &hash, const char *value) noexcept {
            if(value) hash_bytes(hash, value, std::strlen(value));
        }

        static SeenSignature *signature_for(std::uint64_t hash, bool &created) noexcept {
            created = false;
            for(std::size_t i = 0; i < seen_signature_count; i++) {
                if(seen_signatures[i].hash == hash) return &seen_signatures[i];
            }
            if(seen_signature_count >= MAX_SEEN_SIGNATURES) return nullptr;
            auto &entry = seen_signatures[seen_signature_count++];
            entry.hash = hash;
            entry.id = static_cast<std::uint32_t>(seen_signature_count);
            entry.last_log_frame = -1;
            entry.hits_since_log = 0;
            created = true;
            return &entry;
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
            if(!trace_enabled()) return;
            ensure_input_log();
            if(!input_log) return;

            IDirect3DVertexBuffer9 *vb0 = nullptr;
            IDirect3DVertexBuffer9 *vb1 = nullptr;
            IDirect3DIndexBuffer9 *ib = nullptr;
            IDirect3DVertexDeclaration9 *decl = nullptr;
            IDirect3DPixelShader9 *ps = nullptr;
            UINT offset0 = 0, stride0 = 0, offset1 = 0, stride1 = 0;
            DWORD fvf = 0, alpha = 0, src = 0, dst = 0, zwrite = 0, cull = 0;

            device->GetStreamSource(0, &vb0, &offset0, &stride0);
            device->GetStreamSource(1, &vb1, &offset1, &stride1);
            if(indexed) device->GetIndices(&ib);
            device->GetVertexDeclaration(&decl);
            device->GetPixelShader(&ps);
            device->GetFVF(&fvf);
            device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha);
            device->GetRenderState(D3DRS_SRCBLEND, &src);
            device->GetRenderState(D3DRS_DESTBLEND, &dst);
            device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
            device->GetRenderState(D3DRS_CULLMODE, &cull);

            const UINT vb0_size = vertex_buffer_size(vb0);
            const UINT vb1_size = vertex_buffer_size(vb1);
            const unsigned long long vb0_cap = stride0 && vb0_size >= offset0
                ? static_cast<unsigned long long>((vb0_size - offset0) / stride0) : 0ULL;
            UINT ib_size = 0;
            D3DFORMAT ib_fmt = D3DFMT_UNKNOWN;
            index_buffer_description(ib, ib_size, ib_fmt);

            long long first = indexed
                ? static_cast<long long>(base_vertex_index) + min_vertex_index
                : static_cast<long long>(start_vertex);
            long long last = num_vertices ? first + num_vertices - 1LL : first;
            const bool range_ok = vb0_cap && first >= 0 && last >= first
                && static_cast<unsigned long long>(last) < vb0_cap;

            // Structural signature: intentionally excludes VB/IB/PS pointer identity so
            // dynamic effect buffers do not flood the compact table every frame.
            std::uint64_t hash = 1469598103934665603ULL;
            hash_string(hash, kind);
            hash_string(hash, pass_name);
            hash_value(hash, primitive_type);
            hash_value(hash, offset0);
            hash_value(hash, stride0);
            hash_value(hash, vb0_size);
            hash_value(hash, offset1);
            hash_value(hash, stride1);
            hash_value(hash, vb1_size);
            hash_value(hash, ib_size);
            hash_value(hash, ib_fmt);
            hash_value(hash, fvf);
            hash_value(hash, base_vertex_index);
            hash_value(hash, min_vertex_index);
            hash_value(hash, num_vertices);
            hash_value(hash, start_index);
            hash_value(hash, start_vertex);
            hash_value(hash, primitive_count);
            hash_value(hash, alpha);
            hash_value(hash, src);
            hash_value(hash, dst);
            hash_value(hash, zwrite);
            hash_value(hash, cull);

            bool created = false;
            SeenSignature *signature = signature_for(hash, created);
            const long long frame = current_frame();
            const unsigned long long draw = ++draw_counter;
            if(signature) {
                signature->hits_since_log++;
                const bool heartbeat = !created && frame >= 0 && signature->last_log_frame >= 0
                    && frame - signature->last_log_frame >= HEARTBEAT_FRAME_INTERVAL;
                if(created || !range_ok) {
                    std::fprintf(
                        input_log,
                        "%s id=%lu frame=%lld draw=%llu kind=%s pass=%s prim=%u VB0=%p OFFSET0=%u STRIDE0=%u VB0_SIZE=%u VB0_CAP=%llu VB1=%p OFFSET1=%u STRIDE1=%u VB1_SIZE=%u IB=%p IB_SIZE=%u IB_FMT=%u DECL=%p FVF=0x%08lX BASE_VERTEX=%d MIN_VERTEX=%u NUM_VERTICES=%u START_INDEX=%u START_VERTEX=%u PRIMITIVE_COUNT=%u RANGE_FIRST=%lld RANGE_LAST=%lld RANGE_OK=%u ALPHABLEND=%lu SRCBLEND=%lu DESTBLEND=%lu ZWRITE=%lu CULLMODE=%lu\n",
                        range_ok ? "SIG_DEF" : "RANGE_BAD",
                        static_cast<unsigned long>(signature->id), frame, draw, kind, pass_name,
                        static_cast<unsigned>(primitive_type), static_cast<void *>(vb0), offset0, stride0,
                        vb0_size, vb0_cap, static_cast<void *>(vb1), offset1, stride1, vb1_size,
                        static_cast<void *>(ib), ib_size, static_cast<unsigned>(ib_fmt), static_cast<void *>(decl),
                        static_cast<unsigned long>(fvf), base_vertex_index, min_vertex_index, num_vertices,
                        start_index, start_vertex, primitive_count, first, last, range_ok ? 1U : 0U,
                        static_cast<unsigned long>(alpha), static_cast<unsigned long>(src),
                        static_cast<unsigned long>(dst), static_cast<unsigned long>(zwrite),
                        static_cast<unsigned long>(cull)
                    );
                    signature->last_log_frame = frame;
                    signature->hits_since_log = 0;
                }
                else if(heartbeat) {
                    std::fprintf(
                        input_log,
                        "SIG_HIT id=%lu frame=%lld draws=%llu pass=%s\n",
                        static_cast<unsigned long>(signature->id), frame,
                        signature->hits_since_log, pass_name
                    );
                    signature->last_log_frame = frame;
                    signature->hits_since_log = 0;
                }
                if((++log_flush_counter & 63u) == 0u) std::fflush(input_log);
            }

            if(ps) ps->Release();
            if(decl) decl->Release();
            if(ib) ib->Release();
            if(vb1) vb1->Release();
            if(vb0) vb0->Release();
        }

        static const char *current_pass(IDirect3DDevice9 *device) noexcept {
            IDirect3DVertexShader9 *vs = nullptr;
            if(!device || FAILED(device->GetVertexShader(&vs)) || !vs) return nullptr;
            const char *pass = classify_vertex_shader(vs);
            vs->Release();
            return pass;
        }

        static HRESULT STDMETHODCALLTYPE draw_primitive_hook(
            IDirect3DDevice9 *device,
            D3DPRIMITIVETYPE primitive_type,
            UINT start_vertex,
            UINT primitive_count
        ) noexcept {
            if(!original_draw_primitive) return D3DERR_INVALIDCALL;
            if(installed_device == device) {
                const char *pass = current_pass(device);
                if(pass) {
                    const bool skip = should_skip_pass(pass);
                    record_probe_activity(pass, skip);
                    if(trace_enabled()) {
                        log_draw(device, "DP", pass, primitive_type, 0, 0,
                            primitive_vertex_count(primitive_type, primitive_count), 0,
                            start_vertex, primitive_count, false);
                    }
                    if(skip) return D3D_OK;
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
            if(!original_draw_indexed_primitive) return D3DERR_INVALIDCALL;
            if(installed_device == device) {
                const char *pass = current_pass(device);
                if(pass) {
                    const bool skip = should_skip_pass(pass);
                    record_probe_activity(pass, skip);
                    if(trace_enabled()) {
                        log_draw(device, "DIP", pass, primitive_type, base_vertex_index,
                            min_vertex_index, num_vertices, start_index, 0, primitive_count, true);
                    }
                    if(skip) return D3D_OK;
                }
            }
            return original_draw_indexed_primitive(
                device, primitive_type, base_vertex_index, min_vertex_index,
                num_vertices, start_index, primitive_count
            );
        }

        static bool patch_vtable_entry(ULONG_PTR *entry, ULONG_PTR replacement, ULONG_PTR &original) noexcept {
            if(!entry) return false;
            if(*entry == replacement) return original != 0;
            original = *entry;
            if(!original) return false;
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
            if(!device || !d3d9on12_requested()) return false;
            auto *vtable = *reinterpret_cast<ULONG_PTR **>(device);
            if(!vtable) return false;

            ULONG_PTR original_dp = reinterpret_cast<ULONG_PTR>(original_draw_primitive);
            ULONG_PTR original_dip = reinterpret_cast<ULONG_PTR>(original_draw_indexed_primitive);
            const bool dp_ok = patch_vtable_entry(
                &vtable[DEVICE_DRAW_PRIMITIVE], reinterpret_cast<ULONG_PTR>(draw_primitive_hook), original_dp);
            const bool dip_ok = patch_vtable_entry(
                &vtable[DEVICE_DRAW_INDEXED_PRIMITIVE], reinterpret_cast<ULONG_PTR>(draw_indexed_primitive_hook), original_dip);
            original_draw_primitive = reinterpret_cast<DrawPrimitiveFunction>(original_dp);
            original_draw_indexed_primitive = reinterpret_cast<DrawIndexedPrimitiveFunction>(original_dip);
            if(!dp_ok && !dip_ok) return false;

            installed_device = device;
            ensure_input_log();
            if(!installed_announced) {
                console_output("D3D9On12 live probe ready: chimera_d3d9_probe");
                if(trace_enabled()) console_output("D3D9 compact trace -> chimera_d3d9_model_vertex_input.log");
                installed_announced = true;
            }
            return true;
        }

        static void on_end_scene(IDirect3DDevice9 *device) noexcept {
            if(d3d9on12_requested()) install(device);
        }

        static void set_up() noexcept {
            if(!d3d9on12_requested()) return;
            if(!queued_announced) {
                console_output("D3D9On12 live draw probe requested; waiting for D3D9 device.");
                queued_announced = true;
            }
            if(!end_scene_retry_registered) {
                add_d3d9_end_scene_event(on_end_scene);
                end_scene_retry_registered = true;
            }
            if(global_d3d9_device && *global_d3d9_device) install(*global_d3d9_device);
        }
    }

    inline void set_up_d3d9_model_vertex_input_diag() noexcept {
        D3D9ModelVertexInputDiag::set_up();
    }
}

#endif
