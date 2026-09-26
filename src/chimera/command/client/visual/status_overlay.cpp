// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "status_overlay.hpp"
#include "../../command.hpp"
#include "../../../event/connect.hpp"
#include "../../../event/interface_render.hpp"
#include "../../../halo_data/game_engine.hpp"
#include "../../../halo_data/map.hpp"
#include "../../../halo_data/multiplayer.hpp"
#include "../../../halo_data/player.hpp"
#include "../../../output/draw_text.hpp"
#include "../../../output/output.hpp"

namespace Chimera {
    namespace {
        static bool status_overlay_enabled = true;
        static bool status_clock_enabled = true;
        static bool status_server_time_enabled = true;
        static bool status_ping_enabled = true;

        static bool session_running = false;
        static std::chrono::steady_clock::time_point session_start;

        static bool supported_engine() noexcept {
            const auto engine = game_engine();
            return engine == GameEngine::GAME_ENGINE_CUSTOM_EDITION ||
                   engine == GameEngine::GAME_ENGINE_RETAIL;
        }

        static bool ui_map_loaded() noexcept {
            auto &header = get_map_header();
            return header.is_valid() && std::strncmp(header.name, "ui", sizeof(header.name)) == 0;
        }

        static void update_session_state(Player *player) noexcept {
            if(server_type() == ServerType::SERVER_NONE || !player) {
                if(ui_map_loaded()) {
                    session_running = false;
                }
                return;
            }

            if(!session_running) {
                session_start = std::chrono::steady_clock::now();
                session_running = true;
            }
        }

        static bool on_preconnect(std::uint32_t &, std::uint16_t &, const char *) {
            session_running = false;
            return true;
        }

        static std::string local_time_string() {
            SYSTEMTIME time {};
            GetLocalTime(&time);

            unsigned int hour = time.wHour % 12;
            if(hour == 0) {
                hour = 12;
            }

            char buffer[32];
            std::snprintf(
                buffer,
                sizeof(buffer),
                "%u:%02u%s",
                hour,
                static_cast<unsigned int>(time.wMinute),
                time.wHour >= 12 ? "PM" : "AM"
            );
            return buffer;
        }

        static std::string session_time_string() {
            if(!session_running) {
                return "00:00";
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - session_start
            ).count();

            const auto hours = elapsed / 3600;
            const auto minutes = (elapsed / 60) % 60;
            const auto seconds = elapsed % 60;

            char buffer[32];
            if(hours > 0) {
                std::snprintf(
                    buffer,
                    sizeof(buffer),
                    "%02lld:%02lld:%02lld",
                    static_cast<long long>(hours),
                    static_cast<long long>(minutes),
                    static_cast<long long>(seconds)
                );
            }
            else {
                std::snprintf(
                    buffer,
                    sizeof(buffer),
                    "%02lld:%02lld",
                    static_cast<long long>(minutes),
                    static_cast<long long>(seconds)
                );
            }
            return buffer;
        }

        static ColorARGB ping_color(std::uint32_t ping) noexcept {
            if(ping < 80) {
                return ColorARGB(1.0F, 0.0F, 1.0F, 1.0F - static_cast<float>(ping) * 0.0125F);
            }
            if(ping < 160) {
                return ColorARGB(1.0F, static_cast<float>(ping - 80) * 0.0125F, 1.0F, 0.0F);
            }

            float green = 1.0F - static_cast<float>(ping - 160) * 0.0125F;
            if(green < 0.0F) {
                green = 0.0F;
            }
            return ColorARGB(1.0F, 1.0F, green, 0.0F);
        }

        static void draw_status_overlay(bool after) noexcept {
            if(!after || !supported_engine()) {
                return;
            }

            Player *player = nullptr;
            if(server_type() != ServerType::SERVER_NONE) {
                player = PlayerTable::get_player_table().get_client_player();
            }

            update_session_state(player);

            if(!status_overlay_enabled || !player || server_type() == ServerType::SERVER_NONE) {
                return;
            }

            const ColorARGB white(1.0F, 1.0F, 1.0F, 1.0F);

            if(status_clock_enabled) {
                apply_text(
                    local_time_string(),
                    -120,
                    -228,
                    240,
                    24,
                    white,
                    GenericFont::FONT_SMALL,
                    FontAlignment::ALIGN_CENTER,
                    TextAnchor::ANCHOR_CENTER,
                    true
                );
            }

            if(status_server_time_enabled) {
                apply_text(
                    session_time_string(),
                    -120,
                    210,
                    240,
                    24,
                    white,
                    GenericFont::FONT_SMALL,
                    FontAlignment::ALIGN_CENTER,
                    TextAnchor::ANCHOR_CENTER,
                    true
                );
            }

            if(status_ping_enabled) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%ums", player->ping);

                apply_text(
                    std::string(buffer),
                    150,
                    36,
                    140,
                    24,
                    ping_color(player->ping),
                    GenericFont::FONT_SMALL,
                    FontAlignment::ALIGN_RIGHT,
                    TextAnchor::ANCHOR_BOTTOM_RIGHT,
                    true
                );
            }
        }

        static bool set_bool_command(bool &value, int argc, const char **argv) {
            if(argc == 1) {
                value = STR_TO_BOOL(argv[0]);
            }
            console_output(BOOL_TO_STR(value));
            return true;
        }
    }

    void set_up_status_overlay() noexcept {
        if(!supported_engine()) {
            return;
        }

        add_preconnect_event(on_preconnect);
        add_hud_render_event(draw_status_overlay, EventPriority::EVENT_PRIORITY_DEFAULT);
    }

    bool status_overlay_command(int argc, const char **argv) {
        return set_bool_command(status_overlay_enabled, argc, argv);
    }

    bool status_clock_command(int argc, const char **argv) {
        return set_bool_command(status_clock_enabled, argc, argv);
    }

    bool status_server_time_command(int argc, const char **argv) {
        return set_bool_command(status_server_time_enabled, argc, argv);
    }

    bool status_ping_command(int argc, const char **argv) {
        return set_bool_command(status_ping_enabled, argc, argv);
    }
}
