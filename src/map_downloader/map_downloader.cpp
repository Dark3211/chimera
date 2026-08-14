// SPDX-License-Identifier: GPL-3.0-only

#include <cstdio>
#include <cstring>
#include <thread>

#define CURL_STATICLIB
#include <curl/curl.h>
#include <filesystem>
#include <regex>
#include <stdexcept>

#include "map_downloader.hpp"

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

        for(const auto &mirror : mirrors) {
            std::string url = partial_url;
            if(has_mirror_placeholder) {
                url = replace_all_literal(std::move(url), match[0].str(), mirror);
            }

            {
                std::lock_guard lock(downloader->mutex);
                if(downloader->status == DOWNLOAD_STAGE_CANCELING) {
                    result = CURLcode::CURLE_ABORTED_BY_CALLBACK;
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
                    break;
                }
                downloader->download_started = Clock::now();
            }

            const CURLcode set_url_result = curl_easy_setopt(downloader->curl, CURLOPT_URL, url.c_str());
            if(set_url_result != CURLcode::CURLE_OK) {
                result = set_url_result;
                break;
            }
            result = curl_easy_perform(downloader->curl);

            bool canceled = false;
            {
                std::lock_guard lock(downloader->mutex);
                canceled = downloader->status == DownloadStage::DOWNLOAD_STAGE_CANCELING;
            }
            if(canceled) {
                break;
            }

            if(result == CURLcode::CURLE_OK) {
                break;
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
                downloader->written_size += downloader->buffer_used;
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
        }

        if(downloader->status == DownloadStage::DOWNLOAD_STAGE_FAILED && !output_file.empty()) {
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
        return 0.0;
    }

    auto now = Clock::now();
    auto difference = now - this->download_started;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(difference).count();

    // Don't divide by zero
    if(ms <= 0) {
        return 0.0;
    }

    return (this->downloaded_size) / ms;
}

// Callback class
class MapDownloader::MapDownloaderCallback {
public:
    // When we've received data, put it in here
    static size_t write_callback(const std::byte *ptr, std::size_t, std::size_t nmemb, MapDownloader *userdata) {
        userdata->mutex.lock();

        // If we're canceling, stop
        if(userdata->status == MapDownloader::DOWNLOAD_STAGE_CANCELING) {
            userdata->mutex.unlock();
            return 0;
        }

        // Check if this is a bad download
        std::byte header_data[0x800];
        if(userdata->written_size == 0 && userdata->buffer_used < sizeof(header_data) && nmemb + userdata->buffer_used >= sizeof(header_data)) {
            std::memcpy(header_data, userdata->buffer.data(), userdata->buffer_used);
            std::memcpy(header_data + userdata->buffer_used, ptr, sizeof(header_data) - userdata->buffer_used);
            bool bad_header = true;
            if(*reinterpret_cast<std::uint32_t *>(header_data) == 0x68656164 && *reinterpret_cast<std::uint32_t *>(header_data + 0x7FC) == 0x666F6F74) {
                bad_header = false;
            }
            else if(*reinterpret_cast<std::uint32_t *>(header_data + 0x2C0) == 0x45686564 && *reinterpret_cast<std::uint32_t *>(header_data + 0x5F0) == 0x47666F74) {
                bad_header = false;
            }
            if(bad_header) {
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                userdata->mutex.unlock();
                return 0;
            }

        }

        userdata->status = MapDownloader::DOWNLOAD_STAGE_DOWNLOADING;

        if(userdata->buffer_used > userdata->buffer.size() || nmemb > userdata->buffer.size() - userdata->buffer_used) {
            if(userdata->buffer_used != 0 && std::fwrite(userdata->buffer.data(), userdata->buffer_used, 1, userdata->output_file_handle) != 1) {
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                userdata->mutex.unlock();
                return 0;
            }
            if(nmemb != 0 && std::fwrite(ptr, nmemb, 1, userdata->output_file_handle) != 1) {
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                userdata->mutex.unlock();
                return 0;
            }
            userdata->written_size += userdata->buffer_used + nmemb;
            userdata->buffer_used = 0;
        }
        else {
            std::copy(ptr, ptr + nmemb, userdata->buffer.data() + userdata->buffer_used);
            userdata->buffer_used += nmemb;
        }

        userdata->mutex.unlock();
        return nmemb;
    }

    // When progress has been made, record it here
    static int progress_callback(MapDownloader *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
        clientp->mutex.lock();
        clientp->downloaded_size = dlnow;
        clientp->total_size = dltotal;
        clientp->mutex.unlock();
        return 0;
    }
};

const std::string &MapDownloader::get_map() const noexcept {
    return this->map;
}

void MapDownloader::cancel() noexcept {
    {
        std::lock_guard lock(this->mutex);
        if(this->status == DOWNLOAD_STAGE_CANCELED) {
            return;
        }
        if(!this->is_finished_no_mutex()) {
            this->status = DownloadStage::DOWNLOAD_STAGE_CANCELING;
        }
    }

    if(this->dispatch_thread.joinable()) {
        this->dispatch_thread.join();
    }

    {
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
        setopt(CURLOPT_PROGRESSDATA, this) &&
        setopt(CURLOPT_NOPROGRESS, 0L) &&
        setopt(CURLOPT_FAILONERROR, 1L) &&
        setopt(CURLOPT_FOLLOWLOCATION, 1L) &&
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
    this->mutex.lock();
    auto return_value = this->status;
    this->mutex.unlock();
    return return_value;
}

std::size_t MapDownloader::get_downloaded_size() noexcept {
    this->mutex.lock();
    std::size_t return_value = this->downloaded_size;
    this->mutex.unlock();
    return return_value;
}

std::size_t MapDownloader::get_total_size() noexcept {
    this->mutex.lock();
    std::size_t return_value = this->total_size;
    this->mutex.unlock();
    return return_value;
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
