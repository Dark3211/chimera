// SPDX-License-Identifier: GPL-3.0-only

#include <cstdio>
#include <cstring>
#include <thread>
#include <memory>
#include <array>
#include <locale>

#define CURL_STATICLIB
#include <curl/curl.h>
#include <miniz.h>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <limits>
#include <algorithm>
#include <cstdint>

#include "map_downloader.hpp"

namespace {
    constexpr std::size_t MAP_HEADER_SIZE = 0x800;
    constexpr std::size_t HALONET_LISTING_MAX_BYTES = 8 * 1024 * 1024;

    std::uint32_t read_u32(const std::byte *at) noexcept {
        std::uint32_t value = 0;
        std::memcpy(&value, at, sizeof(value));
        return value;
    }

    bool valid_map_header(const std::byte *header_data) noexcept {
        if(!header_data) {
            return false;
        }
        if(read_u32(header_data) == 0x68656164 && read_u32(header_data + 0x7FC) == 0x666F6F74) {
            return true;
        }
        return read_u32(header_data + 0x2C0) == 0x45686564 && read_u32(header_data + 0x5F0) == 0x47666F74;
    }

    bool validate_map_file(const std::filesystem::path &path, std::size_t &file_size_out) {
        file_size_out = 0;

        std::error_code size_error;
        const auto file_size = std::filesystem::file_size(path, size_error);
        if(size_error || file_size < MAP_HEADER_SIZE || file_size > std::numeric_limits<std::size_t>::max()) {
            return false;
        }

        std::FILE *file = std::fopen(path.string().c_str(), "rb");
        if(!file) {
            return false;
        }

        std::array<std::byte, MAP_HEADER_SIZE> header{};
        const bool read_ok = std::fread(header.data(), 1, header.size(), file) == header.size();
        const bool close_ok = std::fclose(file) == 0;
        if(!read_ok || !close_ok || !valid_map_header(header.data())) {
            return false;
        }

        file_size_out = static_cast<std::size_t>(file_size);
        return true;
    }

    std::string lowercase_classic(std::string value) {
        const auto &locale = std::locale::classic();
        for(char &c : value) {
            c = std::tolower(c, locale);
        }
        return value;
    }

    char ascii_lower(char c) noexcept {
        if(c >= 'A' && c <= 'Z') {
            return static_cast<char>(c + ('a' - 'A'));
        }
        return c;
    }

    std::size_t find_ascii_case_insensitive(const std::string &value,
                                            const char *needle,
                                            std::size_t start = 0) noexcept {
        if(!needle) {
            return std::string::npos;
        }
        const std::size_t needle_length = std::strlen(needle);
        if(needle_length == 0) {
            return std::min(start, value.size());
        }
        if(start > value.size() || needle_length > value.size() - start) {
            return std::string::npos;
        }

        const std::size_t last = value.size() - needle_length;
        for(std::size_t i = start; i <= last; ++i) {
            bool matches = true;
            for(std::size_t j = 0; j < needle_length; ++j) {
                if(ascii_lower(value[i + j]) != ascii_lower(needle[j])) {
                    matches = false;
                    break;
                }
            }
            if(matches) {
                return i;
            }
        }
        return std::string::npos;
    }

    bool query_parameter_is_inv(const std::string &url, std::size_t position) noexcept {
        constexpr std::size_t FORMAT_LENGTH = 10; // "format=inv"
        if(position == std::string::npos) {
            return false;
        }
        const bool valid_start = position == 0 || url[position - 1] == '?' || url[position - 1] == '&';
        const std::size_t end = position + FORMAT_LENGTH;
        const bool valid_end = end == url.size() || url[end] == '&' || url[end] == '#';
        return valid_start && valid_end;
    }

    bool halonet_host_is_valid(const std::string &host) noexcept {
        if(host.size() < 16 || !host.starts_with("maps") || !host.ends_with(".halonet.net")) {
            return false;
        }
        for(std::size_t i = 4; i + 12 < host.size(); ++i) {
            if(host[i] < '0' || host[i] > '9') {
                return false;
            }
        }
        return true;
    }

