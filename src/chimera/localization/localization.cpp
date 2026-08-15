// SPDX-License-Identifier: GPL-3.0-only

#include "localization.hpp"
#include "../chimera.hpp"

#include <localization_strings.hpp>

namespace Chimera {
    const char *localize(const char *string, Language language) noexcept {
        if(!string) {
            return "";
        }
        auto language_index = static_cast<int>(language);
        if(language_index < 0 || language_index >= static_cast<int>(CHIMERA_LANGUAGE_COUNT)) {
            language_index = 0;
        }

        std::size_t begin = 0;
        std::size_t count = sizeof(LOCALIZATION_DATA) / sizeof(LOCALIZATION_DATA[0]);

        // Binary search
        const char *result = nullptr;
        while(begin < count) {
            std::size_t middle = (count + begin) / 2;
            int r = std::strcmp(string, LOCALIZATION_DATA[middle][0]);

            // Same
            if(r == 0) {
                result = LOCALIZATION_DATA[middle][language_index + 1];
                break;
            }

            // Greater than 0 (string comes after middle)
            else if(r > 0) {
                begin = middle + 1;
            }

            // Less than 0 (string comes before middle)
            else {
                count = middle;
            }
        }

        // If it's set, return result
        if(result && *result) {
            return result;
        }

        // Otherwise, return something so we can at least display something
        else {
            return string;
        }
    }
}
