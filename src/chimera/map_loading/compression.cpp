// SPDX-License-Identifier: GPL-3.0-only

#include <vector>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <zstd.h>
#include <memory>

#include "../halo_data/map.hpp"
#include "compression.hpp"

namespace Chimera {
    template<typename T> static bool decompress_header(const std::byte *header_input, std::byte *header_output) {
        // Check to see if we can't even fit the header
        auto header_copy = *reinterpret_cast<const T *>(header_input);
        if(!header_copy.is_valid()) {
            return false;
        }

        // Figure out the new engine version
        auto new_engine_version = header_copy.engine_type;
        bool invader_compression = false;
        bool stores_uncompressed_size;
        switch(header_copy.engine_type) {
            case CacheFileEngine::CACHE_FILE_CUSTOM_EDITION_COMPRESSED:
                stores_uncompressed_size = false;
                new_engine_version = CacheFileEngine::CACHE_FILE_CUSTOM_EDITION;
                invader_compression = true;
                break;
            case CacheFileEngine::CACHE_FILE_RETAIL_COMPRESSED:
                stores_uncompressed_size = false;
                new_engine_version = CacheFileEngine::CACHE_FILE_RETAIL;
                invader_compression = true;
                break;
            case CacheFileEngine::CACHE_FILE_DEMO_COMPRESSED:
                stores_uncompressed_size = false;
                new_engine_version = CacheFileEngine::CACHE_FILE_DEMO;
                invader_compression = true;
                break;
            default:
                throw std::exception();
        }

        // Determine if the file size isn't set correctly
        if(invader_compression && header_copy.file_size < sizeof(header_copy)) {
            throw std::exception();
        }

        // Set the file size to either the original decompressed size or 0 (if needed) and the engine to the new thing
        header_copy.file_size = stores_uncompressed_size ? header_copy.file_size : 0;
        header_copy.engine_type = new_engine_version;

        // if demo, convert the header, otherwise copy the header
        if(new_engine_version == CacheFileEngine::CACHE_FILE_DEMO) {
            auto &demo_header = *reinterpret_cast<MapHeaderDemo *>(header_output);
            demo_header = {};
            std::memcpy(demo_header.name, header_copy.name, sizeof(demo_header.name));
            std::memcpy(demo_header.build, header_copy.build, sizeof(demo_header.build));
            demo_header.engine_type = header_copy.engine_type;
            demo_header.tag_data_offset = header_copy.tag_data_offset;
            demo_header.tag_data_size = header_copy.tag_data_size;
            demo_header.game_type = header_copy.game_type;
            demo_header.crc32 = header_copy.crc32;
            demo_header.head = 0x45686564;
            demo_header.foot = 0x47666F74;
        }
        else {
            *reinterpret_cast<T *>(header_output) = header_copy;
        }

        return true;
    }

    constexpr std::size_t HEADER_SIZE = sizeof(MapHeader);

    struct LowMemoryDecompression {
        /**
         * Callback for when a decompression occurs
         * @param decompressed_data decompressed data to write
         * @param size              size of decompressed data
         * @param user_data         user data to pass
         * @return                  true if successful
         */
        bool (*write_callback)(const std::byte *decompressed_data, std::size_t size, void *user_data) = nullptr;

