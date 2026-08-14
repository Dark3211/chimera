// SPDX-License-Identifier: GPL-3.0-only

#include <cstdio>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "bookmark.hpp"
#include "../chimera.hpp"
#include "../event/frame.hpp"
#include <windows.h>
#include <cstring>
#include <optional>
#include <fstream>
#include "../event/connect.hpp"
#include "../output/output.hpp"
#include "../halo_data/script.hpp"
#include "../halo_data/resolution.hpp"
#include "../localization/localization.hpp"
#include <mutex>
#include <thread>

namespace Chimera {
    #define MAX_HISTORY_SIZE 20

    static Bookmark latest_connection = {};

    const Bookmark &get_latest_connection() noexcept {
        return latest_connection;
    }

    static std::optional<Bookmark> parse_bookmark(const char *line) {
        if(!line) {
            return std::nullopt;
        }

        Bookmark b = {};
        std::string input(line);
        while(!input.empty() && (input.back() == '\r' || input.back() == '\n')) {
            input.pop_back();
        }
        if(input.empty()) {
            return std::nullopt;
        }

        std::size_t address_start = 0;
        std::size_t address_end = 0;
        if(input.front() == '[') {
            const auto close = input.find(']');
            if(close == std::string::npos || close <= 1) {
                return std::nullopt;
            }
            b.brackets = true;
            address_start = 1;
            address_end = close;
        }
        else {
            address_start = 0;
            const auto colon = input.find(':');
            const auto space = input.find(' ');
            address_end = colon == std::string::npos || (space != std::string::npos && space < colon)
                ? (space == std::string::npos ? input.size() : space)
                : colon;
        }

        if(address_end <= address_start) {
            return std::nullopt;
        }
        const std::string address = input.substr(address_start, address_end - address_start);
        if(address.size() < 2 || address.size() >= sizeof(b.address)) {
            return std::nullopt;
        }
        std::snprintf(b.address, sizeof(b.address), "%s", address.c_str());

        std::size_t cursor = b.brackets ? address_end + 1 : address_end;
        if(cursor < input.size() && input[cursor] == ':') {
            cursor++;
            const std::size_t port_begin = cursor;
            while(cursor < input.size() && input[cursor] != ' ') {
                if(input[cursor] < '0' || input[cursor] > '9') {
                    return std::nullopt;
                }
                cursor++;
            }
            if(cursor == port_begin) {
                return std::nullopt;
            }
            try {
                const unsigned long port = std::stoul(input.substr(port_begin, cursor - port_begin));
                if(port == 0 || port > 65535UL) {
                    return std::nullopt;
                }
                b.port = static_cast<std::uint16_t>(port);
            }
            catch(const std::exception &) {
                return std::nullopt;
            }
        }
        else {
            b.port = 2302;
        }

        while(cursor < input.size() && input[cursor] == ' ') {
            cursor++;
        }
        if(cursor < input.size()) {
            const auto password = input.substr(cursor);
            if(password.size() >= sizeof(b.password)) {
                return std::nullopt;
            }
            std::snprintf(b.password, sizeof(b.password), "%s", password.c_str());
        }
        return b;
    }

    static bool on_connect(std::uint32_t &ip, std::uint16_t &port, const char *password) {
        // Prepare the bookmark
        std::uint8_t *ip_chars = reinterpret_cast<std::uint8_t *>(&ip);
        Bookmark x = {};
        std::snprintf(x.address, sizeof(x.address), "%i.%i.%i.%i", ip_chars[3], ip_chars[2], ip_chars[1], ip_chars[0]);
        x.port = port;
        std::snprintf(x.password, sizeof(x.password), "%s", password ? password : "");

        // See if it's already in the history. If so, remove it
        auto history = load_bookmarks_file("history.txt");
        for(auto &h : history) {
            if(std::strcmp(h.address, x.address) == 0 && x.port == h.port) {
                history.erase(history.begin() + (&h - history.data()));
                break;
            }
        }

        // Add it to the front
        history.insert(history.begin(), x);

        // If we have too many items in the history, remove the last one
        while(history.size() > MAX_HISTORY_SIZE) {
            history.erase(history.end() - 1);
        }

        // Save
        save_bookmarks_file("history.txt", history);

        // Keep this for later
        latest_connection = x;

        return true;
    }

    void set_up_server_history() noexcept {
        add_preconnect_event(on_connect, EVENT_PRIORITY_FINAL);
    }

    std::vector<Bookmark> load_bookmarks_file(const char *file) noexcept {
        std::ifstream f(get_chimera().get_path() / file);
        std::string line;
        std::vector<Bookmark> bookmarks;
        while(std::getline(f, line)) {
            auto parsed_bookmark = parse_bookmark(line.data());
            if(parsed_bookmark.has_value()) {
                bookmarks.push_back(*parsed_bookmark);
            }
        }
        return bookmarks;
    }

