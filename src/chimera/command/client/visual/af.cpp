// SPDX-License-Identifier: GPL-3.0-only

#include "../../command.hpp"
#include "../../../signature/hook.hpp"
#include "../../../signature/signature.hpp"
#include "../../../chimera.hpp"
#include "../../../output/output.hpp"
#include "../../../fix/af.hpp"

namespace Chimera {
    extern bool af_trial;

    bool af_command(int argc, const char **argv) {
        const bool client_af = get_chimera().feature_present("client_af");
        const bool client_demo = get_chimera().feature_present("client_demo");

        char *setting = nullptr;
        bool current_value = false;

        if(client_af) {
            setting = *reinterpret_cast<char **>(get_chimera().get_signature("af_is_enabled_sig").data() + 1);
            if(setting) {
                current_value = *setting != 0;
            }
        }
        else if(client_demo) {
            current_value = af_trial;
        }

        if(argc == 1) {
            const bool new_value = STR_TO_BOOL(argv[0]);

            // Always write an explicitly requested value to the real renderer flag.
            // The old command compared against a separate static cache initialized to
            // false, so the first "chimera_af 0" could report false without actually
            // disabling AF when Halo/config.txt had already enabled it.
            if(client_af && setting) {
                if(new_value && current_value) {
                    console_warning("Anisotropic Filtering is already enabled (likely via config.txt)!");
                }
                overwrite(setting, static_cast<char>(new_value));
                current_value = new_value;
            }
            else if(client_demo) {
                if(new_value && current_value) {
                    console_warning("Anisotropic Filtering is already enabled (likely via config.txt)!");
                }
                af_trial = new_value;
                current_value = new_value;
            }
        }

        console_output(BOOL_TO_STR(current_value));
        return true;
    }
}
