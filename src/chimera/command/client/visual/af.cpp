// SPDX-License-Identifier: GPL-3.0-only

#include <cstring>

#include "../../command.hpp"
#include "../../../signature/hook.hpp"
#include "../../../signature/signature.hpp"
#include "../../../chimera.hpp"
#include "../../../output/output.hpp"
#include "../../../fix/af.hpp"

namespace Chimera {
    extern bool af_trial;
    
    bool af_command(int argc, const char **argv) {
        static bool active = false;
        if(argc == 1) {
            // Temporary A/B diagnostic: keep AF + detail sampling unchanged while
            // enabling/disabling only the environment bump-map sampling override.
            if(std::strcmp(argv[0], "bump_off") == 0) {
                set_environment_bump_sampling_enabled(false);
                console_output("bump %s", BOOL_TO_STR(get_environment_bump_sampling_enabled()));
                return true;
            }
            if(std::strcmp(argv[0], "bump_on") == 0) {
                set_environment_bump_sampling_enabled(true);
                console_output("bump %s", BOOL_TO_STR(get_environment_bump_sampling_enabled()));
                return true;
            }

            bool new_value = STR_TO_BOOL(argv[0]);
            if(new_value != active) {
                if(get_chimera().feature_present("client_af")) {
                    auto &setting = **reinterpret_cast<char **>(get_chimera().get_signature("af_is_enabled_sig").data() + 1);
                    if(new_value && setting) {
                        console_warning("Anisotropic Filtering is already enabled (likely via config.txt)!");
                    }
                    overwrite(&setting, static_cast<char>(new_value));
                    active = new_value;
                }
                else if(get_chimera().feature_present("client_demo")) {
                    if(new_value && af_trial) {
                        console_warning("Anisotropic Filtering is already enabled (likely via config.txt)!");
                    }
                    af_trial = new_value;
                    active = new_value;
                }
            }
        }
        console_output(BOOL_TO_STR(active));
        return true;
    }
}
