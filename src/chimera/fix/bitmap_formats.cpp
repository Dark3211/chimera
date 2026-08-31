// SPDX-License-Identifier: GPL-3.0-only

#include <d3d9.h>

#include "bitmap_formats.hpp"
#include "../halo_data/bitmaps.hpp"
#include "../chimera.hpp"
#include "../config/ini.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"

namespace Chimera {
    extern "C" {
        void check_for_invalid_bitmap_format_asm() noexcept;
    }

    extern "C" void check_for_invalid_bitmap_format(BitmapData *bitmap) noexcept {
        if(!bitmap) {
            return;
        }

        // Convert unsupported monochrome formats before D3D9 texture creation.
        const bool force_32 = get_chimera().get_ini()->get_value_bool("debug.convert_monochrome_to_argb").value_or(false);
        const auto original_format = bitmap->format;
        const bool should_convert = force_32
            ? ((original_format >= BITMAP_DATA_FORMAT_A8 && original_format <= BITMAP_DATA_FORMAT_A8Y8) || original_format == BITMAP_DATA_FORMAT_P8_BUMP)
            : (original_format == BITMAP_DATA_FORMAT_A8 || original_format == BITMAP_DATA_FORMAT_AY8 || original_format == BITMAP_DATA_FORMAT_P8_BUMP);

        if(!should_convert) {
            return;
        }

        void *new_bitmap = bitmap_covert_format(bitmap, force_32);
        if(!new_bitmap) {
            return;
        }

        auto *old_bitmap = bitmap->base_address;
        bitmap->base_address = new_bitmap;

        if(force_32) {
            bitmap->format = original_format == BITMAP_DATA_FORMAT_Y8
                ? BITMAP_DATA_FORMAT_X8R8G8B8
                : BITMAP_DATA_FORMAT_A8R8G8B8;
        }
        else {
            bitmap->format = original_format == BITMAP_DATA_FORMAT_P8_BUMP
                ? BITMAP_DATA_FORMAT_A8R8G8B8
                : BITMAP_DATA_FORMAT_A8Y8;
        }

        if(old_bitmap) {
            GlobalFree(old_bitmap);
        }
    }

    void set_up_bitmap_formats() noexcept {
        // Support Y8 and A8Y8. These are supported natively by d3d9.
        auto *y8 = *reinterpret_cast<std::uint32_t **>(get_chimera().get_signature("supported_bitmap_formats").data() + 8) + 1;
        auto *a8y8 = *reinterpret_cast<std::uint32_t **>(get_chimera().get_signature("supported_bitmap_formats").data() + 8) + 3;

        overwrite(y8, D3DFMT_L8);
        overwrite(a8y8, D3DFMT_A8L8);

        // Translate A8, AY8 or P8 textures to something that is supported by d3d9.
        static Hook hook;
        write_jmp_call(get_chimera().get_signature("d3d_create_texture_sig").data() + 4, hook, reinterpret_cast<const void *>(check_for_invalid_bitmap_format_asm), nullptr);
    }
}