    bool halonet_inv_url(const std::string &url) {
        const std::string lower = lowercase_classic(url);
        const auto scheme = lower.find("://");
        if(scheme == std::string::npos) {
            return false;
        }

        const auto host_start = scheme + 3;
        const auto path_start = lower.find('/', host_start);
        if(path_start == std::string::npos) {
            return false;
        }

        const std::string host = lower.substr(host_start, path_start - host_start);
        if(!halonet_host_is_valid(host)) {
            return false;
        }

        constexpr std::size_t LOCATOR_PATH_LENGTH = sizeof("/halonet/locator.php") - 1;
        if(lower.compare(path_start, LOCATOR_PATH_LENGTH, "/halonet/locator.php") != 0) {
            return false;
        }
        const std::size_t locator_path_end = path_start + LOCATOR_PATH_LENGTH;
        if(locator_path_end < lower.size() && lower[locator_path_end] != '?' && lower[locator_path_end] != '#') {
            return false;
        }

        const auto format = lower.find("format=inv", locator_path_end);
        return query_parameter_is_inv(lower, format);
    }

    bool replace_halonet_inv_format_with_zip(std::string &url) {
        const std::string lower = lowercase_classic(url);
        std::size_t position = lower.find("format=inv");
        while(position != std::string::npos) {
            if(query_parameter_is_inv(lower, position)) {
                url.replace(position, 10, "format=zip");
                return true;
            }
            position = lower.find("format=inv", position + 1);
        }
        return false;
    }

    bool get_halonet_origin(const std::string &locator_url, std::string &origin) {
        origin.clear();
        const std::string lower = lowercase_classic(locator_url);
        const auto scheme = lower.find("://");
        if(scheme == std::string::npos) {
            return false;
        }
        const std::string scheme_name = lower.substr(0, scheme);
        if(scheme_name != "http" && scheme_name != "https") {
            return false;
        }

        const auto host_start = scheme + 3;
        const auto path_start = lower.find('/', host_start);
        if(path_start == std::string::npos) {
            return false;
        }

        const std::string host = lower.substr(host_start, path_start - host_start);
        if(!halonet_host_is_valid(host)) {
            return false;
        }

        constexpr std::size_t LOCATOR_PATH_LENGTH = sizeof("/halonet/locator.php") - 1;
        if(lower.compare(path_start, LOCATOR_PATH_LENGTH, "/halonet/locator.php") != 0) {
            return false;
        }
        const std::size_t locator_path_end = path_start + LOCATOR_PATH_LENGTH;
        if(locator_path_end < lower.size() && lower[locator_path_end] != '?' && lower[locator_path_end] != '#') {
            return false;
        }

        origin = locator_url.substr(0, path_start);
        return true;
    }

    bool build_halonet_direct_zip_url(const std::string &locator_url,
                                      const std::string &map_urlencoded,
                                      std::string &direct_url) {
        std::string origin;
        if(!get_halonet_origin(locator_url, origin)) {
            direct_url.clear();
            return false;
        }

        direct_url = std::move(origin);
        direct_url += "/maps/";
        direct_url += map_urlencoded;
        direct_url += ".zip";
        return true;
    }

    struct StringDownloadContext {
        std::string data;
        bool overflow = false;
    };

    size_t bounded_string_write_callback(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) noexcept {
        if(!userdata || (!ptr && size != 0 && nmemb != 0) ||
           (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size)) {
            return 0;
        }

        const std::size_t bytes = size * nmemb;
        if(bytes == 0) {
            return 0;
        }

        auto *context = static_cast<StringDownloadContext *>(userdata);
        if(bytes > HALONET_LISTING_MAX_BYTES - std::min(context->data.size(), HALONET_LISTING_MAX_BYTES)) {
            context->overflow = true;
            return 0;
        }

        try {
            context->data.append(ptr, bytes);
        }
        catch(...) {
            context->overflow = true;
            return 0;
        }
        return bytes;
    }

