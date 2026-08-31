// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_RASTERIZER_RETAIL_PRE_HUD_HPP
#define CHIMERA_RASTERIZER_RETAIL_PRE_HUD_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <windows.h>

#include "enhanced_graphics.hpp"
#include "smaa.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../event/d3d9_reset.hpp"
#include "../event/game_loop.hpp"
#include "../halo_data/game_engine.hpp"
#include "../output/output.hpp"
#include "../signature/hook.hpp"

namespace Chimera {
    namespace RetailPreHud {
        struct ScreenEffectCallScan {
            std::array<std::byte *, 2> hud_call_sites { nullptr, nullptr };
            std::size_t direct_xrefs = 0;
            std::size_t validated_xrefs = 0;
            std::size_t hud_call_site_count = 0;
        };

        inline bool pre_hud_requested() noexcept {
            const auto *ini = get_chimera().get_ini();
            if(!ini || !ini->get_value_bool("graphics.enabled").value_or(false) ||
               !ini->get_value_bool("graphics.smaa_exclude_hud").value_or(true)) {
                return false;
            }

            const char *aa = ini->get_value("graphics.anti_aliasing");
            if(!aa) {
                return false;
            }

            return std::strcmp(aa, "fxaa") == 0 || std::strcmp(aa, "FXAA") == 0 ||
                   std::strcmp(aa, "smaa") == 0 || std::strcmp(aa, "SMAA") == 0 ||
                   std::strcmp(aa, "smaa_t2x") == 0 || std::strcmp(aa, "SMAA_T2X") == 0;
        }

        inline bool smaa_requested() noexcept {
            const auto *ini = get_chimera().get_ini();
            const char *aa = ini ? ini->get_value("graphics.anti_aliasing") : nullptr;
            return aa && (
                std::strcmp(aa, "smaa") == 0 || std::strcmp(aa, "SMAA") == 0 ||
                std::strcmp(aa, "smaa_t2x") == 0 || std::strcmp(aa, "SMAA_T2X") == 0
            );
        }

        inline std::byte *relative_call_target(std::byte *site) noexcept {
            if(!site || *reinterpret_cast<const std::uint8_t *>(site) != 0xE8) {
                return nullptr;
            }

            std::int32_t displacement = 0;
            std::memcpy(&displacement, site + 1, sizeof(displacement));

            const auto next = static_cast<std::int64_t>(
                reinterpret_cast<std::uintptr_t>(site) + 5U
            );
            const auto target = next + static_cast<std::int64_t>(displacement);
            if(target <= 0) {
                return nullptr;
            }
            return reinterpret_cast<std::byte *>(static_cast<std::uintptr_t>(target));
        }

        inline bool find_hud_call_after_screen_effect(
            std::byte *screen_effect_call,
            std::byte *section_end,
            std::byte *screen_effect,
            std::byte *&hud_call_site
        ) noexcept {
            hud_call_site = nullptr;
            if(!screen_effect_call || !section_end || screen_effect_call >= section_end ||
               static_cast<std::size_t>(section_end - screen_effect_call) < 5) {
                return false;
            }

            // interface_draw_screen has a stable semantic tail: once Halo has finished
            // rasterizer_screen_effect, the next direct CALL draws the HUD and the following
            // distinct CALL performs game_engine_post_rasterize. Retail can fold the two
            // source-level screen-effect branches into one shared CALL, so derive the actual
            // pre-HUD boundary from this tail instead of executing on the screen-effect CALL.
            auto *cursor = screen_effect_call + 5;
            const auto remaining = static_cast<std::size_t>(section_end - cursor);
            auto *scan_end = cursor + (remaining > 0x40U ? 0x40U : remaining);

            std::byte *first_call_site = nullptr;
            std::byte *first_target = nullptr;
            std::byte *second_target = nullptr;
            while(static_cast<std::size_t>(scan_end - cursor) >= 5) {
                if(*reinterpret_cast<const std::uint8_t *>(cursor) == 0xE8) {
                    auto *target = relative_call_target(cursor);
                    if(target && target != screen_effect) {
                        if(!first_target) {
                            first_call_site = cursor;
                            first_target = target;
                        }
                        else if(target != first_target) {
                            second_target = target;
                            break;
                        }
                    }
                }
                cursor++;
            }

            if(!first_call_site || !first_target || !second_target) {
                return false;
            }

            hud_call_site = first_call_site;
            return true;
        }

        inline ScreenEffectCallScan scan_screen_effect_call_sites() noexcept {
            ScreenEffectCallScan scan;

            auto *module = reinterpret_cast<std::byte *>(GetModuleHandle(nullptr));
            auto *screen_effect = get_chimera().get_signature("widescreen_screen_effect_sig").data();
            if(!module || !screen_effect) {
                return scan;
            }

            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
            if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
                return scan;
            }

            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(module + dos->e_lfanew);
            if(nt->Signature != IMAGE_NT_SIGNATURE) {
                return scan;
            }

            const auto *section = reinterpret_cast<const IMAGE_SECTION_HEADER *>(
                reinterpret_cast<const std::byte *>(&nt->OptionalHeader) +
                nt->FileHeader.SizeOfOptionalHeader
            );

