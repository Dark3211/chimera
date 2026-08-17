// SPDX-License-Identifier: GPL-3.0-only

#include <cstdio>
#include <cstring>
#include <thread>

#define CURL_STATICLIB
#include <curl/curl.h>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <limits>
#include <algorithm>
#include <cstdint>

#include "map_downloader.hpp"

namespace {
    thread_local const char *map_download_callback_failure_reason = nullptr;
    thread_local std::size_t map_download_callback_bytes_received = 0;
    thread_local bool map_download_header_checked = false;
    thread_local std::uint32_t map_download_header_head = 0;
    thread_local std::uint32_t map_download_header_foot = 0;
    thread_local std::uint32_t map_download_demo_head = 0;
    thread_local std::uint32_t map_download_demo_foot = 0;

    void set_map_download_callback_failure(const char *reason) noexcept {
        if(!map_download_callback_failure_reason) {
            map_download_callback_failure_reason = reason;
        }
    }

    void reset_map_download_callback_diagnostics() noexcept {
        map_download_callback_failure_reason = nullptr;
        map_download_callback_bytes_received = 0;
        map_download_header_checked = false;
        map_download_header_head = 0;
        map_download_header_foot = 0;
        map_download_demo_head = 0;
        map_download_demo_foot = 0;
    }

    void write_map_download_debug_log(const std::filesystem::path &output_file,
                                      const std::string &map,
                                      const std::string &game_engine,
                                      const char *failure_reason,
                                      CURLcode curl_result,
                                      const char *curl_error_buffer,
                                      long http_response_code,
                                      long redirect_count,
                                      long os_errno,
                                      curl_off_t curl_downloaded_size,
                                      std::size_t callback_bytes_received,
                                      const std::string &content_type,
                                      const std::string &effective_url,
                                      bool header_checked,
                                      std::uint32_t header_head,
                                      std::uint32_t header_foot,
                                      std::uint32_t demo_head,
                                      std::uint32_t demo_foot) noexcept {
        try {
            std::filesystem::path debug_path = output_file.parent_path() / "map_download_debug.log";
            if(output_file.parent_path().empty()) {
                debug_path = "map_download_debug.log";
            }

            std::FILE *debug_file = std::fopen(debug_path.string().c_str(), "wb");
            if(!debug_file) {
                return;
            }

            const char *curl_description = curl_easy_strerror(curl_result);
            std::fprintf(debug_file, "Chimera map downloader diagnostic\n");
            std::fprintf(debug_file, "map=%s\n", map.c_str());
            std::fprintf(debug_file, "game_engine=%s\n", game_engine.c_str());
            std::fprintf(debug_file, "failure_reason=%s\n", failure_reason ? failure_reason : "UNKNOWN");
            std::fprintf(debug_file, "curl_code=%d\n", static_cast<int>(curl_result));
            std::fprintf(debug_file, "curl_description=%s\n", curl_description ? curl_description : "(none)");
            std::fprintf(debug_file, "curl_error=%s\n", curl_error_buffer && *curl_error_buffer ? curl_error_buffer : "(none)");
            std::fprintf(debug_file, "http_response_code=%ld\n", http_response_code);
            std::fprintf(debug_file, "redirect_count=%ld\n", redirect_count);
            std::fprintf(debug_file, "os_errno=%ld\n", os_errno);
            std::fprintf(debug_file, "curl_downloaded_bytes=%lld\n", static_cast<long long>(curl_downloaded_size));
            std::fprintf(debug_file, "callback_received_bytes=%zu\n", callback_bytes_received);
            std::fprintf(debug_file, "content_type=%s\n", content_type.empty() ? "(none)" : content_type.c_str());
            std::fprintf(debug_file, "effective_url=%s\n", effective_url.empty() ? "(none)" : effective_url.c_str());
            std::fprintf(debug_file, "header_checked=%s\n", header_checked ? "yes" : "no");
            if(header_checked) {
                std::fprintf(debug_file, "header_head=0x%08X\n", static_cast<unsigned int>(header_head));
                std::fprintf(debug_file, "header_foot=0x%08X\n", static_cast<unsigned int>(header_foot));
                std::fprintf(debug_file, "demo_head=0x%08X\n", static_cast<unsigned int>(demo_head));
                std::fprintf(debug_file, "demo_foot=0x%08X\n", static_cast<unsigned int>(demo_foot));
            }

            std::fclose(debug_file);
        }
        catch(...) {
            // Diagnostics must never interfere with the downloader itself.
        }
    }
}

