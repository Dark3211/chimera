// SPDX-License-Identifier: GPL-3.0-only

#include <cstring>
#include "output.hpp"
#include "../config/ini.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../custom_chat/custom_chat.hpp"
#include "../console/console.hpp"
#include "../event/rcon_message.hpp"
#include "../rasterizer/retail_pre_hud.hpp"
#include "../chimera.hpp"

namespace Chimera {
    const char *output_prefix = nullptr;

    static bool output_enabled = false;

    struct DeferredGraphicsOutput {
        ConsoleColor color {};
        char message[256] = {};
    };

    static DeferredGraphicsOutput deferred_graphics_output[16] {};
    static std::size_t deferred_graphics_output_count = 0;

    extern "C" void console_output_asm(const ConsoleColor &color, const char *message);

    static void emit_console_output(const ConsoleColor &color, const char *message) noexcept {
        if(!message) {
            return;
        }

        char message_copy[256] = {};
        if(output_prefix) {
            std::snprintf(message_copy, sizeof(message_copy), "%s: %s", output_prefix, message);
        }
        else {
            std::strncpy(message_copy, message, sizeof(message_copy) - 1);
        }
        console_output_asm(color, message_copy);
    }

    static bool is_graphics_diagnostic(const char *message) noexcept {
        static constexpr const char *PREFIX = "Chimera Graphics";
        return message && std::strncmp(message, PREFIX, std::strlen(PREFIX)) == 0;
    }

    static void defer_graphics_output(const ConsoleColor &color, const char *message) noexcept {
        if(!is_graphics_diagnostic(message) ||
           deferred_graphics_output_count >= sizeof(deferred_graphics_output) / sizeof(*deferred_graphics_output)) {
            return;
        }

        auto &entry = deferred_graphics_output[deferred_graphics_output_count++];
        entry.color = color;
        std::strncpy(entry.message, message, sizeof(entry.message) - 1);
    }

    static void flush_deferred_graphics_output() noexcept {
        for(std::size_t i = 0; i < deferred_graphics_output_count; i++) {
            emit_console_output(
                deferred_graphics_output[i].color,
                deferred_graphics_output[i].message
            );
        }
        deferred_graphics_output_count = 0;
    }

    void enable_output(bool enabled) noexcept {
        output_enabled = enabled;
        if(enabled) {
            // Graphics diagnostics can be generated while the rasterizer is initialized,
            // before Halo's console output path is ready. Replay only those early graphics
            // diagnostics once output becomes available instead of silently losing them.
            flush_deferred_graphics_output();

            // Retail's semantic pre-HUD discovery is delayed until output is available so
            // a failed validation can still report the safe full-frame fallback.
            RetailPreHud::finalize_after_output_enabled();
        }
    }

    extern "C" void send_rcon_message_asm(std::uint32_t player, const char *message) noexcept;
    void send_rcon_message(int player, const char *message) {
        if(!message) {
            return;
        }
        send_rcon_message_asm(static_cast<std::uint32_t>(player), message);
    }

    void console_output_raw(const ConsoleColor &color, const char *message) noexcept {
        if(!message) {
            return;
        }
        if(!output_enabled) {
            defer_graphics_output(color, message);
            return;
        }
        emit_console_output(color, message);
    }

    extern "C" void hud_output_asm(const wchar_t *message);
    void hud_output_raw(const wchar_t *message) noexcept {
        if(!output_enabled || !message) {
            return;
        }
        hud_output_asm(message);
    }
    void hud_output_raw(const char *message) noexcept {
        if(!message) {
            return;
        }
        wchar_t x[256] = {};
        for(std::size_t i = 0; i < sizeof(x) / sizeof(*x) - 1 && message[i]; i++) {
            x[i] = message[i];
        }
        hud_output_raw(x);
    }

    static bool server_messages_are_blocked = false;
    static bool server_message_allow_unsolicted_rcon_messages = false;

    extern "C" void before_rcon_message() noexcept;
    extern "C" bool on_rcon_message(const char *message) noexcept {
        if(!message) {
            return false;
        }
        if (!call_rcon_message_events(message)) {
            return false;
        }
        else if(server_message_allow_unsolicted_rcon_messages || rcon_used_recently()) {
            return true;
        }
        else if(custom_chat_enabled()) {
            add_server_message(message);
            return false;
        }
        else {
            return !server_messages_are_blocked;
        }
    }

    void set_up_rcon_message_hook() noexcept {
        static bool enabled = false;
        if(enabled) {
            return;
        }
        enabled = true;

        static Hook hook;
        auto &chimera = get_chimera();
        if(get_chimera().feature_present("client_rcon")) {
            write_jmp_call(chimera.get_signature("rcon_message_sig").data(), hook, reinterpret_cast<const void *>(before_rcon_message));
        }
        auto *ini = chimera.get_ini();
        server_message_allow_unsolicted_rcon_messages = ini ? ini->get_value_bool("custom_chat.server_message_allow_unsolicted_rcon_messages").value_or(false) : false;
    }

    void set_server_messages_blocked(bool blocked) noexcept {
        server_messages_are_blocked = blocked;
    }

    bool server_messages_blocked() noexcept {
        return server_messages_are_blocked;
    }
}