            for(unsigned int i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
                if((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
                   (section->Characteristics & IMAGE_SCN_CNT_CODE) == 0 ||
                   section->Misc.VirtualSize < 5) {
                    continue;
                }

                const auto rva = static_cast<std::uintptr_t>(section->VirtualAddress);
                const auto size = static_cast<std::uintptr_t>(section->Misc.VirtualSize);
                if(rva >= nt->OptionalHeader.SizeOfImage ||
                   size > nt->OptionalHeader.SizeOfImage - rva) {
                    continue;
                }

                auto *begin = module + rva;
                auto *end = begin + size;
                for(auto *site = begin; static_cast<std::size_t>(end - site) >= 5; site++) {
                    if(*reinterpret_cast<const std::uint8_t *>(site) != 0xE8 ||
                       relative_call_target(site) != screen_effect) {
                        continue;
                    }

                    scan.direct_xrefs++;
                    std::byte *hud_call_site = nullptr;
                    if(find_hud_call_after_screen_effect(site, end, screen_effect, hud_call_site)) {
                        scan.validated_xrefs++;

                        bool already_recorded = false;
                        for(std::size_t h = 0; h < scan.hud_call_site_count; h++) {
                            if(scan.hud_call_sites[h] == hud_call_site) {
                                already_recorded = true;
                                break;
                            }
                        }

                        if(!already_recorded) {
                            if(scan.hud_call_site_count < scan.hud_call_sites.size()) {
                                scan.hud_call_sites[scan.hud_call_site_count] = hud_call_site;
                            }
                            scan.hud_call_site_count++;
                        }
                    }

                    if(scan.direct_xrefs > 2 || scan.validated_xrefs > 2 ||
                       scan.hud_call_site_count > scan.hud_call_sites.size()) {
                        return scan;
                    }
                }
            }

            return scan;
        }

        inline bool install_pre_hud_call_hooks(
            const void *callback,
            ScreenEffectCallScan &scan
        ) noexcept {
            static std::array<Hook, 2> hooks {};
            static const void *installed_callback = nullptr;
            static std::size_t installed_hook_count = 0;

            if(installed_hook_count > 0) {
                bool all_installed = installed_callback == callback;
                for(std::size_t i = 0; i < installed_hook_count; i++) {
                    all_installed = all_installed && hooks[i].address && hooks[i].hook &&
                                    !hooks[i].original_bytes.empty();
                }
                if(all_installed) {
                    scan.direct_xrefs = installed_hook_count;
                    scan.validated_xrefs = installed_hook_count;
                    scan.hud_call_site_count = installed_hook_count;
                    return true;
                }
                return false;
            }

            scan = scan_screen_effect_call_sites();

            if(scan.direct_xrefs < 1 || scan.direct_xrefs > 2 ||
               scan.validated_xrefs != scan.direct_xrefs ||
               scan.hud_call_site_count < 1 || scan.hud_call_site_count > 2) {
                return false;
            }

            for(std::size_t i = 0; i < scan.hud_call_site_count; i++) {
                if(!scan.hud_call_sites[i]) {
                    for(std::size_t rollback = 0; rollback < i; rollback++) {
                        hooks[rollback].rollback();
                    }
                    return false;
                }

                // This is the actual pre-HUD boundary: Chimera runs immediately before
                // Halo's hud_draw_screen CALL, then the original HUD call executes normally.
                write_jmp_call(scan.hud_call_sites[i], hooks[i], callback);
                const bool installed =
                    hooks[i].address == scan.hud_call_sites[i] && hooks[i].hook &&
                    !hooks[i].original_bytes.empty();
                if(!installed) {
                    for(std::size_t rollback = 0; rollback <= i; rollback++) {
                        if(hooks[rollback].address) {
                            hooks[rollback].rollback();
                        }
                    }
                    return false;
                }
            }

            installed_callback = callback;
            installed_hook_count = scan.hud_call_site_count;
            return true;
        }

        inline void finalize_after_output_enabled() noexcept {
            static bool attempted = false;
            if(attempted) {
                return;
            }
            attempted = true;

            if(game_engine() != GameEngine::GAME_ENGINE_RETAIL || !pre_hud_requested()) {
                return;
            }

            auto &graphics = EnhancedGraphics::state();
            if(!graphics.settings.enabled || graphics.runtime_disabled) {
                console_error("Chimera Graphics: Retail pre-HUD was requested, but graphics is not active.");
                return;
            }

            // Early startup may already have installed a validated Retail pre-HUD path.
            if(graphics.settings.smaa_exclude_hud) {
                return;
            }

            const bool use_smaa = smaa_requested();
            if(use_smaa && (!d3d9_device_caps || d3d9_device_caps->PixelShaderVersion < 0xffff0300)) {
                console_error("Chimera Graphics: Retail SMAA pre-HUD requires ps_3_0; full-frame fallback remains active.");
                return;
            }

            const void *callback = use_smaa
                ? reinterpret_cast<const void *>(SMAA::on_pre_hud)
                : reinterpret_cast<const void *>(EnhancedGraphics::on_pre_hud);

            ScreenEffectCallScan scan;
            if(!install_pre_hud_call_hooks(callback, scan)) {
                console_error(
                    "Chimera Graphics: Halo PC Retail pre-HUD validation failed; "
                    "full-frame fallback remains active."
                );
                return;
            }

            // Suppress the already-registered full-frame compatibility callback. The new
            // callback now runs on the derived hud_draw_screen CALL itself.
            graphics.settings.smaa_exclude_hud = true;
            if(use_smaa) {
                graphics.settings.fxaa = true;
                add_d3d9_reset_event(SMAA::on_reset, EVENT_PRIORITY_BEFORE);
                add_game_exit_event(SMAA::release_resources, EVENT_PRIORITY_BEFORE);
            }
        }
    }
}

#endif