/**
 * Split a string on a delimiter
 * @param str   the string to split
 * @param delim the delimiter to split on
 * @return A vector of strings
 */
std::vector<std::string> split(std::string str, std::string delim) {
    size_t start = 0, end = 0, d_len = delim.length();
    std::vector<std::string> ret;

    if(d_len == 0) {
        ret.push_back(std::move(str));
        return ret;
    }

    while ((end = str.find(delim, start)) != std::string::npos) {
        ret.push_back(str.substr(start, end - start));
        start = end + d_len;
    }

    ret.push_back(str.substr(start));
    return ret;
}


void MapDownloader::dispatch_thread_function(MapDownloader *downloader) {
    CURLcode result = CURLcode::CURLE_FAILED_INIT;

    try {
        // Take a snapshot of immutable download configuration before entering the worker.
        std::string url_template;
        std::string map;
        std::string password;
        std::string game_engine;
        std::string server;
        std::filesystem::path output_file;
        {
            std::lock_guard lock(downloader->mutex);
            url_template = downloader->url_template;
            map = downloader->map;
            password = downloader->password;
            game_engine = downloader->game_engine;
            server = downloader->server;
            output_file = downloader->output_file;
        }

        char *map_urlencoded = curl_easy_escape(downloader->curl, map.c_str(), 0);
        char *password_urlencoded = curl_easy_escape(downloader->curl, password.c_str(), 0);
        if(!map_urlencoded || !password_urlencoded) {
            if(map_urlencoded) curl_free(map_urlencoded);
            if(password_urlencoded) curl_free(password_urlencoded);
            throw std::runtime_error("Failed to URL encode map download parameters");
        }
        const std::string password_urlencoded_string = password_urlencoded;

        auto replace_all_literal = [](std::string value, const std::string &needle, const std::string &replacement) {
            if(needle.empty()) {
                return value;
            }
            std::size_t position = 0;
            while((position = value.find(needle, position)) != std::string::npos) {
                value.replace(position, needle.size(), replacement);
                position += replacement.size();
            }
            return value;
        };
        std::string partial_url = url_template;
        partial_url = replace_all_literal(std::move(partial_url), "{map}", map_urlencoded);
        partial_url = replace_all_literal(std::move(partial_url), "{game}", game_engine);
        partial_url = replace_all_literal(std::move(partial_url), "{server}", server);
        partial_url = replace_all_literal(std::move(partial_url), "{password}", password_urlencoded);
        curl_free(map_urlencoded);
        curl_free(password_urlencoded);

        static const std::regex mirror_regex("\\{mirror<([^>]+)>\\}");
        std::smatch match;
        std::string mirror_str;
        const bool has_mirror_placeholder = std::regex_search(url_template, match, mirror_regex);
        if(has_mirror_placeholder) {
            mirror_str = match[1].str();
        }
        std::vector<std::string> mirrors = split(mirror_str, ",");
        if(mirrors.empty()) {
            mirrors.emplace_back("");
        }

        char curl_error_buffer[CURL_ERROR_SIZE] = {};
        const char *failure_reason = nullptr;
        long http_response_code = 0;
        long redirect_count = 0;
        long os_errno = 0;
        curl_off_t curl_downloaded_size = 0;
        std::string content_type;
        std::string effective_url;
        std::size_t callback_bytes_received = 0;
        bool header_checked = false;
        std::uint32_t header_head = 0;
        std::uint32_t header_foot = 0;
        std::uint32_t demo_head = 0;
        std::uint32_t demo_foot = 0;

        for(const auto &mirror : mirrors) {
            reset_map_download_callback_diagnostics();
            std::memset(curl_error_buffer, 0, sizeof(curl_error_buffer));
            failure_reason = nullptr;
            http_response_code = 0;
            redirect_count = 0;
            os_errno = 0;
            curl_downloaded_size = 0;
            content_type.clear();
            effective_url.clear();

            std::string url = partial_url;
            if(has_mirror_placeholder) {
                url = replace_all_literal(std::move(url), match[0].str(), mirror);
            }

            {
                std::lock_guard lock(downloader->mutex);
                if(downloader->status == DOWNLOAD_STAGE_CANCELING) {
                    result = CURLcode::CURLE_ABORTED_BY_CALLBACK;
                    failure_reason = "CANCELED";
                    break;
                }
                if(downloader->output_file_handle) {
                    std::fclose(downloader->output_file_handle);
                    downloader->output_file_handle = nullptr;
                }
                downloader->buffer_used = 0;
                downloader->written_size = 0;
                downloader->downloaded_size = 0;
                downloader->total_size = 0;
                downloader->status = DOWNLOAD_STAGE_STARTING;
                downloader->output_file_handle = std::fopen(output_file.string().c_str(), "wb");
                if(!downloader->output_file_handle) {
                    downloader->status = DOWNLOAD_STAGE_FAILED;
                    result = CURLcode::CURLE_WRITE_ERROR;
                    failure_reason = "TEMP_FILE_OPEN_FAILED";
                    break;
                }
                downloader->download_started = Clock::now();
            }

            const CURLcode error_buffer_result = curl_easy_setopt(downloader->curl, CURLOPT_ERRORBUFFER, curl_error_buffer);
            if(error_buffer_result != CURLcode::CURLE_OK) {
                result = error_buffer_result;
                failure_reason = "CURLOPT_ERRORBUFFER_FAILED";
                break;
            }

            const CURLcode set_url_result = curl_easy_setopt(downloader->curl, CURLOPT_URL, url.c_str());
            if(set_url_result != CURLcode::CURLE_OK) {
                result = set_url_result;
                failure_reason = "CURLOPT_URL_FAILED";
                break;
            }
            result = curl_easy_perform(downloader->curl);

            curl_easy_getinfo(downloader->curl, CURLINFO_RESPONSE_CODE, &http_response_code);
            curl_easy_getinfo(downloader->curl, CURLINFO_REDIRECT_COUNT, &redirect_count);
            curl_easy_getinfo(downloader->curl, CURLINFO_OS_ERRNO, &os_errno);
            curl_easy_getinfo(downloader->curl, CURLINFO_SIZE_DOWNLOAD_T, &curl_downloaded_size);

            char *content_type_ptr = nullptr;
            if(curl_easy_getinfo(downloader->curl, CURLINFO_CONTENT_TYPE, &content_type_ptr) == CURLcode::CURLE_OK && content_type_ptr) {
                content_type = content_type_ptr;
            }

            char *effective_url_ptr = nullptr;
            if(curl_easy_getinfo(downloader->curl, CURLINFO_EFFECTIVE_URL, &effective_url_ptr) == CURLcode::CURLE_OK && effective_url_ptr) {
                effective_url = effective_url_ptr;
                if(!password_urlencoded_string.empty()) {
                    effective_url = replace_all_literal(std::move(effective_url), password_urlencoded_string, "<redacted>");
                }
            }

            callback_bytes_received = map_download_callback_bytes_received;
            header_checked = map_download_header_checked;
            header_head = map_download_header_head;
            header_foot = map_download_header_foot;
            demo_head = map_download_demo_head;
            demo_foot = map_download_demo_foot;
            if(map_download_callback_failure_reason) {
                failure_reason = map_download_callback_failure_reason;
            }

            bool canceled = false;
            {
                std::lock_guard lock(downloader->mutex);
                canceled = downloader->status == DownloadStage::DOWNLOAD_STAGE_CANCELING;
            }
            if(canceled) {
                failure_reason = "CANCELED";
                break;
            }

            if(result == CURLcode::CURLE_OK) {
                break;
            }

            if(!failure_reason) {
                failure_reason = "CURL_ERROR";
            }
        }

        std::lock_guard lock(downloader->mutex);
        if(downloader->curl) {
            curl_easy_cleanup(downloader->curl);
            downloader->curl = nullptr;
        }

        if(result == CURLcode::CURLE_OK && downloader->output_file_handle) {
            bool write_ok = true;
            if(downloader->buffer_used != 0) {
                write_ok = std::fwrite(downloader->buffer.data(), downloader->buffer_used, 1, downloader->output_file_handle) == 1;
            }
            if(write_ok) {
                if(downloader->buffer_used > std::numeric_limits<std::size_t>::max() - downloader->written_size) {
                    write_ok = false;
                }
                else {
                    downloader->written_size += downloader->buffer_used;
                }
            }
            downloader->buffer_used = 0;
            downloader->buffer.clear();

            if(std::fclose(downloader->output_file_handle) != 0) {
                write_ok = false;
            }
            downloader->output_file_handle = nullptr;

            downloader->status = write_ok && downloader->written_size >= 0x800
                ? DownloadStage::DOWNLOAD_STAGE_COMPLETE
                : DownloadStage::DOWNLOAD_STAGE_FAILED;

            if(downloader->status == DownloadStage::DOWNLOAD_STAGE_FAILED && !failure_reason) {
                failure_reason = write_ok ? "RESPONSE_TOO_SMALL" : "FINAL_WRITE_FAILED";
            }
        }
        else {
            if(downloader->output_file_handle) {
                std::fclose(downloader->output_file_handle);
                downloader->output_file_handle = nullptr;
            }
            downloader->buffer_used = 0;
            downloader->buffer.clear();
            downloader->written_size = 0;
            downloader->status = DownloadStage::DOWNLOAD_STAGE_FAILED;
            if(!failure_reason) {
                failure_reason = "CURL_ERROR";
            }
        }

        if(downloader->status == DownloadStage::DOWNLOAD_STAGE_FAILED && !output_file.empty()) {
            write_map_download_debug_log(output_file,
                                         map,
                                         game_engine,
                                         failure_reason,
                                         result,
                                         curl_error_buffer,
                                         http_response_code,
                                         redirect_count,
                                         os_errno,
                                         curl_downloaded_size,
                                         callback_bytes_received,
                                         content_type,
                                         effective_url,
                                         header_checked,
                                         header_head,
                                         header_foot,
                                         demo_head,
                                         demo_foot);
            std::error_code ec;
            std::filesystem::remove(output_file, ec);
        }
    }
    catch(...) {
        std::lock_guard lock(downloader->mutex);
        if(downloader->curl) {
            curl_easy_cleanup(downloader->curl);
            downloader->curl = nullptr;
        }
        if(downloader->output_file_handle) {
            std::fclose(downloader->output_file_handle);
            downloader->output_file_handle = nullptr;
        }
        downloader->buffer_used = 0;
        downloader->buffer.clear();
        downloader->status = DownloadStage::DOWNLOAD_STAGE_FAILED;
        std::error_code ec;
        std::filesystem::remove(downloader->output_file, ec);
    }
}