    void save_bookmarks_file(const char *file, const std::vector<Bookmark> &bookmarks) noexcept {
        std::ofstream f(get_chimera().get_path() / file, std::ios_base::out | std::ios_base::trunc);

        for(auto &b : bookmarks) {
            char line[256];

            // If we have a password, also include that.
            if(b.password[0]) {
                std::snprintf(line, sizeof(line), "%s%s%s:%u %s", b.brackets ? "[" : "", b.address, b.brackets ? "]" : "", b.port, b.password);
            }
            // Otherwise it's not really required to have a password
            else {
                std::snprintf(line, sizeof(line), "%s%s%s:%u", b.brackets ? "[" : "", b.address, b.brackets ? "]" : "", b.port);
            }
            for(auto &c : line) {
                if(c == '\r' || c == '\n' || c == '\t') {
                    c = ' ';
                }
            }

            f << line << "\n";
        }

        f.flush();
        f.close();
    }
    static std::vector<QueryPacketDone> finished_packets;

    static std::mutex querying;

    QueryPacketDone query_server(const Bookmark &what) {
        QueryPacketDone finished_packet{};
        finished_packet.b = what;
        finished_packet.error = QueryPacketDone::Error::TIMED_OUT;

        char port[6] = {};
        std::snprintf(port, sizeof(port), "%u", what.port);

        addrinfo hints = {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo *addresses = nullptr;
        if(getaddrinfo(what.address, port, &hints, &addresses) != 0) {
            finished_packet.error = QueryPacketDone::Error::FAILED_TO_RESOLVE;
            return finished_packet;
        }

        static constexpr char PACKET_QUERY[] = "\\query";
        for(addrinfo *address = addresses; address; address = address->ai_next) {
            if((address->ai_family != AF_INET && address->ai_family != AF_INET6) || address->ai_addrlen > sizeof(sockaddr_storage)) {
                continue;
            }

            SOCKET s = socket(address->ai_family, SOCK_DGRAM, IPPROTO_UDP);
            if(s == INVALID_SOCKET) {
                continue;
            }

            DWORD timeout_ms = 700;
            if(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms)) == SOCKET_ERROR) {
                closesocket(s);
                continue;
            }

            if(sendto(s, PACKET_QUERY, sizeof(PACKET_QUERY) - 1, 0, address->ai_addr, static_cast<int>(address->ai_addrlen)) == SOCKET_ERROR) {
                closesocket(s);
                continue;
            }

            char data[4096] = {};
            sockaddr_storage response_address = {};
            int response_length = sizeof(response_address);
            const auto start = std::chrono::steady_clock::now();
            const int received = recvfrom(s, data, static_cast<int>(sizeof(data) - 1), 0, reinterpret_cast<sockaddr *>(&response_address), &response_length);
            const auto end = std::chrono::steady_clock::now();
            closesocket(s);

            if(received <= 1 || received == SOCKET_ERROR) {
                continue;
            }

            data[received] = 0;
            finished_packet.ping = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            std::pair<std::string, std::string> kv;
            bool key = true;
            char *str_start = data + 1;
            for(char *c = str_start; *c; c++) {
                bool break_now = false;
                if(*c != '\\' && c[1] == 0) {
                    c++;
                    break_now = true;
                }
                if(*c == '\\') {
                    if(key) {
                        kv.first.assign(str_start, c - str_start);
                    }
                    else {
                        for(char *k = str_start; k + 1 < c && *k <= 0x20; k++, str_start++);
                        kv.second.assign(str_start, c - str_start);
                        for(char &value : kv.second) {
                            if(static_cast<unsigned char>(value) < 0x20) {
                                value = '?';
                            }
                        }
                        finished_packet.query_data.insert_or_assign(kv.first, kv.second);
                        kv = {};
                    }
                    key = !key;
                    str_start = c + 1;
                }
                if(break_now) {
                    break;
                }
            }
            finished_packet.error = QueryPacketDone::Error::NONE;
            break;
        }