        /**
         * Decompress the map file
         * @param path path to the map file
         */
        void decompress_map_file(const char *input, void *user_data) {
            std::FILE *input_file = std::fopen(input, "rb");
            if(!input_file) {
                throw std::exception();
            }

            try {
                const std::size_t total_size = std::filesystem::file_size(input);
                if(total_size < HEADER_SIZE) {
                    throw std::exception();
                }

                MapHeader header_input;
                if(std::fread(&header_input, sizeof(header_input), 1, input_file) != 1) {
                    throw std::exception();
                }

                std::byte header_output[HEADER_SIZE];
                if(!decompress_header<MapHeader>(reinterpret_cast<std::byte *>(&header_input), header_output)) {
                    if(!decompress_header<MapHeaderDemo>(reinterpret_cast<std::byte *>(&header_input), header_output)) {
                        throw std::exception();
                    }
                }

                if(!write_callback(header_output, sizeof(header_output), user_data)) {
                    throw std::exception();
                }

                ZSTD_DStream *raw_stream = ZSTD_createDStream();
                if(!raw_stream) {
                    throw std::exception();
                }
                std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> decompression_stream(raw_stream, &ZSTD_freeDStream);
                const std::size_t init = ZSTD_initDStream(decompression_stream.get());
                if(ZSTD_isError(init) || init == 0) {
                                        throw std::exception();
                }

                std::vector<std::byte> input_data(ZSTD_DStreamInSize());
                std::vector<std::byte> output_data(ZSTD_DStreamOutSize());
                std::size_t total_read = HEADER_SIZE;
                std::size_t input_pos = 0;
                std::size_t input_size = 0;
                bool stream_finished = false;

                while(!stream_finished) {
                    if(input_pos == input_size) {
                        if(total_read >= total_size) {
                            break;
                        }
                        const std::size_t to_read = std::min(input_data.size(), total_size - total_read);
                        if(std::fread(input_data.data(), 1, to_read, input_file) != to_read) {
                                                        throw std::exception();
                        }
                        total_read += to_read;
                        input_pos = 0;
                        input_size = to_read;
                    }

                    ZSTD_inBuffer input_buffer{input_data.data(), input_size, input_pos};
                    ZSTD_outBuffer output_buffer{output_data.data(), output_data.size(), 0};
                    const std::size_t q = ZSTD_decompressStream(decompression_stream.get(), &output_buffer, &input_buffer);
                    if(ZSTD_isError(q)) {
                                                throw std::exception();
                    }
                    input_pos = input_buffer.pos;

                    if(output_buffer.pos != 0 && !write_callback(reinterpret_cast<const std::byte *>(output_buffer.dst), output_buffer.pos, user_data)) {
                                                throw std::exception();
                    }

                    if(q == 0) {
                        // A frame ended. There may already be another frame in the input buffer.
                        if(input_pos == input_size && total_read >= total_size) {
                            stream_finished = true;
                        }
                    }
                    else if(input_pos == input_size && total_read >= total_size) {
                        // The stream says more compressed data is required, but the file ended.
                                                throw std::exception();
                    }
                }

                                if(total_read != total_size || !stream_finished) {
                    throw std::exception();
                }
            }
            catch(...) {
                std::fclose(input_file);
                throw;
            }

            std::fclose(input_file);
        }
    };

    std::size_t decompress_map_file(const char *input, const char *output) {
        struct OutputWriter {
            std::FILE *output_file;
            std::size_t output_position = 0;
        } output_writer = { std::fopen(output, "wb") };

        if(!output_writer.output_file) {
            throw std::exception();
        }

        LowMemoryDecompression decomp;
        decomp.write_callback = [](const std::byte *decompressed_data, std::size_t size, void *user_data) -> bool {
            auto &output_writer = *reinterpret_cast<OutputWriter *>(user_data);
            if(std::fwrite(decompressed_data, size, 1, reinterpret_cast<std::FILE *>(output_writer.output_file)) != 1) {
                return false;
            }
            output_writer.output_position += size;
            return true;
        };

        try {
            decomp.decompress_map_file(input, &output_writer);
        }
        catch(const std::exception &) {
            std::fclose(output_writer.output_file);
            throw;
        }
        std::fclose(output_writer.output_file);

        return output_writer.output_position;
    }

    std::size_t decompress_map_file(const char *input, std::byte *output, std::size_t output_size) {
        struct OutputWriter {
            std::byte *output;
            std::size_t output_size;
            std::size_t output_position = 0;
        } output_writer = { output, output_size };

        LowMemoryDecompression decomp;
        decomp.write_callback = [](const std::byte *decompressed_data, std::size_t size, void *user_data) -> bool {
            OutputWriter &output_writer = *reinterpret_cast<OutputWriter *>(user_data);
            std::size_t new_position = output_writer.output_position + size;
            if(new_position > output_writer.output_size) {
                return false;
            }
            std::copy(decompressed_data, decompressed_data + size, output_writer.output + output_writer.output_position);
            output_writer.output_position = new_position;
            return true;
        };

        decomp.decompress_map_file(input, &output_writer);
        return output_writer.output_position;
    }
}