std::size_t MapDownloader::get_download_speed() noexcept {
    std::lock_guard lock(this->mutex);
    // If we haven't started, return 0
    if(this->downloaded_size == 0) {
        return 0;
    }

    auto now = Clock::now();
    auto difference = now - this->download_started;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(difference).count();

    // Don't divide by zero
    if(ms <= 0) {
        return 0;
    }

    return (this->downloaded_size) / ms;
}

// Callback class
class MapDownloader::MapDownloaderCallback {
public:
    // When we've received data, put it in here
    static size_t write_callback(const std::byte *ptr, std::size_t size, std::size_t nmemb, MapDownloader *userdata) {
        if(!userdata || (!ptr && size != 0 && nmemb != 0) || (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size)) {
            set_map_download_callback_failure("INVALID_WRITE_CALLBACK_INPUT");
            return 0;
        }
        const std::size_t bytes = size * nmemb;
        if(bytes == 0) {
            return 0;
        }

        if(bytes > std::numeric_limits<std::size_t>::max() - map_download_callback_bytes_received) {
            map_download_callback_bytes_received = std::numeric_limits<std::size_t>::max();
        }
        else {
            map_download_callback_bytes_received += bytes;
        }

        std::lock_guard lock(userdata->mutex);

        // If we're canceling, stop cURL immediately.
        if(userdata->status == MapDownloader::DOWNLOAD_STAGE_CANCELING) {
            set_map_download_callback_failure("CANCELED");
            return 0;
        }
        if(!userdata->output_file_handle || userdata->buffer_used > userdata->buffer.size()) {
            set_map_download_callback_failure("OUTPUT_STATE_INVALID");
            userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
            return 0;
        }

        // Check if this is a bad download once we have enough bytes for a map header.
        std::byte header_data[0x800] = {};
        if(userdata->written_size == 0 && userdata->buffer_used < sizeof(header_data) && bytes >= sizeof(header_data) - userdata->buffer_used) {
            std::memcpy(header_data, userdata->buffer.data(), userdata->buffer_used);
            std::memcpy(header_data + userdata->buffer_used, ptr, sizeof(header_data) - userdata->buffer_used);

            const auto read_u32 = [](const std::byte *at) noexcept {
                std::uint32_t value = 0;
                std::memcpy(&value, at, sizeof(value));
                return value;
            };

            map_download_header_checked = true;
            map_download_header_head = read_u32(header_data);
            map_download_header_foot = read_u32(header_data + 0x7FC);
            map_download_demo_head = read_u32(header_data + 0x2C0);
            map_download_demo_foot = read_u32(header_data + 0x5F0);

            bool bad_header = true;
            if(map_download_header_head == 0x68656164 && map_download_header_foot == 0x666F6F74) {
                bad_header = false;
            }
            else if(map_download_demo_head == 0x45686564 && map_download_demo_foot == 0x47666F74) {
                bad_header = false;
            }
            if(bad_header) {
                set_map_download_callback_failure("BAD_MAP_HEADER");
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                return 0;
            }
        }

        userdata->status = MapDownloader::DOWNLOAD_STAGE_DOWNLOADING;

        if(bytes > userdata->buffer.size() - userdata->buffer_used) {
            if(userdata->buffer_used != 0 && std::fwrite(userdata->buffer.data(), userdata->buffer_used, 1, userdata->output_file_handle) != 1) {
                set_map_download_callback_failure("WRITE_FAILED");
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                return 0;
            }
            if(bytes != 0 && std::fwrite(ptr, bytes, 1, userdata->output_file_handle) != 1) {
                set_map_download_callback_failure("WRITE_FAILED");
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                return 0;
            }
            if(userdata->buffer_used > std::numeric_limits<std::size_t>::max() - bytes) {
                set_map_download_callback_failure("SIZE_OVERFLOW");
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                return 0;
            }
            const auto bytes_to_commit = userdata->buffer_used + bytes;
            if(userdata->written_size > std::numeric_limits<std::size_t>::max() - bytes_to_commit) {
                set_map_download_callback_failure("SIZE_OVERFLOW");
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                return 0;
            }
            userdata->written_size += bytes_to_commit;
            userdata->buffer_used = 0;
        }
        else {
            std::copy(ptr, ptr + bytes, userdata->buffer.data() + userdata->buffer_used);
            userdata->buffer_used += bytes;
        }

        return bytes;
    }