        freeaddrinfo(addresses);
        return finished_packet;
    }

    static void query_list(const std::vector<Bookmark> &bookmarks) {
        finished_packets.clear();

        for(auto &b : bookmarks) {
            finished_packets.push_back(query_server(b));
        }

        querying.unlock();
    }

    static void show_list() {
        if(!querying.try_lock()) {
            return;
        }
        querying.unlock();

        // Show the results
        auto &resolution = get_resolution();
        bool can_use_tabs = static_cast<float>(resolution.width) / resolution.height > (4.05F / 3.0F);
        if(can_use_tabs) {
            console_output(localize("chimera_bookmark_list_header"));
        }
        std::size_t q = 0;

        for(auto &p : finished_packets) {
            q++;
            switch(p.error) {
                case QueryPacketDone::Error::NONE: {
                    float red = 0.0F;
                    float green = 0.0F;

                    // <15 ms = green
                    if(p.ping < 15) {
                        green = 1.0F;
                    }

                    // 30 - 80 ms = green to yellow
                    else if(p.ping < 80) {
                        green = 1.0F;
                        red = (p.ping - 15) / static_cast<float>(80 - 15);
                    }

                    // 80 - 200 ms = yellow to red
                    else if(p.ping < 200) {
                        green = 1.0F - (p.ping - 80) / static_cast<float>(200 - 80);
                        red = 1.0F;
                    }

                    // 200 ms - 300 ms = darker red
                    else if(p.ping < 300) {
                        red = 1.0F - (p.ping - 200) / static_cast<float>(300 - 200) * 0.7F;
                    }

                    // 300+ ping = dark red
                    else {
                        red = 0.7F;
                    }

                    // Sometimes the game variant isn't set to anything (default gametype I think?). In this case, use the gametype.
                    const char *gametype = p.get_data_for_key("gamevariant");
                    if(!gametype[0]) {
                        gametype = p.get_data_for_key("gametype");
                    }

                    if(can_use_tabs) {
                        console_output(ConsoleColor { 1.0F, red, green, 0.25F }, "%zu. %s|t%s|t%s|t%s / %s|r%zu ms", q, p.get_data_for_key("hostname"), p.get_data_for_key("mapname"), gametype, p.get_data_for_key("numplayers"), p.get_data_for_key("maxplayers"), p.ping);
                    }
                    else {
                        console_output(ConsoleColor { 1.0F, red, green, 0.25F }, "%zu. %s (%s; %s / %s) - %zu ms", q, p.get_data_for_key("hostname"), p.get_data_for_key("mapname"), p.get_data_for_key("numplayers"), p.get_data_for_key("maxplayers"), p.ping);
                    }

                    break;
                }
                case QueryPacketDone::Error::FAILED_TO_RESOLVE:
                    console_output(ConsoleColor { 1.0F, 1.0F, 0.5F, 0.5F }, "%zu. %s%s%s:%zu|t%s", q, p.b.brackets ? "[" : "", p.b.address, p.b.brackets ? "]" : "", p.b.port, localize("chimera_bookmark_list_error_failed_to_resolve"));
                    break;
                case QueryPacketDone::Error::TIMED_OUT:
                    console_output(ConsoleColor { 1.0F, 1.0F, 0.5F, 0.5F }, "%zu. %s%s%s:%zu|t%s", q, p.b.brackets ? "[" : "", p.b.address, p.b.brackets ? "]" : "", p.b.port, localize("chimera_bookmark_list_error_timed_out"));
                    break;
            }
        }

        remove_preframe_event(show_list);
    }

    bool history_list_command(int, const char **) {
        if(!querying.try_lock()) {
            console_error(localize("chimera_bookmark_list_command_busy"));
            return false;
        }
        console_output(localize("chimera_history_list_command_querying"));
        add_preframe_event(show_list);
        std::thread(query_list, load_bookmarks_file("history.txt")).detach();
        return true;
    }

    bool bookmark_list_command(int, const char **) {
        if(!querying.try_lock()) {
            console_error(localize("chimera_bookmark_list_command_busy"));
            return false;
        }
        console_output(localize("chimera_bookmark_list_command_querying"));
        add_preframe_event(show_list);
        std::thread(query_list, load_bookmarks_file("bookmark.txt")).detach();
        return true;
    }

    bool bookmark_add_command(int argc, const char **argv) {
        auto bookmarks = load_bookmarks_file("bookmark.txt");
        Bookmark new_bookmark = {};
        if(argc == 0) {
            auto history = load_bookmarks_file("history.txt");
            if(history.size() == 0) {
                console_error(localize("chimera_bookmark_add_no_recent_servers"));
                return false;
            }
            new_bookmark = history[0];
        }
        else {
            auto potential_bookmark = parse_bookmark(*argv);
            if(!potential_bookmark.has_value()) {
                console_error(localize("chimera_bookmark_error_invalid"));
                return false;
            }
            new_bookmark = *potential_bookmark;

            // If we have a password, add that too
            if(argc == 2) {
                if(std::strlen(argv[1]) < sizeof(new_bookmark.password)) {
                    std::strncpy(new_bookmark.password, argv[1], sizeof(new_bookmark.password) - 1);
                }
                else {
                    console_error(localize("chimera_bookmark_error_password_too_long"));
                    return false;
                }
            }
        }

        // Simply replace if found
        for(std::size_t b = 0; b < bookmarks.size(); b++) {
            auto &bookmark = bookmarks[b];
            if(std::strcmp(bookmark.address, new_bookmark.address) == 0 && bookmark.port == new_bookmark.port) {
                bookmark = new_bookmark;
                save_bookmarks_file("bookmark.txt", bookmarks);
                console_output(localize("chimera_bookmark_add_success"), bookmark.brackets ? "[" : "", bookmark.address, bookmark.brackets ? "]" : "", bookmark.port, b);
                return true;
            }
        }

        bookmarks.emplace_back(new_bookmark);
        console_output(localize("chimera_bookmark_add_success"), new_bookmark.brackets ? "[" : "", new_bookmark.address, new_bookmark.brackets ? "]" : "", new_bookmark.port, bookmarks.size());
        save_bookmarks_file("bookmark.txt", bookmarks);

        return true;
    }

    bool bookmark_delete_command(int argc, const char **argv) {
        auto bookmarks = load_bookmarks_file("bookmark.txt");
        Bookmark delete_bookmark = {};
        if(argc == 0) {
            auto history = load_bookmarks_file("history.txt");
            if(history.size() == 0) {
                console_error(localize("chimera_bookmark_add_no_recent_servers"));
                return false;
            }
            delete_bookmark = history[0];
        }
        else {
            try {
                std::size_t index = static_cast<std::size_t>(std::stoul(*argv));
                if(index > bookmarks.size() || index < 1) {
                    console_error(localize("chimera_bookmark_error_not_found"));
                    return false;
                }
                delete_bookmark = bookmarks[index - 1];
            }
            catch(std::exception &) {
                auto potential_bookmark = parse_bookmark(*argv);
                if(!potential_bookmark.has_value()) {
                    console_error(localize("chimera_bookmark_error_invalid"));
                    return false;
                }
                delete_bookmark = *potential_bookmark;
            }
        }

        // Remove
        bool success = false;

        // Delete if found
        for(std::size_t b = 0; b < bookmarks.size(); b++) {
            auto &bookmark = bookmarks[b];
            if(std::strcmp(bookmark.address, delete_bookmark.address) == 0 && bookmark.port == delete_bookmark.port) {
                success = true;
                console_output(localize("chimera_bookmark_delete_success"), bookmark.brackets ? "[" : "", bookmark.address, bookmark.brackets ? "]" : "", bookmark.port);
                bookmarks.erase(bookmarks.begin() + b);
                b--;
            }
        }
        if(success) {
            save_bookmarks_file("bookmark.txt", bookmarks);
        }
        else {
            console_error(localize("chimera_bookmark_error_not_found"));
        }

        return success;
    }

    static void join_bookmark(const Bookmark &bookmark) {
        char escaped_password[sizeof(bookmark.password) * 2] = {};
        std::size_t escaped_length = 0;
        for(char c : bookmark.password) {
            if(!c) {
                break;
            }
            if(c == '\\' || c == '"') {
                if(escaped_length + 1 >= sizeof(escaped_password)) {
                    return;
                }
                escaped_password[escaped_length++] = '\\';
            }
            if(escaped_length + 1 >= sizeof(escaped_password)) {
                return;
            }
            escaped_password[escaped_length++] = c;
        }
        escaped_password[escaped_length] = 0;

        char connect_command[256];
        std::snprintf(connect_command, sizeof(connect_command), "connect \"%s%s%s:%u\" \"%s\"", bookmark.brackets ? "[" : "", bookmark.address, bookmark.brackets ? "]" : "", bookmark.port, escaped_password);
        execute_script(connect_command);
    }

    bool bookmark_connect_command(int, const char **argv) {
        auto bookmarks = load_bookmarks_file("bookmark.txt");
        std::size_t index;
        try {
            index = static_cast<std::size_t>(std::stoul(*argv));
        }
        catch(std::exception &) {
            console_error(localize("chimera_bookmark_error_invalid"));
            return false;
        }
        if(index < 1 || index > bookmarks.size()) {
            console_error(localize("chimera_bookmark_error_invalid"));
            return false;
        }
        join_bookmark(bookmarks[index - 1]);
        return true;
    }

    bool history_connect_command(int, const char **argv) {
        auto history = load_bookmarks_file("history.txt");
        std::size_t index;
        try {
            index = static_cast<std::size_t>(std::stoul(*argv));
        }
        catch(std::exception &) {
            console_error(localize("chimera_bookmark_error_invalid"));
            return false;
        }
        if(index < 1 || index > history.size()) {
            console_error(localize("chimera_bookmark_error_invalid"));
            return false;
        }
        join_bookmark(history[index - 1]);
        return true;
    }
}
