// SPDX-License-Identifier: GPL-3.0-only

#include "../output/output.hpp"
#include "tag.hpp"
#include "../chimera.hpp"
#include "game_engine.hpp"
#include "../signature/signature.hpp"
#include <optional>
#include <limits>
#include <cstdint>

namespace Chimera {
    static constexpr std::uintptr_t TAG_DATA_SAFE_REGION_SIZE = 0x1700000;

    static Tag *validated_tag_array(TagDataHeader &header) noexcept {
        if(!header.tag_array) {
            return nullptr;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(get_tag_data_address());
        if(base > std::numeric_limits<std::uintptr_t>::max() - TAG_DATA_SAFE_REGION_SIZE) {
            return nullptr;
        }
        const auto end = base + TAG_DATA_SAFE_REGION_SIZE;
        const auto array = reinterpret_cast<std::uintptr_t>(header.tag_array);
        if(array < base || array >= end) {
            return nullptr;
        }

        const auto available = end - array;
        if(header.tag_count > available / sizeof(Tag)) {
            return nullptr;
        }
        return header.tag_array;
    }

    static bool validated_tag_path(const char *path) noexcept {
        if(!path) {
            return false;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(get_tag_data_address());
        if(base > std::numeric_limits<std::uintptr_t>::max() - TAG_DATA_SAFE_REGION_SIZE) {
            return false;
        }
        const auto end = base + TAG_DATA_SAFE_REGION_SIZE;
        auto current = reinterpret_cast<std::uintptr_t>(path);
        if(current < base || current >= end) {
            return false;
        }

        for(; current < end; current++) {
            if(*reinterpret_cast<const char *>(current) == 0) {
                return true;
            }
        }
        return false;
    }

    Tag *get_tag(const char *path, std::uint32_t tag_class) noexcept {
        if(!path) {
            return nullptr;
        }

        auto &tag_data_header = get_tag_data_header();
        auto *tag = validated_tag_array(tag_data_header);
        if(!tag) {
            return nullptr;
        }

        const auto tag_count = tag_data_header.tag_count;
        for(std::size_t i = 0; i < tag_count; i++) {
            if(tag[i].primary_class == tag_class && validated_tag_path(tag[i].path) && std::strcmp(path, tag[i].path) == 0) {
                return tag + i;
            }
        }
        return nullptr;
    }

    Tag *get_tag(const char *path, const char *tag_class) noexcept {
        if(!path || !tag_class) {
            return nullptr;
        }

        std::uint32_t tag_class_int = tag_class_from_string(tag_class);

        if(tag_class_int == TagClassInt::TAG_CLASS_NULL) {
            if(std::strlen(tag_class) == 4) {
                char buffer[5] = {};
                bool fill = false;
                for(std::size_t i = 0; i < 4; i++) {
                    if(tag_class[i] == 0x0 || fill) {
                        buffer[3 - i] = 0x20;
                        fill = true;
                    }
                    else {
                        buffer[3 - i] = tag_class[i];
                    }
                }
                std::memcpy(&tag_class_int, buffer, sizeof(tag_class_int));
            }
            else {
                return nullptr;
            }
        }

        return get_tag(path, tag_class_int);
    }

    Tag *get_tag(TagID tag_id) noexcept {
        if(tag_id.is_null()) {
            return nullptr;
        }

        auto &tag_data_header = get_tag_data_header();
        auto *tag_array = validated_tag_array(tag_data_header);
        const auto tag_count = tag_data_header.tag_count;
        if(tag_array && tag_id.index.index < tag_count) {
            return tag_array + tag_id.index.index;
        }
        return nullptr;
    }

    Tag *get_tag(std::size_t tag_index) noexcept {
        if(tag_index == 0xFFFFFFFF) {
            return nullptr;
        }

        auto &tag_data_header = get_tag_data_header();
        auto *tag_array = validated_tag_array(tag_data_header);
        const auto tag_count = tag_data_header.tag_count;
        if(tag_array && tag_index < tag_count) {
            return tag_array + tag_index;
        }
        return nullptr;
    }

    std::byte *get_tag_data_address() noexcept {
        static std::optional<std::byte *> address;
        if(!address.has_value()) {
            switch(game_engine()) {
                case GameEngine::GAME_ENGINE_DEMO:
                    address = reinterpret_cast<std::byte *>(0x4BF10000);
                    break;
                default:
                    address = reinterpret_cast<std::byte *>(0x40440000);
                    break;
            }
        }
        return address.value();
    }

    std::byte *get_tag_block_data(TagBlock *block, std::uint32_t index, std::uint32_t size) noexcept {
        if(!block || !block->address || index >= block->count) {
            return nullptr;
        }
        if(size != 0 && static_cast<std::size_t>(index) > std::numeric_limits<std::size_t>::max() / size) {
            return nullptr;
        }

        const auto offset = static_cast<std::size_t>(index) * static_cast<std::size_t>(size);
        const auto base = reinterpret_cast<std::uintptr_t>(block->address);
        if(offset > std::numeric_limits<std::uintptr_t>::max() - base) {
            return nullptr;
        }
        return reinterpret_cast<std::byte *>(base + offset);
    }
}