    bool resolve_halonet_canonical_zip_url(CURL *curl,
                                           const std::string &locator_url,
                                           const std::string &requested_map,
                                           std::string &canonical_url) {
        canonical_url.clear();
        if(!curl || requested_map.empty()) {
            return false;
        }

        std::string origin;
        if(!get_halonet_origin(locator_url, origin)) {
            return false;
        }

        const std::string listing_url = origin + "/index.php?fulllist=y";
        StringDownloadContext context;
        context.data.reserve(512 * 1024);

        if(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bounded_string_write_callback) != CURLcode::CURLE_OK ||
           curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context) != CURLcode::CURLE_OK ||
           curl_easy_setopt(curl, CURLOPT_URL, listing_url.c_str()) != CURLcode::CURLE_OK) {
            return false;
        }

        const CURLcode listing_result = curl_easy_perform(curl);
        if(listing_result != CURLcode::CURLE_OK || context.overflow || context.data.empty()) {
            return false;
        }

        long response_code = 0;
        if(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code) != CURLcode::CURLE_OK || response_code != 200) {
            return false;
        }

        const std::string wanted = lowercase_classic(requested_map);
        std::size_t path_start = 0;
        while((path_start = find_ascii_case_insensitive(context.data, "/maps/", path_start)) != std::string::npos) {
            const std::size_t name_start = path_start + sizeof("/maps/") - 1;
            const std::size_t extension = find_ascii_case_insensitive(context.data, ".zip", name_start);
            if(extension == std::string::npos) {
                break;
            }
            if(extension - name_start > 512) {
                path_start = name_start;
                continue;
            }

            const std::string encoded_name = context.data.substr(name_start, extension - name_start);
            int decoded_length = 0;
            char *decoded = curl_easy_unescape(curl,
                                               encoded_name.c_str(),
                                               static_cast<int>(encoded_name.size()),
                                               &decoded_length);
            if(decoded) {
                std::string decoded_name;
                if(decoded_length > 0) {
                    decoded_name.assign(decoded, static_cast<std::size_t>(decoded_length));
                }
                curl_free(decoded);

                if(!decoded_name.empty() && lowercase_classic(decoded_name) == wanted) {
                    canonical_url = origin + context.data.substr(path_start, extension + 4 - path_start);
                    return true;
                }
            }

            path_start = extension + 4;
        }

        return false;
    }

    bool extract_halonet_zip_map(const std::filesystem::path &zip_path,
                                 const std::filesystem::path &output_path,
                                 const std::string &map,
                                 std::size_t &extracted_size) {
        extracted_size = 0;

        mz_zip_archive archive{};
        if(!mz_zip_reader_init_file(&archive, zip_path.string().c_str(), 0)) {
            return false;
        }

        bool success = false;
        do {
            const mz_uint file_count = mz_zip_reader_get_num_files(&archive);
            if(file_count == 0) {
                break;
            }

            const std::string expected_name = map + ".map";
            const int located_index = mz_zip_reader_locate_file(&archive, expected_name.c_str(), nullptr, MZ_ZIP_FLAG_IGNORE_PATH);
            if(located_index < 0) {
                break;
            }

            mz_zip_archive_file_stat stat{};
            if(!mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(located_index), &stat) ||
               stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported ||
               stat.m_uncomp_size < MAP_HEADER_SIZE ||
               stat.m_uncomp_size > static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
                break;
            }

            std::error_code remove_error;
            std::filesystem::remove(output_path, remove_error);
            if(!mz_zip_reader_extract_to_file(&archive, static_cast<mz_uint>(located_index), output_path.string().c_str(), 0)) {
                std::filesystem::remove(output_path, remove_error);
                break;
            }

            std::size_t validated_size = 0;
            if(!validate_map_file(output_path, validated_size) ||
               validated_size != static_cast<std::size_t>(stat.m_uncomp_size)) {
                std::filesystem::remove(output_path, remove_error);
                break;
            }

            extracted_size = validated_size;
            success = true;
        } while(false);

        if(!mz_zip_reader_end(&archive)) {
            success = false;
        }

        if(!success) {
            std::error_code remove_error;
            std::filesystem::remove(output_path, remove_error);
            extracted_size = 0;
        }
        return success;
    }

    struct ZipDownloadContext {
        std::FILE *file = nullptr;
    };

    size_t zip_write_callback(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) noexcept {
        if(!userdata || (!ptr && size != 0 && nmemb != 0) || (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size)) {
            return 0;
        }

        const std::size_t bytes = size * nmemb;
        if(bytes == 0) {
            return 0;
        }

        auto *context = static_cast<ZipDownloadContext *>(userdata);
        if(!context->file) {
            return 0;
        }
        return std::fwrite(ptr, 1, bytes, context->file);
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
    std::filesystem::path zip_temp_file;

    try {
        // Take a snapshot of immutable download configuration before entering the worker.
        std::string url_template;
        std::string map;
        std::string requested_map;
        std::string password;
        std::string game_engine;
        std::string server;
        std::filesystem::path output_file;
        {
            std::lock_guard lock(downloader->mutex);
            url_template = downloader->url_template;
            map = downloader->map;
            requested_map = downloader->requested_map;
            password = downloader->password;
            game_engine = downloader->game_engine;
            server = downloader->server;
            output_file = downloader->output_file;
        }

        char *map_urlencoded = curl_easy_escape(downloader->curl, map.c_str(), 0);
        char *requested_map_urlencoded = curl_easy_escape(downloader->curl, requested_map.c_str(), 0);
        char *password_urlencoded = curl_easy_escape(downloader->curl, password.c_str(), 0);
        if(!map_urlencoded || !requested_map_urlencoded || !password_urlencoded) {
            if(map_urlencoded) curl_free(map_urlencoded);
            if(requested_map_urlencoded) curl_free(requested_map_urlencoded);
            if(password_urlencoded) curl_free(password_urlencoded);
            throw std::runtime_error("Failed to URL encode map download parameters");
        }
        const std::string requested_map_urlencoded_string = requested_map_urlencoded;

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
        curl_free(requested_map_urlencoded);
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

        long http_response_code = 0;
        bool output_open_failed = false;
        bool halonet_inv_source = false;
        for(const auto &mirror : mirrors) {
            std::string url = partial_url;
            if(has_mirror_placeholder) {
                url = replace_all_literal(std::move(url), match[0].str(), mirror);
            }
            halonet_inv_source = halonet_inv_source || halonet_inv_url(url);

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
                    output_open_failed = true;
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
            http_response_code = 0;
            curl_easy_getinfo(downloader->curl, CURLINFO_RESPONSE_CODE, &http_response_code);

            bool canceled_now = false;
            {
                std::lock_guard lock(downloader->mutex);
                canceled_now = downloader->status == DownloadStage::DOWNLOAD_STAGE_CANCELING;
            }
            if(canceled_now) {
                break;
            }

            if(result == CURLcode::CURLE_OK) {
                break;
            }
        }

        bool inv_succeeded = false;
        bool inv_write_ok = false;
        std::size_t inv_written_size = 0;
        bool canceled = false;
        {
            std::lock_guard lock(downloader->mutex);
            canceled = downloader->status == DownloadStage::DOWNLOAD_STAGE_CANCELING;

            if(result == CURLcode::CURLE_OK && downloader->output_file_handle) {
                inv_write_ok = true;
                if(downloader->buffer_used != 0) {
                    inv_write_ok = std::fwrite(downloader->buffer.data(), downloader->buffer_used, 1, downloader->output_file_handle) == 1;
                }
                if(inv_write_ok) {
                    if(downloader->buffer_used > std::numeric_limits<std::size_t>::max() - downloader->written_size) {
                        inv_write_ok = false;
                    }
                    else {
                        downloader->written_size += downloader->buffer_used;
                    }
                }
                downloader->buffer_used = 0;

                if(std::fclose(downloader->output_file_handle) != 0) {
                    inv_write_ok = false;
                }
                downloader->output_file_handle = nullptr;
                inv_written_size = downloader->written_size;
                inv_succeeded = inv_write_ok && inv_written_size >= MAP_HEADER_SIZE;
            }
            else {
                if(downloader->output_file_handle) {
                    std::fclose(downloader->output_file_handle);
                    downloader->output_file_handle = nullptr;
                }
                downloader->buffer_used = 0;
                downloader->written_size = 0;
            }
        }

        const bool protocol_failure = result == CURLcode::CURLE_HTTP_RETURNED_ERROR && http_response_code != 0;
        const bool unavailable_inv_body = result == CURLcode::CURLE_OK && inv_write_ok && inv_written_size < MAP_HEADER_SIZE;
        const bool can_try_zip = !inv_succeeded && !canceled && !output_open_failed &&
                                 halonet_inv_source && (protocol_failure || unavailable_inv_body);

        if(!inv_succeeded) {
            std::error_code remove_error;
            std::filesystem::remove(output_file, remove_error);
        }

        bool zip_succeeded = false;
        std::size_t zip_extracted_size = 0;
        if(can_try_zip) {
            {
                std::lock_guard lock(downloader->mutex);
                if(downloader->status == DOWNLOAD_STAGE_CANCELING) {
                    canceled = true;
                }
                else {
                    downloader->status = DOWNLOAD_STAGE_STARTING;
                    downloader->buffer_used = 0;
                    downloader->written_size = 0;
                    downloader->downloaded_size = 0;
                    downloader->total_size = 0;
                }
            }

            if(!canceled) {
                zip_temp_file = output_file;
                zip_temp_file += ".zip";
                std::error_code remove_error;
                std::filesystem::remove(zip_temp_file, remove_error);

                auto try_zip_url = [&](const std::string &candidate_url) -> bool {
                    std::filesystem::remove(zip_temp_file, remove_error);
                    std::filesystem::remove(output_file, remove_error);

                    using FilePtr = std::unique_ptr<std::FILE, int (*)(std::FILE *)>;
                    FilePtr zip_file(std::fopen(zip_temp_file.string().c_str(), "wb"), &std::fclose);
                    if(!zip_file) {
                        result = CURLcode::CURLE_WRITE_ERROR;
                        return false;
                    }

                    ZipDownloadContext context{zip_file.get()};
                    const CURLcode set_write_result = curl_easy_setopt(downloader->curl, CURLOPT_WRITEFUNCTION, zip_write_callback);
                    const CURLcode set_data_result = curl_easy_setopt(downloader->curl, CURLOPT_WRITEDATA, &context);
                    const CURLcode set_url_result = curl_easy_setopt(downloader->curl, CURLOPT_URL, candidate_url.c_str());
                    if(set_write_result != CURLcode::CURLE_OK || set_data_result != CURLcode::CURLE_OK || set_url_result != CURLcode::CURLE_OK) {
                        if(set_write_result != CURLcode::CURLE_OK) {
                            result = set_write_result;
                        }
                        else {
                            result = set_data_result != CURLcode::CURLE_OK ? set_data_result : set_url_result;
                        }
                        return false;
                    }

                    {
                        std::lock_guard lock(downloader->mutex);
                        if(downloader->status == DOWNLOAD_STAGE_CANCELING) {
                            result = CURLcode::CURLE_ABORTED_BY_CALLBACK;
                            canceled = true;
                            return false;
                        }
                        downloader->download_started = Clock::now();
                        downloader->downloaded_size = 0;
                        downloader->total_size = 0;
                        downloader->status = DOWNLOAD_STAGE_DOWNLOADING;
                    }

                    result = curl_easy_perform(downloader->curl);
                    zip_file.reset();

                    {
                        std::lock_guard lock(downloader->mutex);
                        canceled = downloader->status == DownloadStage::DOWNLOAD_STAGE_CANCELING;
                    }
                    if(canceled) {
                        return false;
                    }

                    if(result == CURLcode::CURLE_OK && extract_halonet_zip_map(zip_temp_file, output_file, map, zip_extracted_size)) {
                        return true;
                    }

                    std::filesystem::remove(zip_temp_file, remove_error);
                    std::filesystem::remove(output_file, remove_error);
                    return false;
                };

                for(const auto &mirror : mirrors) {
                    std::string zip_url = partial_url;
                    if(has_mirror_placeholder) {
                        zip_url = replace_all_literal(std::move(zip_url), match[0].str(), mirror);
                    }
                    if(!replace_halonet_inv_format_with_zip(zip_url)) {
                        result = CURLcode::CURLE_URL_MALFORMAT;
                        break;
                    }

                    if(try_zip_url(zip_url)) {
                        zip_succeeded = true;
                        break;
                    }
                    if(canceled) {
                        break;
                    }

                    std::string direct_zip_url;
                    if(build_halonet_direct_zip_url(zip_url, requested_map_urlencoded_string, direct_zip_url) &&
                       try_zip_url(direct_zip_url)) {
                        zip_succeeded = true;
                        break;
                    }
                    if(canceled) {
                        break;
                    }

                    {
                        std::lock_guard lock(downloader->mutex);
                        if(downloader->status == DOWNLOAD_STAGE_CANCELING) {
                            canceled = true;
                        }
                        else {
                            downloader->downloaded_size = 0;
                            downloader->total_size = 0;
                            downloader->status = DOWNLOAD_STAGE_STARTING;
                        }
                    }
                    if(canceled) {
                        break;
                    }

                    std::string canonical_zip_url;
                    if(resolve_halonet_canonical_zip_url(static_cast<CURL *>(downloader->curl),
                                                         zip_url,
                                                         requested_map,
                                                         canonical_zip_url) &&
                       try_zip_url(canonical_zip_url)) {
                        zip_succeeded = true;
                        break;
                    }

                    {
                        std::lock_guard lock(downloader->mutex);
                        canceled = downloader->status == DownloadStage::DOWNLOAD_STAGE_CANCELING;
                    }
                    if(canceled) {
                        break;
                    }
                }

                std::filesystem::remove(zip_temp_file, remove_error);
            }
        }

        {
            std::lock_guard lock(downloader->mutex);
            if(downloader->curl) {
                curl_easy_cleanup(downloader->curl);
                downloader->curl = nullptr;
            }
            downloader->buffer_used = 0;
            downloader->buffer.clear();

            if(downloader->status != DownloadStage::DOWNLOAD_STAGE_CANCELING) {
                if(inv_succeeded) {
                    downloader->status = DownloadStage::DOWNLOAD_STAGE_COMPLETE;
                }
                else if(zip_succeeded) {
                    downloader->written_size = zip_extracted_size;
                    downloader->status = DownloadStage::DOWNLOAD_STAGE_COMPLETE;
                }
                else {
                    downloader->written_size = 0;
                    downloader->status = DownloadStage::DOWNLOAD_STAGE_FAILED;
                }
            }
        }

        if(!inv_succeeded && !zip_succeeded && !output_file.empty()) {
            std::error_code remove_error;
            std::filesystem::remove(output_file, remove_error);
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
        if(!zip_temp_file.empty()) {
            std::filesystem::remove(zip_temp_file, ec);
        }
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
            return 0;
        }
        const std::size_t bytes = size * nmemb;
        if(bytes == 0) {
            return 0;
        }

        std::lock_guard lock(userdata->mutex);

        // If we're canceling, stop cURL immediately.
        if(userdata->status == MapDownloader::DOWNLOAD_STAGE_CANCELING) {
            return 0;
        }
        if(!userdata->output_file_handle || userdata->buffer_used > userdata->buffer.size()) {
            userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
            return 0;
        }

        // Check if this is a bad download once we have enough bytes for a map header.
        if(userdata->written_size == 0 && userdata->buffer_used < MAP_HEADER_SIZE && bytes >= MAP_HEADER_SIZE - userdata->buffer_used) {
            std::array<std::byte, MAP_HEADER_SIZE> header_data{};
            std::memcpy(header_data.data(), userdata->buffer.data(), userdata->buffer_used);
            std::memcpy(header_data.data() + userdata->buffer_used, ptr, header_data.size() - userdata->buffer_used);

            if(!valid_map_header(header_data.data())) {
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                return 0;
            }
        }

        userdata->status = MapDownloader::DOWNLOAD_STAGE_DOWNLOADING;

        if(bytes > userdata->buffer.size() - userdata->buffer_used) {
            if(userdata->buffer_used != 0 && std::fwrite(userdata->buffer.data(), userdata->buffer_used, 1, userdata->output_file_handle) != 1) {
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                return 0;
            }
            if(bytes != 0 && std::fwrite(ptr, bytes, 1, userdata->output_file_handle) != 1) {
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                return 0;
            }
            if(userdata->buffer_used > std::numeric_limits<std::size_t>::max() - bytes) {
                userdata->status = MapDownloader::DOWNLOAD_STAGE_FAILED;
                return 0;
            }
            const auto bytes_to_commit = userdata->buffer_used + bytes;
            if(userdata->written_size > std::numeric_limits<std::size_t>::max() - bytes_to_commit) {
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
            return 1;
        }

        std::lock_guard lock(clientp->mutex);
        if(clientp->status == MapDownloader::DOWNLOAD_STAGE_CANCELING) {
            return 1;
        }
        if(dlnow < 0 || dltotal < 0 ||
           static_cast<std::uintmax_t>(dlnow) > std::numeric_limits<std::size_t>::max() ||
           static_cast<std::uintmax_t>(dltotal) > std::numeric_limits<std::size_t>::max()) {
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

    this->requested_map = map ? map : "";
    this->map = this->requested_map;
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
    this->requested_map = "";
}
MapDownloader::~MapDownloader() {
    this->cancel();
}