    // When progress has been made, record it here
    static int progress_callback(MapDownloader *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
        if(!clientp) {
            set_map_download_callback_failure("INVALID_PROGRESS_CALLBACK_INPUT");
            return 1;
        }

        std::lock_guard lock(clientp->mutex);
        if(clientp->status == MapDownloader::DOWNLOAD_STAGE_CANCELING) {
            set_map_download_callback_failure("CANCELED");
            return 1;
        }
        if(dlnow < 0 || dltotal < 0 ||
           static_cast<std::uintmax_t>(dlnow) > std::numeric_limits<std::size_t>::max() ||
           static_cast<std::uintmax_t>(dltotal) > std::numeric_limits<std::size_t>::max()) {
            set_map_download_callback_failure("PROGRESS_INVALID");
            clientp->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
            return 1;
        }
        clientp->downloaded_size = static_cast<std::size_t>(dlnow);
        clientp->total_size = static_cast<std::size_t>(dltotal);
        return 0;
    }
};

const std::string &MapDownloader::get_map() const noexcept {
    return this->map;
}

void MapDownloader::cancel() noexcept {
    bool cancel_requested = false;
    {
        std::lock_guard lock(this->mutex);
        if(this->status == DOWNLOAD_STAGE_CANCELED) {
            return;
        }
        if(this->status == DOWNLOAD_STAGE_NOT_STARTED) {
            this->status = DOWNLOAD_STAGE_CANCELED;
            return;
        }
        if(!this->is_finished_no_mutex()) {
            this->status = DownloadStage::DOWNLOAD_STAGE_CANCELING;
            cancel_requested = true;
        }
    }

    if(this->dispatch_thread.joinable()) {
        this->dispatch_thread.join();
    }

    if(cancel_requested) {
        std::lock_guard lock(this->mutex);
        this->status = DOWNLOAD_STAGE_CANCELED;
    }
}

