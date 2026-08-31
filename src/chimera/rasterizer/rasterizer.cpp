// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <unordered_map>

#include "rasterizer.hpp"
#include "rasterizer_transparent_geometry.hpp"
#include "enhanced_graphics.hpp"
#include "smaa.hpp"
#include "graphics_diagnostics.hpp"
#include "graphics_runtime_metrics.hpp"
#include "graphics_visual_regression.hpp"
#include "../chimera.hpp"
#include "../signature/hook.hpp"
#include "../halo_data/game_engine.hpp"
#include "../halo_data/game_variables.hpp"
#include "../halo_data/shaders/shader_blob.hpp"
#include "../output/error_box.hpp"
#include "../event/frame.hpp"
#include "../event/game_loop.hpp"


namespace Chimera {

    IDirect3DDevice9 **global_d3d9_device = nullptr;
    D3DCAPS9 *d3d9_device_caps = nullptr;
    bool chimera_rasterizer_enabled = false;

    IDirect3DPixelShader9 *chimera_pixel_shaders[NUMBER_OF_CHIMERA_PIXEL_SHADERS] = { nullptr };

    static bool graphics_requests_smaa() noexcept {
        const auto *ini = get_chimera().get_ini();
        if(!ini) {
            return false;
        }
        const char *anti_aliasing = ini->get_value("graphics.anti_aliasing");
        return anti_aliasing && (
            std::strcmp(anti_aliasing, "smaa") == 0 ||
            std::strcmp(anti_aliasing, "SMAA") == 0 ||
            std::strcmp(anti_aliasing, "smaa_t2x") == 0 ||
            std::strcmp(anti_aliasing, "SMAA_T2X") == 0
        );
    }

    static bool graphics_requests_pre_hud_aa() noexcept {
        const auto *ini = get_chimera().get_ini();
        if(!ini || !ini->get_value_bool("graphics.enabled").value_or(false) ||
           !ini->get_value_bool("graphics.smaa_exclude_hud").value_or(true)) {
            return false;
        }

        const char *anti_aliasing = ini->get_value("graphics.anti_aliasing");
        if(!anti_aliasing) {
            return false;
        }

        return std::strcmp(anti_aliasing, "fxaa") == 0 || std::strcmp(anti_aliasing, "FXAA") == 0 ||
               std::strcmp(anti_aliasing, "smaa") == 0 || std::strcmp(anti_aliasing, "SMAA") == 0 ||
               std::strcmp(anti_aliasing, "smaa_t2x") == 0 || std::strcmp(anti_aliasing, "SMAA_T2X") == 0;
    }

    static bool retail_pre_hud_active = false;
    static bool retail_pre_hud_smaa_active = false;

    static bool get_executable_code_range(std::byte *&begin, std::byte *&end) noexcept {
        begin = nullptr;
        end = nullptr;

        auto *module = reinterpret_cast<std::byte *>(GetModuleHandle(nullptr));
        if(!module) {
            return false;
        }

        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
        if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
            return false;
        }
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(module + dos->e_lfanew);
        if(nt->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }

        const auto *section = IMAGE_FIRST_SECTION(nt);
        for(unsigned int i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
            if((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
               (section->Characteristics & IMAGE_SCN_CNT_CODE) == 0 ||
               section->Misc.VirtualSize < 5) {
                continue;
            }

            const auto start_rva = static_cast<std::uintptr_t>(section->VirtualAddress);
            const auto size = static_cast<std::uintptr_t>(section->Misc.VirtualSize);
            if(start_rva >= nt->OptionalHeader.SizeOfImage ||
               size > nt->OptionalHeader.SizeOfImage - start_rva) {
                continue;
            }

            begin = module + start_rva;
            end = begin + size;
            return true;
        }
        return false;
    }

