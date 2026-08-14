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

    void *bitmap_covert_format(BitmapData *bitmap) noexcept {
        if(!bitmap || !bitmap->base_address || bitmap->pixels_size <= 0) {
            return nullptr;
        }

        const auto pixel_count = static_cast<std::size_t>(bitmap->pixels_size);
        auto *pixel_data = reinterpret_cast<std::uint8_t *>(bitmap->base_address);
        void *converted_bitmap = nullptr;
        auto allocate_pixels = [pixel_count](std::size_t element_size) -> void * {
            if(element_size == 0 || pixel_count > std::numeric_limits<std::size_t>::max() / element_size) {
                return nullptr;
            }
            return GlobalAlloc(0, pixel_count * element_size);
        };

        switch(bitmap->format) {
            case BITMAP_DATA_FORMAT_A8: {
                auto *a8y8_bitmap = reinterpret_cast<std::uint16_t *>(allocate_pixels(sizeof(std::uint16_t)));
                if(!a8y8_bitmap) {
                    return nullptr;
                }
                for(std::size_t i = 0; i < pixel_count; i++) {
                    a8y8_bitmap[i] = lookup_a8[pixel_data[i]];
                }
                converted_bitmap = reinterpret_cast<void *>(a8y8_bitmap);
                break;
            }

            case BITMAP_DATA_FORMAT_AY8: {
                auto *a8y8_bitmap = reinterpret_cast<std::uint16_t *>(allocate_pixels(sizeof(std::uint16_t)));
                if(!a8y8_bitmap) {
                    return nullptr;
                }
                for(std::size_t i = 0; i < pixel_count; i++) {
                    a8y8_bitmap[i] = lookup_ay8[pixel_data[i]];
                }
                converted_bitmap = reinterpret_cast<void *>(a8y8_bitmap);
                break;
            }

            case BITMAP_DATA_FORMAT_P8_BUMP: {
                auto *uncomp_data = reinterpret_cast<std::uint32_t *>(allocate_pixels(sizeof(std::uint32_t)));
                if(!uncomp_data) {
                    return nullptr;
                }
                for(std::size_t i = 0; i < pixel_count; i++) {
                    uncomp_data[i] = lookup_p8[pixel_data[i]];
                }
                converted_bitmap = reinterpret_cast<void *>(uncomp_data);
                break;
            }

            default:
                break;
        }

        return converted_bitmap;
    }

}
