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
    bool suppress_early_config_output = false;

    static constexpr std::size_t DEFERRED_OUTPUT_CAPACITY = 256;

    struct DeferredOutput {
        ConsoleColor color {};
        char message[256] = {};
    };

    static DeferredOutput deferred_output[DEFERRED_OUTPUT_CAPACITY] {};
    static std::size_t deferred_output_start = 0;
    static std::size_t deferred_output_count = 0;
    static std::size_t deferred_output_dropped = 0;

    extern "C" void console_output_asm(const ConsoleColor &color, const char *message);

    static void format_console_output(char *destination, std::size_t destination_size, const char *message) noexcept {
        if(!destination || destination_size == 0) {
            return;
        }

        destination[0] = 0;
        if(!message) {
            return;
        }

        if(output_prefix) {
            std::snprintf(destination, destination_size, "%s: %s", output_prefix, message);
        }
        else {
            std::strncpy(destination, message, destination_size - 1);
            destination[destination_size - 1] = 0;
        }
    }

    static void emit_console_output(const ConsoleColor &color, const char *message) noexcept {
        if(!message) {
            return;
        }

        char message_copy[256] = {};
        format_console_output(message_copy, sizeof(message_copy), message);
        console_output_asm(color, message_copy);
    }

    static bool is_error_output(const ConsoleColor &color) noexcept {
        return color.a == 1.0f && color.r == 1.0f && color.g == 0.25f && color.b == 0.25f;
    }

    static bool is_command_output() noexcept {
        return output_prefix && std::strncmp(output_prefix, "chimera_", 8) == 0;
    }

    static void defer_console_output(const ConsoleColor &color, const char *message) noexcept {
        if(!message) {
            return;
        }

        if(deferred_output_count == DEFERRED_OUTPUT_CAPACITY) {
            deferred_output_start = (deferred_output_start + 1) % DEFERRED_OUTPUT_CAPACITY;
            deferred_output_count--;
            deferred_output_dropped++;
        }

        const auto index = (deferred_output_start + deferred_output_count) % DEFERRED_OUTPUT_CAPACITY;
        auto &entry = deferred_output[index];
        entry.color = color;
        format_console_output(entry.message, sizeof(entry.message), message);
        deferred_output_count++;
    }

    static void flush_deferred_console_output() noexcept {
        for(std::size_t i = 0; i < deferred_output_count; i++) {
            const auto index = (deferred_output_start + i) % DEFERRED_OUTPUT_CAPACITY;
            const auto &entry = deferred_output[index];
            console_output_asm(entry.color, entry.message);
        }

        deferred_output_start = 0;
        deferred_output_count = 0;

        if(deferred_output_dropped > 0) {
            char message[256] = {};
            std::snprintf(message, sizeof(message), "Chimera: %zu early console message(s) were discarded because the startup queue was full.", deferred_output_dropped);
            console_output_asm(ConsoleColor {1.0, 1.0, 1.0, 0.125}, message);
            deferred_output_dropped = 0;
        }
    }

    void enable_output(bool enabled) noexcept {
        output_enabled = enabled;
        if(enabled) {
            flush_deferred_console_output();
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
            if((suppress_early_config_output || is_command_output()) && !is_error_output(color)) {
                return;
            }
            defer_console_output(color, message);
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