    static std::byte *relative_call_target(
        std::byte *site,
        std::byte *code_begin,
        std::byte *code_end
    ) noexcept {
        if(!site || !code_begin || !code_end || site < code_begin || site >= code_end ||
           static_cast<std::size_t>(code_end - site) < 5 ||
           *reinterpret_cast<const std::uint8_t *>(site) != 0xE8) {
            return nullptr;
        }

        std::int32_t displacement = 0;
        std::memcpy(&displacement, site + 1, sizeof(displacement));

        const auto next_address = reinterpret_cast<std::uintptr_t>(site) + 5U;
        const auto target_address =
            static_cast<std::int64_t>(next_address) + static_cast<std::int64_t>(displacement);
        const auto code_begin_address =
            static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(code_begin));
        const auto code_end_address =
            static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(code_end));
        if(target_address < code_begin_address || target_address >= code_end_address) {
            return nullptr;
        }

        return reinterpret_cast<std::byte *>(static_cast<std::uintptr_t>(target_address));
    }

    static std::byte *validated_retail_pre_hud_call_site() noexcept {
        // Keep the Retail discovery path completely separate from Custom Edition. CE has
        // its own validated 1.10 call-site path in enhanced_graphics.hpp and must not be
        // affected by Retail executable-layout differences.
        if(game_engine() != GameEngine::GAME_ENGINE_RETAIL) {
            return nullptr;
        }

        auto *screen_effect = get_chimera().get_signature("widescreen_screen_effect_sig").data();
        if(!screen_effect) {
            return nullptr;
        }

        std::byte *code_begin = nullptr;
        std::byte *code_end = nullptr;
        if(!get_executable_code_range(code_begin, code_end)) {
            return nullptr;
        }

        if(screen_effect < code_begin || screen_effect >= code_end) {
            return nullptr;
        }

        auto find_unique_candidate = [=](bool require_null_argument, bool &ambiguous) noexcept -> std::byte * {
            ambiguous = false;
            std::byte *candidate = nullptr;

            for(auto *site = code_begin; static_cast<std::size_t>(code_end - site) >= 5; site++) {
                if(relative_call_target(site, code_begin, code_end) != screen_effect) {
                    continue;
                }

                // The original Retail pattern used the no-effect form `push 0`. Preserve
                // that as the strict first choice, but allow other valid argument-producing
                // forms in a second pass because Retail executables/mods can express the
                // same call flow differently.
                if(require_null_argument) {
                    if(site - code_begin < 2) {
                        continue;
                    }
                    const auto *before = reinterpret_cast<const std::uint8_t *>(site - 2);
                    if(before[0] != 0x6A || before[1] != 0x00) {
                        continue;
                    }
                }

                // Retail builds can use different x86 stack-cleanup spellings.
                // Start immediately after the call and skip any recognized one-argument
                // cleanup sequence instead of requiring one exact encoding.
                auto *after_screen_effect = site + 5;
                if(static_cast<std::size_t>(code_end - after_screen_effect) >= 3) {
                    const auto *after = reinterpret_cast<const std::uint8_t *>(after_screen_effect);
                    if(after[0] == 0x83 && after[1] == 0xC4 && after[2] == 0x04) {
                        after_screen_effect += 3;
                    }
                    else if(after[0] == 0x59) {
                        after_screen_effect += 1;
                    }
                    else if(static_cast<std::size_t>(code_end - after_screen_effect) >= 4 &&
                            after[0] == 0x8D && after[1] == 0x64 &&
                            after[2] == 0x24 && after[3] == 0x04) {
                        after_screen_effect += 4;
                    }
                }

                // At the interface-draw boundary the first two following calls are the HUD
                // draw and game-engine post-rasterize stages. Validate their spacing and
                // require different targets so an arbitrary call sequence cannot be hooked.
                std::byte *first_call = nullptr;
                std::byte *second_call = nullptr;
                auto *cursor = after_screen_effect;
                const auto remaining = static_cast<std::size_t>(code_end - cursor);
                auto *scan_end = cursor + (remaining > 0x60U ? 0x60U : remaining);

                while(static_cast<std::size_t>(scan_end - cursor) >= 5) {
                    auto *target = relative_call_target(cursor, code_begin, code_end);
                    if(target && target != screen_effect) {
                        if(!first_call) {
                            first_call = cursor;
                        }
                        else {
                            second_call = cursor;
                            break;
                        }
                    }
                    cursor++;
                }

                if(!first_call || !second_call ||
                   first_call - after_screen_effect > 0x30 ||
                   second_call - first_call > 0x30) {
                    continue;
                }

                auto *first_target = relative_call_target(first_call, code_begin, code_end);
                auto *second_target = relative_call_target(second_call, code_begin, code_end);
                if(!first_target || !second_target || first_target == second_target) {
                    continue;
                }

                // Multiple screen-effect paths are acceptable only when they converge on
                // the exact same HUD call site. Distinct candidates are ambiguous and fail
                // closed instead of guessing an executable address.
                if(candidate && candidate != first_call) {
                    ambiguous = true;
                    return nullptr;
                }
                candidate = first_call;
            }

            return candidate;
        };

        bool ambiguous = false;
        if(auto *strict_candidate = find_unique_candidate(true, ambiguous)) {
            return strict_candidate;
        }
        if(ambiguous) {
            return nullptr;
        }

        // Retail-compatible semantic fallback: same validated call flow, without assuming
        // that the NULL screen-effect argument is encoded specifically as `push 0`.
        auto *compatible_candidate = find_unique_candidate(false, ambiguous);
        if(ambiguous) {
            return nullptr;
        }
        return compatible_candidate;
    }

    static bool install_retail_pre_hud_hook(const void *callback) noexcept {
        static Hook hook;
        static const void *installed_callback = nullptr;

        if(hook.address && hook.hook && !hook.original_bytes.empty()) {
            return installed_callback == callback;
        }
        if(!callback) {
            return false;
        }

        auto *call_site = validated_retail_pre_hud_call_site();
        if(!call_site) {
            return false;
        }

        write_jmp_call(call_site, hook, callback);
        const bool installed =
            hook.address == call_site && hook.hook && !hook.original_bytes.empty();
        if(installed) {
            installed_callback = callback;
        }
        return installed;
    }

    static bool set_up_retail_pre_hud_graphics() noexcept {
        if(game_engine() != GameEngine::GAME_ENGINE_RETAIL || !graphics_requests_pre_hud_aa()) {
            return false;
        }

        const bool smaa = graphics_requests_smaa();
        if(smaa && (!d3d9_device_caps || d3d9_device_caps->PixelShaderVersion < 0xffff0300)) {
            return false;
        }

        auto &graphics = EnhancedGraphics::state();
        graphics.settings = EnhancedGraphics::load_settings();
        if(!graphics.settings.enabled) {
            return false;
        }

        // Official SMAA 1x/T2x reuse the proven pre-HUD routing while keeping
        // EnhancedGraphics' shared frame resources in strict mode.
        graphics.settings.smaa_exclude_hud = true;
        if(smaa) {
            graphics.settings.fxaa = true;
        }

        const void *callback = smaa
            ? reinterpret_cast<const void *>(SMAA::on_pre_hud)
            : reinterpret_cast<const void *>(EnhancedGraphics::on_pre_hud);
        if(!install_retail_pre_hud_hook(callback)) {
            return false;
        }

        add_d3d9_reset_event(EnhancedGraphics::on_reset, EVENT_PRIORITY_BEFORE);
        add_game_exit_event(EnhancedGraphics::release_resources, EVENT_PRIORITY_BEFORE);

        if(smaa) {
            add_d3d9_reset_event(SMAA::on_reset, EVENT_PRIORITY_BEFORE);
            add_game_exit_event(SMAA::release_resources, EVENT_PRIORITY_BEFORE);
        }

        retail_pre_hud_active = true;
        retail_pre_hud_smaa_active = smaa;
        return true;
    }

    static void set_up_enhanced_graphics_for_current_engine() noexcept {
        if(game_engine() != GameEngine::GAME_ENGINE_RETAIL || !graphics_requests_pre_hud_aa()) {
            set_up_enhanced_graphics();
            return;
        }

        if(set_up_retail_pre_hud_graphics()) {
            return;
        }

        // A modified/unsupported Retail executable must never receive a guessed code hook.
        // Preserve the already-tested post-EndScene compatibility path as the fail-closed
        // fallback when the Retail pre-HUD boundary cannot be validated uniquely.
        auto &graphics = EnhancedGraphics::state();
        graphics.settings = EnhancedGraphics::load_settings();
        if(!graphics.settings.enabled) {
            return;
        }

        graphics.settings.smaa_exclude_hud = false;
        add_d3d9_end_scene_after_event(EnhancedGraphics::on_end_scene_after, EVENT_PRIORITY_AFTER);
        add_d3d9_reset_event(EnhancedGraphics::on_reset, EVENT_PRIORITY_BEFORE);
        add_game_exit_event(EnhancedGraphics::release_resources, EVENT_PRIORITY_BEFORE);

        console_error(
            "Chimera Graphics: Halo PC Retail pre-HUD validation was unavailable; "
            "using the safe full-frame compatibility path."
        );
    }

    struct BloomSettings {
        bool enabled = false;
        bool multiscale = false;
        float threshold = 0.80f;
        float intensity = 0.18f;
        float radius = 3.0f;
    };

    struct PostEffectsSettings {
        bool adaptive_sharpening = false;
        bool sharpening_anti_halo = false;
        bool debanding = false;
        float debanding_strength = 0.35f;
        float debanding_threshold = 0.025f;
        bool dithering = false;
        float dithering_strength = 1.00f;
    };

    static float clamp_graphics_value(double value, float minimum, float maximum) noexcept {
        const float converted = static_cast<float>(value);
        return converted < minimum ? minimum : (converted > maximum ? maximum : converted);
    }

    static BloomSettings graphics_bloom_settings() noexcept {
        BloomSettings bloom;
        const auto *ini = get_chimera().get_ini();
        if(!ini) {
            return bloom;
        }

        bloom.enabled = ini->get_value_bool("graphics.bloom").value_or(false);
        bloom.multiscale = ini->get_value_bool("graphics.bloom_multiscale").value_or(false);
        bloom.threshold = clamp_graphics_value(
            ini->get_value_float("graphics.bloom_threshold").value_or(0.80),
            0.0f,
            1.0f
        );
        bloom.intensity = clamp_graphics_value(
            ini->get_value_float("graphics.bloom_intensity").value_or(0.18),
            0.0f,
            1.5f
        );
        bloom.radius = clamp_graphics_value(
            ini->get_value_float("graphics.bloom_radius").value_or(3.0),
            0.5f,
            8.0f
        );
        return bloom;
    }

    static PostEffectsSettings graphics_post_effects_settings() noexcept {
        PostEffectsSettings effects;
        const auto *ini = get_chimera().get_ini();
        if(!ini) {
            return effects;
        }

        effects.adaptive_sharpening = ini->get_value_bool("graphics.adaptive_sharpening").value_or(false);
        effects.sharpening_anti_halo = ini->get_value_bool("graphics.sharpening_anti_halo").value_or(false);
        effects.debanding = ini->get_value_bool("graphics.debanding").value_or(false);
        effects.debanding_strength = clamp_graphics_value(
            ini->get_value_float("graphics.debanding_strength").value_or(0.35),
            0.0f,
            1.0f
        );
        effects.debanding_threshold = clamp_graphics_value(
            ini->get_value_float("graphics.debanding_threshold").value_or(0.025),
            0.005f,
            0.10f
        );
        effects.dithering = ini->get_value_bool("graphics.dithering").value_or(false);
        effects.dithering_strength = clamp_graphics_value(
            ini->get_value_float("graphics.dithering_strength").value_or(1.00),
            0.0f,
            2.0f
        );
        return effects;
    }

    void rasterizer_set_render_state(D3DRENDERSTATETYPE state, DWORD value) noexcept {
        throw_error(global_d3d9_device, "d3d device missing");
        IDirect3DDevice9_SetRenderState(*global_d3d9_device, state, value);
    }

    void rasterizer_set_sampler_state(std::uint16_t sampler, D3DSAMPLERSTATETYPE type, DWORD value) noexcept {
        throw_error(global_d3d9_device, "d3d device missing");
        IDirect3DDevice9_SetSamplerState(*global_d3d9_device, sampler, type, value);
    }

    void rasterizer_create_pixel_shaders() noexcept {
        throw_error(global_d3d9_device, "d3d device missing");
        for(int i = 0; i < NUMBER_OF_CHIMERA_PIXEL_SHADERS; i++) {
            throw_error(!chimera_pixel_shaders[i], "Something went horribly wrong");
        }

        auto create_pixel_shader = [](const void *shader_bytecode, IDirect3DPixelShader9 **shader) noexcept {
            if(!shader) {
                return false;
            }
            *shader = nullptr;
            if(!shader_bytecode || !global_d3d9_device || !*global_d3d9_device) {
                return false;
            }
            const HRESULT result = IDirect3DDevice9_CreatePixelShader(
                *global_d3d9_device,
                reinterpret_cast<const DWORD *>(shader_bytecode),
                shader
            );
            if(FAILED(result) || !*shader) {
                *shader = nullptr;
                return false;
            }
            return true;
        };

        create_pixel_shader(white_1_1, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_WHITE_1_1]);

        // Ensure ps2.0 support for all except the ps_1_1 shader.
        if(!d3d9_device_caps || d3d9_device_caps->PixelShaderVersion < 0xffff0200) {
            return;
        }

        create_pixel_shader(white, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_WHITE]);
        create_pixel_shader(black, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_BLACK]);
        create_pixel_shader(hud_meters, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_HUD_METERS]);
        create_pixel_shader(fog, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_FOG]);
        create_pixel_shader(fog_akill, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_FOG_ALPHA_KILL]);
        create_pixel_shader(fog_screen, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_FOG_SCREEN]);
        create_pixel_shader(eff_nlin_tint_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_Z]);
        create_pixel_shader(eff_nlin_tint_add_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_ADD_Z]);
        create_pixel_shader(eff_nlin_tint_alpha_blend_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_ALPHA_BLEND_Z]);
        create_pixel_shader(eff_nlin_tint_double_mul_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_DOUBLE_MUL_Z]);
        create_pixel_shader(eff_nlin_tint_mul_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_MUL_Z]);
        create_pixel_shader(eff_nlin_tint_mul_add_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NLIN_TINT_MUL_ADD_Z]);
        create_pixel_shader(eff_normal_tint_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_Z]);
        create_pixel_shader(eff_normal_tint_add_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_ADD_Z]);
        create_pixel_shader(eff_normal_tint_alpha_blend_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_ALPHA_BLEND_Z]);
        create_pixel_shader(eff_normal_tint_double_mul_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_DOUBLE_MUL_Z]);
        create_pixel_shader(eff_normal_tint_mul_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_MUL_Z]);
        create_pixel_shader(eff_normal_tint_mul_add_z, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_EFF_NORMAL_TINT_MUL_ADD_Z]);
        create_pixel_shader(decal_add, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ADD]);
        create_pixel_shader(decal_multiply, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_MULTIPLY]);
        create_pixel_shader(decal_multiply2x, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_MULTIPLY2X]);
        create_pixel_shader(decal_alpha_blend, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ALPHA_BLEND]);
        create_pixel_shader(decal_alpha_madd, &chimera_pixel_shaders[CHIMERA_PIXEL_SHADER_DECAL_ALPHA_MULTIPLY_ADD]);
    }

    void rasterizer_release_pixel_shaders() noexcept {
        for(int i = 0; i < NUMBER_OF_CHIMERA_PIXEL_SHADERS; i++) {
            if(chimera_pixel_shaders[i]) {
                IDirect3DPixelShader9_Release(chimera_pixel_shaders[i]);
                chimera_pixel_shaders[i] = nullptr;
            }
        }
    }

    static std::string prepare_runtime_shader_source(const char *source) {
        if(!source) {
            return {};
        }

        std::string prepared(source);

        // Runtime graphics shaders that expose this declaration receive Chimera's
        // current color settings. No separate probe variable is needed here.
        constexpr const char *COLOR_OPTIONS_DECLARATION = "float4 color_options : register(c1);";
        const auto position = prepared.find(COLOR_OPTIONS_DECLARATION);
        if(position == std::string::npos) {
            return prepared;
        }

        const auto &settings = EnhancedGraphics::state().settings;
        std::ostringstream replacement;
        replacement.imbue(std::locale::classic());
        replacement << std::fixed << std::setprecision(6)
                    << "static const float4 color_options = float4("
                    << settings.brightness << ", "
                    << settings.contrast << ", "
                    << settings.saturation << ", "
                    << (settings.color_correction ? 1.0f : 0.0f)
                    << ");";

        prepared.replace(position, std::strlen(COLOR_OPTIONS_DECLARATION), replacement.str());

        const auto post_effects = graphics_post_effects_settings();

        // Adaptive sharpening keeps the existing sharpening strength control but scales
        // its gain by local contrast. Flat regions are left alone and very strong edges
        // are protected to reduce halos and temporal shimmer.
        if(post_effects.adaptive_sharpening) {
            constexpr const char *SHARPEN_GAIN_DECLARATION =
                "const float sharpen_gain = saturate(frame_options.w) * 2.5;";
            const auto sharpen_position = prepared.find(SHARPEN_GAIN_DECLARATION);
            if(sharpen_position != std::string::npos) {
                const char *range_name = prepared.find("const float luma_range =") != std::string::npos
                    ? "luma_range"
                    : "local_range";

                std::ostringstream adaptive_source;
                adaptive_source.imbue(std::locale::classic());
                adaptive_source
                    << "const float base_sharpen_gain = saturate(frame_options.w) * 2.5;\n"
                    << "                const float adaptive_detail = saturate((" << range_name
                    << " - 0.008) * 19.230769);\n"
                    << "                const float adaptive_edge_protection = 1.0 - saturate((" << range_name
                    << " - 0.20) * 2.857143);\n"
                    << "                const float sharpen_gain = base_sharpen_gain * adaptive_detail * adaptive_edge_protection;";

                prepared.replace(
                    sharpen_position,
                    std::strlen(SHARPEN_GAIN_DECLARATION),
                    adaptive_source.str()
                );
            }
        }

        // Anti-halo only trims the highest-amplitude sharpening contribution on real
        // local edges. The cap scales with the active sharpening gain, so conservative
        // strengths such as 0.12 are protected too instead of falling below a fixed
        // threshold. Flat/low-contrast texture detail remains unchanged.
        if(post_effects.sharpening_anti_halo) {
            constexpr const char *SHARPEN_APPLY =
                "color = saturate(color + sharp_detail * sharpen_gain);";
            const auto sharpen_apply_position = prepared.find(SHARPEN_APPLY);
            if(sharpen_apply_position != std::string::npos) {
                const char *range_name = nullptr;
                if(prepared.find("const float luma_range =") != std::string::npos) {
                    range_name = "luma_range";
                }
                else if(prepared.find("const float local_range =") != std::string::npos) {
                    range_name = "local_range";
                }

                if(range_name) {
                    std::ostringstream anti_halo_source;
                    anti_halo_source.imbue(std::locale::classic());
                    anti_halo_source
                        << "const float3 requested_sharpen = sharp_detail * sharpen_gain;\n"
                        << "                const float anti_halo_edge = saturate((" << range_name
                        << " - 0.045) * 10.0);\n"
                        << "                const float anti_halo_limit = detail_limit * sharpen_gain * 0.80;\n"
                        << "                const float3 clipped_sharpen = clamp(requested_sharpen, -anti_halo_limit.xxx, anti_halo_limit.xxx);\n"
                        << "                const float3 protected_sharpen = lerp(requested_sharpen, clipped_sharpen, anti_halo_edge);\n"
                        << "                color = saturate(color + protected_sharpen);";

                    prepared.replace(
                        sharpen_apply_position,
                        std::strlen(SHARPEN_APPLY),
                        anti_halo_source.str()
                    );
                }
            }
        }

        // Bloom is injected only into the final Chimera Graphics color pass. Edge and
        // weight passes do not contain this marker and remain untouched.
        const auto bloom = graphics_bloom_settings();
        constexpr const char *BLOOM_INSERTION_POINT = "const float grey = luma(color);";
        const auto bloom_position = prepared.find(BLOOM_INSERTION_POINT);
        if(bloom.enabled && bloom_position != std::string::npos &&
           d3d9_device_caps && d3d9_device_caps->PixelShaderVersion >= 0xffff0300) {
            std::ostringstream bloom_source;
            bloom_source.imbue(std::locale::classic());
            bloom_source << std::fixed << std::setprecision(6)
                         << "const float bloom_threshold = " << bloom.threshold << ";\n"
                         << "const float bloom_intensity = " << bloom.intensity << ";\n"
                         << "const float bloom_radius = " << bloom.radius << ";\n";

            if(bloom.multiscale) {
                bloom_source << R"HLSL(
            // Stable single-pass multi-scale bloom: three spatial radii provide a tight
            // core, a medium halo and a weak wide halo without extra render targets.
            const float2 bloom_near = frame_options.xy * (bloom_radius * 0.65);
            const float2 bloom_mid = frame_options.xy * (bloom_radius * 1.45);
            const float2 bloom_mid_diag = bloom_mid * 0.70710678;
            const float2 bloom_far = frame_options.xy * (bloom_radius * 2.75);

            float3 bn0 = tex2D(frame_sampler, uv + float2( bloom_near.x, 0.0)).rgb;
            float3 bn1 = tex2D(frame_sampler, uv + float2(-bloom_near.x, 0.0)).rgb;
            float3 bn2 = tex2D(frame_sampler, uv + float2(0.0,  bloom_near.y)).rgb;
            float3 bn3 = tex2D(frame_sampler, uv + float2(0.0, -bloom_near.y)).rgb;
            float wn0 = saturate((luma(bn0) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float wn1 = saturate((luma(bn1) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float wn2 = saturate((luma(bn2) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float wn3 = saturate((luma(bn3) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            const float near_weight = wn0 + wn1 + wn2 + wn3;
            const float3 near_color = (bn0 * wn0 + bn1 * wn1 + bn2 * wn2 + bn3 * wn3) / max(near_weight, 1.0);

            float3 bm0 = tex2D(frame_sampler, uv + float2( bloom_mid_diag.x,  bloom_mid_diag.y)).rgb;
            float3 bm1 = tex2D(frame_sampler, uv + float2(-bloom_mid_diag.x,  bloom_mid_diag.y)).rgb;
            float3 bm2 = tex2D(frame_sampler, uv + float2( bloom_mid_diag.x, -bloom_mid_diag.y)).rgb;
            float3 bm3 = tex2D(frame_sampler, uv + float2(-bloom_mid_diag.x, -bloom_mid_diag.y)).rgb;
            float wm0 = saturate((luma(bm0) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float wm1 = saturate((luma(bm1) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float wm2 = saturate((luma(bm2) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float wm3 = saturate((luma(bm3) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            const float mid_weight = wm0 + wm1 + wm2 + wm3;
            const float3 mid_color = (bm0 * wm0 + bm1 * wm1 + bm2 * wm2 + bm3 * wm3) / max(mid_weight, 1.0);

            float3 bf0 = tex2D(frame_sampler, uv + float2( bloom_far.x, 0.0)).rgb;
            float3 bf1 = tex2D(frame_sampler, uv + float2(-bloom_far.x, 0.0)).rgb;
            float3 bf2 = tex2D(frame_sampler, uv + float2(0.0,  bloom_far.y)).rgb;
            float3 bf3 = tex2D(frame_sampler, uv + float2(0.0, -bloom_far.y)).rgb;
            float wf0 = saturate((luma(bf0) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float wf1 = saturate((luma(bf1) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float wf2 = saturate((luma(bf2) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float wf3 = saturate((luma(bf3) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            const float far_weight = wf0 + wf1 + wf2 + wf3;
            const float3 far_color = (bf0 * wf0 + bf1 * wf1 + bf2 * wf2 + bf3 * wf3) / max(far_weight, 1.0);

            const float near_presence = saturate(near_weight * 0.35);
            const float mid_presence = saturate(mid_weight * 0.35);
            const float far_presence = saturate(far_weight * 0.35);
            const float3 bloom_color =
                near_color * near_presence * 0.52 +
                mid_color * mid_presence * 0.31 +
                far_color * far_presence * 0.17;
            color = saturate(color + bloom_color * bloom_intensity);

            )HLSL";
            }
            else {
                bloom_source << R"HLSL(
            const float2 bloom_step = frame_options.xy * bloom_radius;
            const float2 bloom_diag = bloom_step * 0.70710678;
            float3 bloom_sum = 0.0;
            float bloom_weight = 0.0;

            float3 bloom_s0 = tex2D(frame_sampler, uv + float2( bloom_step.x, 0.0)).rgb;
            float3 bloom_s1 = tex2D(frame_sampler, uv + float2(-bloom_step.x, 0.0)).rgb;
            float3 bloom_s2 = tex2D(frame_sampler, uv + float2(0.0,  bloom_step.y)).rgb;
            float3 bloom_s3 = tex2D(frame_sampler, uv + float2(0.0, -bloom_step.y)).rgb;
            float3 bloom_s4 = tex2D(frame_sampler, uv + float2( bloom_diag.x,  bloom_diag.y)).rgb;
            float3 bloom_s5 = tex2D(frame_sampler, uv + float2(-bloom_diag.x,  bloom_diag.y)).rgb;
            float3 bloom_s6 = tex2D(frame_sampler, uv + float2( bloom_diag.x, -bloom_diag.y)).rgb;
            float3 bloom_s7 = tex2D(frame_sampler, uv + float2(-bloom_diag.x, -bloom_diag.y)).rgb;

            float bloom_w0 = saturate((luma(bloom_s0) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float bloom_w1 = saturate((luma(bloom_s1) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float bloom_w2 = saturate((luma(bloom_s2) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float bloom_w3 = saturate((luma(bloom_s3) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float bloom_w4 = saturate((luma(bloom_s4) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float bloom_w5 = saturate((luma(bloom_s5) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float bloom_w6 = saturate((luma(bloom_s6) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));
            float bloom_w7 = saturate((luma(bloom_s7) - bloom_threshold) / max(1.0 - bloom_threshold, 0.001));

            bloom_sum += bloom_s0 * bloom_w0;
            bloom_sum += bloom_s1 * bloom_w1;
            bloom_sum += bloom_s2 * bloom_w2;
            bloom_sum += bloom_s3 * bloom_w3;
            bloom_sum += bloom_s4 * bloom_w4;
            bloom_sum += bloom_s5 * bloom_w5;
            bloom_sum += bloom_s6 * bloom_w6;
            bloom_sum += bloom_s7 * bloom_w7;
            bloom_weight = bloom_w0 + bloom_w1 + bloom_w2 + bloom_w3 + bloom_w4 + bloom_w5 + bloom_w6 + bloom_w7;

            const float3 bloom_color = bloom_sum / max(bloom_weight, 1.0);
            const float bloom_presence = saturate(bloom_weight * 0.25);
            color = saturate(color + bloom_color * bloom_intensity * bloom_presence);

            )HLSL";
            }
            prepared.insert(bloom_position, bloom_source.str());
        }

        // Debanding runs after color correction and before the final 8-bit dithering step.
        // It only blends low-contrast neighborhoods and therefore avoids softening real
        // geometry edges. The neighboring raw colors are graded with the same color
        // settings before they are mixed back into the current pixel.
        constexpr const char *POST_COLOR_INSERTION_POINT =
            "color = lerp(color, saturate(corrected), saturate(color_options.w));";
        const auto post_color_position = prepared.find(POST_COLOR_INSERTION_POINT);
        if(post_color_position != std::string::npos) {
            std::ostringstream final_color_source;
            final_color_source.imbue(std::locale::classic());

            if(post_effects.debanding && d3d9_device_caps &&
               d3d9_device_caps->PixelShaderVersion >= 0xffff0300) {
                final_color_source << std::fixed << std::setprecision(9)
                    << "\n\n                const float deband_strength = " << post_effects.debanding_strength << ";\n"
                    << "                const float deband_threshold = " << post_effects.debanding_threshold << ";\n"
                    << R"HLSL(                const float2 deband_step = frame_options.xy * 2.0;
                const float3 deband_l = tex2D(frame_sampler, uv + float2(-deband_step.x, 0.0)).rgb;
                const float3 deband_r = tex2D(frame_sampler, uv + float2( deband_step.x, 0.0)).rgb;
                const float3 deband_u = tex2D(frame_sampler, uv + float2(0.0, -deband_step.y)).rgb;
                const float3 deband_d = tex2D(frame_sampler, uv + float2(0.0,  deband_step.y)).rgb;
                const float deband_center_luma = luma(center_sample.rgb);
                const float deband_dl = abs(luma(deband_l) - deband_center_luma);
                const float deband_dr = abs(luma(deband_r) - deband_center_luma);
                const float deband_du = abs(luma(deband_u) - deband_center_luma);
                const float deband_dd = abs(luma(deband_d) - deband_center_luma);
                const float deband_inv_range = 1.0 / max(deband_threshold * 0.75, 0.0001);
                const float deband_wl = 1.0 - saturate((deband_dl - deband_threshold * 0.25) * deband_inv_range);
                const float deband_wr = 1.0 - saturate((deband_dr - deband_threshold * 0.25) * deband_inv_range);
                const float deband_wu = 1.0 - saturate((deband_du - deband_threshold * 0.25) * deband_inv_range);
                const float deband_wd = 1.0 - saturate((deband_dd - deband_threshold * 0.25) * deband_inv_range);
                const float deband_weight = 1.5 + deband_wl + deband_wr + deband_wu + deband_wd;
                const float3 deband_raw = (
                    center_sample.rgb * 1.5 +
                    deband_l * deband_wl + deband_r * deband_wr +
                    deband_u * deband_wu + deband_d * deband_wd
                ) / deband_weight;
                const float deband_grey = luma(deband_raw);
                float3 deband_corrected = lerp(float3(deband_grey, deband_grey, deband_grey), deband_raw, color_options.z);
                deband_corrected = (deband_corrected - 0.5) * color_options.y + 0.5;
                deband_corrected *= color_options.x;
                deband_corrected = lerp(deband_raw, saturate(deband_corrected), saturate(color_options.w));
                const float deband_range = max(max(deband_dl, deband_dr), max(deband_du, deband_dd));
                const float deband_flatness = 1.0 - saturate(deband_range / max(deband_threshold, 0.0001));
                color = lerp(color, saturate(deband_corrected), saturate(deband_strength * deband_flatness));
)HLSL";
            }

            // Dithering is the final color operation. A stable screen-space sequence targets
            // 8-bit A8R8G8B8 quantization directly: most of the perturbation is shared across
            // RGB to avoid colored grain, while a small phase offset decorrelates the channels.
            if(post_effects.dithering) {
                const float dither_scale = (2.0f * post_effects.dithering_strength) / 255.0f;
                final_color_source << std::fixed << std::setprecision(9)
                    << "\n                const float2 dither_pixel = floor(uv / frame_options.xy);\n"
                    << "                const float dither_phase = frac(52.9829189 * frac(dot(dither_pixel, float2(0.06711056, 0.00583715))));\n"
                    << "                const float dither_luma = dither_phase - 0.5;\n"
                    << "                const float3 dither_channels = frac(dither_phase + float3(0.0, 0.333333333, 0.666666667)) - 0.5;\n"
                    << "                const float3 dither_noise = lerp(float3(dither_luma, dither_luma, dither_luma), dither_channels, 0.25);\n"
                    << "                color = saturate(color + dither_noise * " << dither_scale << ");";
            }

            const auto final_source = final_color_source.str();
            if(!final_source.empty()) {
                prepared.insert(
                    post_color_position + std::strlen(POST_COLOR_INSERTION_POINT),
                    final_source
                );
            }
        }

        return prepared;
    }

    static std::unordered_map<std::string, ID3DBlob *> &runtime_shader_cache() noexcept {
        static std::unordered_map<std::string, ID3DBlob *> cache;
        return cache;
    }

    static std::string runtime_shader_cache_key(const std::string &prepared_source, const char *entry,
                                                const char *profile, D3D_SHADER_MACRO *defines) {
        std::string key;
        key.reserve(prepared_source.size() + 128);
        key.append(profile).push_back('\x1f');
        key.append(entry).push_back('\x1f');
        if(defines) {
            for(auto *macro = defines; macro->Name; macro++) {
                key.append(macro->Name).push_back('=');
                if(macro->Definition) {
                    key.append(macro->Definition);
                }
                key.push_back('\x1e');
            }
        }
        key.push_back('\x1f');
        key.append(prepared_source);
        return key;
    }

    static void release_runtime_shader_cache() noexcept {
        auto &cache = runtime_shader_cache();
        for(auto &entry : cache) {
            if(entry.second) {
                entry.second->Release();
                entry.second = nullptr;
            }
        }
        cache.clear();
        GraphicsRuntimeMetrics::write_log();
    }

    bool rasterizer_compile_shader(const char *source, const char *entry, const char *profile, D3D_SHADER_MACRO *defines, ID3DBlob **compiled_shader) {
        if(!source || !entry || !profile || !compiled_shader) {
            return false;
        }
        *compiled_shader = nullptr;
        const auto prepared_source = prepare_runtime_shader_source(source);
        const auto cache_key = runtime_shader_cache_key(prepared_source, entry, profile, defines);
        auto &cache = runtime_shader_cache();
        const auto cached = cache.find(cache_key);
        if(cached != cache.end() && cached->second) {
            cached->second->AddRef();
            *compiled_shader = cached->second;
            GraphicsRuntimeMetrics::shader_cache_hit();
            return true;
        }

        ID3DBlob *error_messages = nullptr;
        ID3DBlob *new_shader = nullptr;
        const DWORD flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        const HRESULT result = D3DCompile(prepared_source.c_str(), prepared_source.size(), nullptr, defines,
                                          D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile, flags, 0,
                                          &new_shader, &error_messages);
        if(FAILED(result) || !new_shader) {
            if(error_messages) {
                console_error("Pixel shader failed to compile");
                error_messages->Release();
            }
            else {
                show_error_box("Error", "compiling pixel shader: unknown error\n");
            }
            if(new_shader) {
                new_shader->Release();
            }
            return false;
        }
        if(error_messages) {
            error_messages->Release();
        }
        new_shader->AddRef();
        cache.emplace(cache_key, new_shader);
        *compiled_shader = new_shader;
        GraphicsRuntimeMetrics::shader_compiled();
        return true;
    }

    static D3DCAPS9 live_d3d9_device_caps {};
    static bool runtime_graphics_setup_complete = false;

    static bool set_up_runtime_graphics_if_device_ready() noexcept {
        if(runtime_graphics_setup_complete) {
            return true;
        }
        if(!global_d3d9_device || !*global_d3d9_device) {
            return false;
        }

        auto *device = *global_d3d9_device;
        D3DCAPS9 live_caps {};
        if(FAILED(IDirect3DDevice9_GetDeviceCaps(device, &live_caps))) {
            return false;
        }

        // Halo's global D3DCAPS9 storage can still contain startup-time zeroes here.
        // Keep Chimera on a private copy obtained from the live D3D9 device instead of
        // treating those transient values as a permanent lack of shader-model support.
        live_d3d9_device_caps = live_caps;
        d3d9_device_caps = &live_d3d9_device_caps;
        runtime_graphics_setup_complete = true;

        // Chimera Graphics is deliberately opt-in. When graphics.enabled is false,
        // this registers no D3D9 post-process events and allocates no graphics resources.
        set_up_enhanced_graphics_for_current_engine();

        // EnhancedGraphics originally only knew about FXAA. Promote its internal AA flag
        // so the EndScene callback self-suppresses while SMAA owns the strict pre-HUD path.
        // Do not install EnhancedGraphics' hook here: doing so patches the CE call site
        // before SMAA::set_up validates it, causing SMAA to reject its own hook target.
        if(graphics_requests_smaa() && !retail_pre_hud_active) {
            auto &graphics = EnhancedGraphics::state();
            if(graphics.settings.enabled && !graphics.runtime_disabled) {
                graphics.settings.fxaa = true;
            }
        }

        // Only the official SMAA 1x/T2x implementation remains.
        if(!retail_pre_hud_smaa_active) {
            SMAA::set_up();
        }

        const auto bloom = graphics_bloom_settings();
        if(bloom.enabled && d3d9_device_caps->PixelShaderVersion < 0xffff0300) {
            console_error("Chimera Graphics: Bloom requires ps_3_0 and was left disabled.");
        }

        const auto post_effects = graphics_post_effects_settings();
        if(post_effects.debanding && d3d9_device_caps->PixelShaderVersion < 0xffff0300) {
            console_error("Chimera Graphics: Debanding requires ps_3_0 and was left disabled.");
        }

        const bool pre_hud_validated =
            game_engine() == GameEngine::GAME_ENGINE_RETAIL
                ? retail_pre_hud_active
                : (game_engine() == GameEngine::GAME_ENGINE_CUSTOM_EDITION
                    ? EnhancedGraphics::validated_pre_hud_call_site() != nullptr
                    : false);
        const auto &graphics = EnhancedGraphics::state();
        const bool smaa_active =
            graphics_requests_smaa() && pre_hud_validated &&
            graphics.settings.enabled && !graphics.runtime_disabled &&
            d3d9_device_caps->PixelShaderVersion >= 0xffff0300 &&
            !SMAA::state().runtime_disabled;
        if(GraphicsRuntimeMetrics::enabled()) {
            GraphicsDiagnostics::report_once(device, pre_hud_validated, smaa_active);
        }

        return true;
    }

    static void set_up_runtime_graphics_on_preframe() noexcept {
        if(set_up_runtime_graphics_if_device_ready()) {
            remove_preframe_event(set_up_runtime_graphics_on_preframe);
        }
    }

    void set_up_rasterizer() noexcept {
        auto *device_pointer_signature = get_chimera().get_signature("model_af_set_sampler_states_sig").data();
        auto *caps_pointer_signature = get_chimera().get_signature("d3d9_device_caps_sig").data();
        if(!is_executable_memory_range(device_pointer_signature, 1 + sizeof(std::byte *)) ||
           !is_executable_memory_range(caps_pointer_signature, 1 + sizeof(std::byte *))) {
            console_error("Chimera rasterizer disabled: required D3D9 signatures were not validated.");
            return;
        }
        std::byte *device_pointer_storage = nullptr;
        std::byte *caps_pointer_storage = nullptr;
        std::memcpy(&device_pointer_storage, device_pointer_signature + 1, sizeof(device_pointer_storage));
        std::memcpy(&caps_pointer_storage, caps_pointer_signature + 1, sizeof(caps_pointer_storage));
        if(!is_readable_memory_range(device_pointer_storage, sizeof(IDirect3DDevice9 *)) ||
           !is_readable_memory_range(caps_pointer_storage, sizeof(D3DCAPS9))) {
            console_error("Chimera rasterizer disabled: D3D9 signature targets were invalid.");
            return;
        }
        global_d3d9_device = reinterpret_cast<IDirect3DDevice9 **>(device_pointer_storage);
        d3d9_device_caps = reinterpret_cast<D3DCAPS9 *>(caps_pointer_storage);
        const auto *debug_ini = get_chimera().get_ini();
        const bool debug_diagnostics = debug_ini &&
            debug_ini->get_value_bool("debug.diagnostics").value_or(false);
        GraphicsRuntimeMetrics::set_enabled(debug_diagnostics);

        set_up_environment_transparent_index_buffer_fix();
        add_game_exit_event(rasterizer_release_vertex_shaders_3_0);
        add_game_exit_event(rasterizer_release_pixel_shaders, EVENT_PRIORITY_AFTER);
        add_game_start_event(rasterizer_create_pixel_shaders);
        if(debug_ini && debug_ini->get_value_bool("debug.benchmark").value_or(false)) {
            GraphicsDiagnostics::set_up_benchmark();
        }
        if(debug_ini && debug_ini->get_value_bool("debug.visual_regression").value_or(false)) {
            GraphicsVisualRegression::set_up();
        }
        add_game_exit_event(release_runtime_shader_cache, EVENT_PRIORITY_AFTER);

        // Halo's global D3DCAPS9 block may not be populated while Chimera's
        // constructor is still running. Prefer the real device immediately when available;
        // otherwise defer only the graphics capability-dependent setup to preframe.
        if(!set_up_runtime_graphics_if_device_ready()) {
            add_preframe_event(set_up_runtime_graphics_on_preframe, EVENT_PRIORITY_BEFORE);
        }

        chimera_rasterizer_enabled = true;
    }

}