// Set up stuff
void MapDownloader::download(const char *map, const char *output_file, const char *game_engine) {
    std::unique_lock lock(this->mutex);
    if(this->curl || this->dispatch_thread.joinable()) {
        throw std::exception();
    }

    this->map = map ? map : "";
    this->output_file = output_file ? output_file : "";
    this->game_engine = game_engine ? game_engine : "";
    auto &c_locale = std::locale::classic();
    for(char &c : this->map) {
        c = std::tolower(c, c_locale);
    }

    this->curl = curl_easy_init();
    if(!this->curl) {
        throw std::exception();
    }

    auto setopt = [this](CURLoption option, auto value) {
        return curl_easy_setopt(this->curl, option, value) == CURLcode::CURLE_OK;
    };

    const bool options_ok =
        setopt(CURLOPT_XFERINFOFUNCTION, MapDownloaderCallback::progress_callback) &&
        setopt(CURLOPT_WRITEFUNCTION, MapDownloaderCallback::write_callback) &&
        setopt(CURLOPT_WRITEDATA, this) &&
        setopt(CURLOPT_XFERINFODATA, this) &&
        setopt(CURLOPT_NOPROGRESS, 0L) &&
        setopt(CURLOPT_FAILONERROR, 1L) &&
        setopt(CURLOPT_FOLLOWLOCATION, 1L) &&
        setopt(CURLOPT_MAXREDIRS, 10L) &&
        setopt(CURLOPT_CONNECTTIMEOUT, 10L) &&
        setopt(CURLOPT_TIMEOUT, 90L) &&
        setopt(CURLOPT_USERAGENT, "Chimera MapDownloader/1.0");

    if(!options_ok) {
        curl_easy_cleanup(this->curl);
        this->curl = nullptr;
        throw std::exception();
    }

    this->status = MapDownloader::DOWNLOAD_STAGE_STARTING;
    this->buffer.resize(1024 * 1024);
    this->buffer_used = 0;
    this->downloaded_size = 0;
    this->written_size = 0;
    this->total_size = 0;

    try {
        this->dispatch_thread = std::thread(MapDownloader::dispatch_thread_function, this);
    }
    catch(...) {
        curl_easy_cleanup(this->curl);
        this->curl = nullptr;
        this->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
        throw;
    }
}

