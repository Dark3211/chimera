// SPDX-License-Identifier: GPL-3.0-only

#include "bitmaps.hpp"
#include "bitmap_lookup.hpp"
#include "../chimera.hpp"
#include "../signature/signature.hpp"
#include "../signature/hook.hpp"
#include "tag.hpp"
#include "tag_class.hpp"
#include "table.hpp"
#include <limits>

namespace Chimera {

    Bitmap *get_bitmap_tag(TagID tag_id) noexcept {
        auto *tag = get_tag(tag_id);
        if(!tag || tag->primary_class != TAG_CLASS_BITMAP) {
            return nullptr;
        }
        return reinterpret_cast<Bitmap *>(tag->data);
    }

    TextureTable &TextureTable::get_texture_table() noexcept {
        static auto *pc_texture_table = **reinterpret_cast<TextureTable ***>(get_chimera().get_signature("pc_texture_table_sig").data() + 1);
        return *pc_texture_table;
    }

    void *bitmap_covert_format(BitmapData *bitmap, bool force_32) noexcept {
        if(!bitmap || !bitmap->base_address || bitmap->pixels_size <= 0) {
            return nullptr;
        }

        const auto source_bytes = static_cast<std::size_t>(bitmap->pixels_size);
        auto allocate = [](std::size_t count, std::size_t element_size) -> void * {
            if(element_size == 0 || count > std::numeric_limits<std::size_t>::max() / element_size) {
                return nullptr;
            }
            return GlobalAlloc(0, count * element_size);
        };

        switch(bitmap->format) {
            case BITMAP_DATA_FORMAT_A8: {
                auto *pixel_data = reinterpret_cast<const std::uint8_t *>(bitmap->base_address);
                if(force_32) {
                    auto *output = reinterpret_cast<std::uint32_t *>(allocate(source_bytes, sizeof(std::uint32_t)));
                    if(!output) {
                        return nullptr;
                    }
                    for(std::size_t i = 0; i < source_bytes; i++) {
                        output[i] = 0x00FFFFFFu | (static_cast<std::uint32_t>(pixel_data[i]) << 24);
                    }
                    return output;
                }

                auto *output = reinterpret_cast<std::uint16_t *>(allocate(source_bytes, sizeof(std::uint16_t)));
                if(!output) {
                    return nullptr;
                }
                for(std::size_t i = 0; i < source_bytes; i++) {
                    output[i] = lookup_a8[pixel_data[i]];
                }
                return output;
            }

            case BITMAP_DATA_FORMAT_AY8: {
                auto *pixel_data = reinterpret_cast<const std::uint8_t *>(bitmap->base_address);
                if(force_32) {
                    auto *output = reinterpret_cast<std::uint32_t *>(allocate(source_bytes, sizeof(std::uint32_t)));
                    if(!output) {
                        return nullptr;
                    }
                    for(std::size_t i = 0; i < source_bytes; i++) {
                        const auto value = static_cast<std::uint32_t>(pixel_data[i]);
                        output[i] = (value << 24) | (value << 16) | (value << 8) | value;
                    }
                    return output;
                }

                auto *output = reinterpret_cast<std::uint16_t *>(allocate(source_bytes, sizeof(std::uint16_t)));
                if(!output) {
                    return nullptr;
                }
                for(std::size_t i = 0; i < source_bytes; i++) {
                    output[i] = lookup_ay8[pixel_data[i]];
                }
                return output;
            }

            case BITMAP_DATA_FORMAT_Y8: {
                auto *pixel_data = reinterpret_cast<const std::uint8_t *>(bitmap->base_address);
                auto *output = reinterpret_cast<std::uint32_t *>(allocate(source_bytes, sizeof(std::uint32_t)));
                if(!output) {
                    return nullptr;
                }
                for(std::size_t i = 0; i < source_bytes; i++) {
                    const auto value = static_cast<std::uint32_t>(pixel_data[i]);
                    output[i] = 0xFF000000u | (value << 16) | (value << 8) | value;
                }
                return output;
            }

            case BITMAP_DATA_FORMAT_A8Y8: {
                if((source_bytes & 1u) != 0) {
                    return nullptr;
                }
                const auto pixel_count = source_bytes / sizeof(std::uint16_t);
                auto *pixel_data = reinterpret_cast<const std::uint16_t *>(bitmap->base_address);
                auto *output = reinterpret_cast<std::uint32_t *>(allocate(pixel_count, sizeof(std::uint32_t)));
                if(!output) {
                    return nullptr;
                }
                for(std::size_t i = 0; i < pixel_count; i++) {
                    const auto value = static_cast<std::uint32_t>(pixel_data[i]);
                    const auto alpha = (value >> 8) & 0xFFu;
                    const auto luminance = value & 0xFFu;
                    output[i] = (alpha << 24) | (luminance << 16) | (luminance << 8) | luminance;
                }
                return output;
            }

            case BITMAP_DATA_FORMAT_P8_BUMP: {
                auto *pixel_data = reinterpret_cast<const std::uint8_t *>(bitmap->base_address);
                auto *output = reinterpret_cast<std::uint32_t *>(allocate(source_bytes, sizeof(std::uint32_t)));
                if(!output) {
                    return nullptr;
                }
                for(std::size_t i = 0; i < source_bytes; i++) {
                    output[i] = lookup_p8[pixel_data[i]];
                }
                return output;
            }

            default:
                return nullptr;
        }
    }

}
