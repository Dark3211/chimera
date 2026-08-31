// SPDX-License-Identifier: GPL-3.0-only

#include "../../command.hpp"
#include "../../../signature/hook.hpp"
#include "../../../signature/signature.hpp"
#include "../../../chimera.hpp"
#include "../../../output/output.hpp"
#include "../../../localization/localization.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace Chimera {
    static bool parse_color_component(const char *text, float &value) noexcept {
        if(!text) return false;
        errno = 0;
        char *end = nullptr;
        value = std::strtof(text, &end);
        if(errno == ERANGE || end == text || !end || *end != 0 || !std::isfinite(value)) {
            return false;
        }
        value = std::clamp(value, 0.0F, 1.0F);
        return true;
    }

    bool console_prompt_color_command(int argc, const char **argv) {
        // Set the prompt color
        static ConsoleColor *color = nullptr;
        if(!color) {
            if(get_chimera().feature_present("client_console_prompt_color_demo")) {
                color = reinterpret_cast<ConsoleColor *>(*reinterpret_cast<std::byte **>(get_chimera().get_signature("console_prompt_color_demo_sig").data() + 2) - 4);
            }
            else {
                color = *reinterpret_cast<ConsoleColor **>(get_chimera().get_signature("console_prompt_color_sig").data() + 1);
            }
        }

        if(!color) {
            console_error("Unable to locate the console prompt color.");
            return false;
        }

        // If we have 3 arguments, try to get the color
        if(argc == 3) {
            float red = 0.0F, green = 0.0F, blue = 0.0F;
            if(!parse_color_component(argv[0], red) || !parse_color_component(argv[1], green) || !parse_color_component(argv[2], blue)) {
                console_error("Console prompt color values must be finite numbers.");
                return false;
            }
            color->r = red;
            color->g = green;
            color->b = blue;

            if(color->r == 0.0 && color->g == 1.0 && color->b == 1.0) {
                console_output(*color, "Vap!~");
            }
        }

        // If we don't, turn off the mod
        else if(argc > 0) {
            color->r = 1.0;
            color->g = 0.3;
            color->b = 1.0;
        }

        console_output(localize("chimera_console_prompt_color_command_output"), color->r, color->g, color->b);

        return true;
    }
}