MapDownloader::DownloadStage MapDownloader::get_status() noexcept {
    std::lock_guard lock(this->mutex);
    return this->status;
}

std::size_t MapDownloader::get_downloaded_size() noexcept {
    std::lock_guard lock(this->mutex);
    return this->downloaded_size;
}

std::size_t MapDownloader::get_total_size() noexcept {
    std::lock_guard lock(this->mutex);
    return this->total_size;
}

bool MapDownloader::is_finished_no_mutex() const noexcept {
    return this->status == DOWNLOAD_STAGE_COMPLETE || this->status == DOWNLOAD_STAGE_FAILED || this->status == DOWNLOAD_STAGE_NOT_STARTED || this->status == DOWNLOAD_STAGE_CANCELED;
}

bool MapDownloader::is_finished() noexcept {
    auto m = this->mutex.try_lock();
    if(!m) {
        return false;
    }
    bool finished = this->is_finished_no_mutex();
    this->mutex.unlock();
    return finished;
}

void MapDownloader::set_server_info(const std::string &server, const std::string &password) noexcept {
    std::lock_guard lock(this->mutex);
    this->server = server;
    this->password = password;
}

MapDownloader::MapDownloader(const std::string &url_template) : url_template(url_template) {
    this->server = "";
    this->password = "";
    this->map = "";
}
MapDownloader::~MapDownloader() {
    this->cancel();
}
