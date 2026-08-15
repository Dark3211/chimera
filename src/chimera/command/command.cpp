// SPDX-License-Identifier: GPL-3.0-only

#include <cstring>
#include <memory>
#include "../localization/localization.hpp"
#include "../chimera.hpp"
#include "command.hpp"

namespace Chimera {
    CommandResult Command::call(std::size_t arg_count, const char **args) const noexcept {
        if(!this->p_function || (arg_count > 0 && !args)) {
            return CommandResult::COMMAND_RESULT_FAILED_ERROR;
        }
        if(!get_chimera().feature_present(this->feature())) {
            return CommandResult::COMMAND_RESULT_FAILED_FEATURE_NOT_AVAILABLE;
        }
        if(arg_count > this->max_args()) {
            return CommandResult::COMMAND_RESULT_FAILED_TOO_MANY_ARGUMENTS;
        }
        if(arg_count < this->min_args()) {
            return CommandResult::COMMAND_RESULT_FAILED_NOT_ENOUGH_ARGUMENTS;
        }

        try {
            return this->p_function(arg_count, args) ? CommandResult::COMMAND_RESULT_SUCCESS : CommandResult::COMMAND_RESULT_FAILED_ERROR;
        }
        catch(...) {
            return CommandResult::COMMAND_RESULT_FAILED_ERROR;
        }
    }

    CommandResult Command::call(const std::vector<std::string> &arguments) const noexcept {
        try {
            std::size_t arg_count = arguments.size();
            if(arg_count == 0) {
                return this->call(0, nullptr);
            }

            auto arguments_alloc(std::make_unique<const char *[]>(arg_count));
            for(std::size_t i = 0; i < arg_count; i++) {
                arguments_alloc[i] = arguments[i].c_str();
            }
            return this->call(arg_count, arguments_alloc.get());
        }
        catch(...) {
            return CommandResult::COMMAND_RESULT_FAILED_ERROR;
        }
    }

    std::vector<std::string> split_arguments(const char *command) noexcept {
        std::vector<std::string> arguments;
        if(!command) {
            return arguments;
        }

        try {
            bool in_quotes = false;
            bool escape_character = false;
            bool allow_empty_argument = false;
            std::size_t command_size = std::strlen(command);
            std::string argument;

            for(std::size_t i = 0; i < command_size; i++) {
                if(escape_character) {
                    escape_character = false;
                }
                else if(command[i] == '\\') {
                    escape_character = true;
                    continue;
                }
                else if(command[i] == '"') {
                    in_quotes = !in_quotes;
                    allow_empty_argument = true;
                    continue;
                }
                else if((command[i] == ' ' || command[i] == '\r' || command[i] == '\n' || command[i] == '#') && !in_quotes) {
                    if(!argument.empty() || allow_empty_argument) {
                        arguments.push_back(argument);
                        argument.clear();
                        allow_empty_argument = false;
                    }
                    if(command[i] == '#') {
                        break;
                    }
                    continue;
                }
                argument += command[i];
            }

            // Preserve a trailing backslash rather than silently dropping it.
            if(escape_character) {
                argument += '\\';
            }
            if(!argument.empty() || allow_empty_argument) {
                arguments.push_back(argument);
            }
        }
        catch(...) {
            arguments.clear();
        }
        return arguments;
    }

    std::string unsplit_arguments(const std::vector<std::string> &arguments) noexcept {
        try {
            std::string unsplit;
            for(std::size_t i = 0; i < arguments.size(); i++) {
                const std::string &argument = arguments[i];
                std::string argument_final;
                bool surround_with_quotes = false;

                for(const char &c : argument) {
                    switch(c) {
                        case '\\':
                        case '"':
                            argument_final += '\\';
                            break;
                        case '#':
                        case ' ':
                            surround_with_quotes = true;
                            break;
                        default:
                            break;
                    }
                    argument_final += c;
                }

                if(surround_with_quotes || argument.empty()) {
                    argument_final = std::string("\"") + argument_final + "\"";
                }
                unsplit += argument_final;
                if(i + 1 < arguments.size()) {
                    unsplit += " ";
                }
            }
            return unsplit;
        }
        catch(...) {
            return {};
        }
    }

    Command::Command(const char *name, const char *category, const char *feature, const char *help, CommandFunction function, bool autosave, std::size_t min_args, std::size_t max_args) :
        p_name(name), p_category(category), p_feature(feature), p_help(help), p_function(function), p_autosave(autosave), p_min_args(min_args), p_max_args(max_args) {}

    Command::Command(const char *name, const char *category, const char *feature, const char *help, CommandFunction function, bool autosave, std::size_t args) : Command(name, category, feature, help, function, autosave, args, args) {}

    void Chimera::get_all_commands() noexcept {
        #define ADD_COMMAND(name, category, feature, command_fn, autosave, ...) \
            extern bool command_fn(int, const char **); \
            static_assert(autosave == false || autosave == true, "autosave value is not a boolean"); \
            this->p_commands.emplace_back(name, category, feature, name "_command_help", command_fn, autosave, __VA_ARGS__);

        this->p_commands.clear();

        // Chimera-specific commands
        this->p_commands.emplace_back("chimera", localize("chimera_category_core"), "core", localize("chimera_command_help"), Chimera::chimera_command, false, 0, 1);
        this->p_commands.emplace_back("chimera_signature_info", localize("chimera_category_core"), "core", localize("chimera_signature_info_command_help"), Chimera::signature_info_command, false, 1, 1);
        ADD_COMMAND("chimera_about", "chimera_category_core", "core", about_command, true, 0, 0);
        ADD_COMMAND("chimera_language", "chimera_category_core", "core", language_command, true, 0, 1);
        ADD_COMMAND("chimera_chat_color_help", "chimera_category_custom_chat", "client_custom_chat", chat_color_help_command, true, 0, 1);
        ADD_COMMAND("chimera_chat_block_server_messages", "chimera_category_custom_chat", "client_custom_chat", chat_block_server_messages_command, true, 0, 1);
        ADD_COMMAND("chimera_chat_block_ips", "chimera_category_custom_chat", "client_custom_chat", chat_block_ips_command, true, 0, 1);

        // Debug
        ADD_COMMAND("chimera_budget", "chimera_category_debug", "client", budget_command, true, 0, 1);

        if(this->feature_present("core_devmode_retail")) {
            ADD_COMMAND("chimera_devmode", "chimera_category_debug", "core_devmode_retail", devmode_retail_command, true, 0, 1);
        }
        else {
            ADD_COMMAND("chimera_devmode", "chimera_category_debug", "core_devmode", devmode_command, true, 0, 1);
        }
        ADD_COMMAND("chimera_load_ui_map", "chimera_category_debug", "client", load_ui_map_command, false, 0, 0);
        ADD_COMMAND("chimera_player_info", "chimera_category_debug", "core", player_info_command, false, 0, 1);
        ADD_COMMAND("chimera_apply_damage", "chimera_category_debug", "core", apply_damage_command, false, 2, 5);
        ADD_COMMAND("chimera_block_damage", "chimera_category_debug", "core", block_damage_command, false, 0, 1);
        ADD_COMMAND("chimera_show_coordinates", "chimera_category_debug", "client", show_coordinates_command, true, 0, 1);
        ADD_COMMAND("chimera_show_fps", "chimera_category_debug", "client", show_fps_command, true, 0, 1);
        ADD_COMMAND("chimera_tps", "chimera_category_debug", "core", tps_command, false, 0, 1);
        ADD_COMMAND("chimera_teleport", "chimera_category_debug", "core", teleport_command, false, 1, 4);
        ADD_COMMAND("chimera_script_command_dump", "chimera_category_debug", "core", script_command_dump_command, false, 0, 0);
        ADD_COMMAND("chimera_send_chat_message", "chimera_category_debug", "client", send_chat_message_command, false, 2, 2);
        ADD_COMMAND("chimera_map_info", "chimera_category_debug", "client", map_info_command, false, 0, 0);
        ADD_COMMAND("chimera_debug_alternate_bump_attenuation", "chimera_category_debug", "client_custom_edition", map_config_alternate_bump_attenuation, false, 0, 0);
        ADD_COMMAND("chimera_debug_disable_bitmap_hud_scale_flags", "chimera_category_debug", "client_custom_edition", map_config_bitmap_hud_scale_flags, false, 0, 0);
        ADD_COMMAND("chimera_debug_gearbox_meters", "chimera_category_debug", "client_custom_edition", map_config_gearbox_meters, false, 0, 0);
        ADD_COMMAND("chimera_debug_gearbox_multitexture_blending", "chimera_category_debug", "client_custom_edition", map_config_gearbox_multitexture, false, 0, 0);
        ADD_COMMAND("chimera_debug_gearbox_bump_attenuation", "chimera_category_debug", "client_custom_edition", map_config_gearbox_bump_attenuation, false, 0, 0);
        ADD_COMMAND("chimera_debug_gearbox_chicago_multiply", "chimera_category_debug", "client_custom_edition", map_config_gearbox_chicago_multiply, false, 0, 0);
        ADD_COMMAND("chimera_debug_gearbox_shader_environment_types", "chimera_category_debug", "client_custom_edition", map_config_gearbox_shader_environment, false, 0, 0);
        ADD_COMMAND("chimera_debug_invert_detail_after_reflection", "chimera_category_debug", "client_custom_edition", map_config_detail_after_reflection, false, 0, 0);
        ADD_COMMAND("chimera_debug_old_widescreen_fix", "chimera_category_debug", "client_custom_edition", map_config_old_widescreen_fix, false, 0, 0);
        ADD_COMMAND("chimera_debug_block_multitexture_overlays", "chimera_category_debug", "client_custom_edition", map_config_block_multitexture_overlays, false, 0, 0);

        // Enhancements
        this->p_commands.emplace_back("chimera_block_all_bullshit", localize("chimera_category_enhancement"), "client", localize("chimera_block_all_bullshit_help"), Chimera::block_all_bullshit_command, false, 0, 0);
        ADD_COMMAND("chimera_block_extra_weapon", "chimera_category_enhancement", "client_block_extra_weapon", block_extra_weapon_command, false, 0, 0);
        ADD_COMMAND("chimera_unblock_all_extra_weapons", "chimera_category_enhancement", "client_block_extra_weapon", unblock_all_extra_weapons_command, false, 0, 0);
        ADD_COMMAND("chimera_set_name", "chimera_category_enhancement", "client", set_name_command, true, 0, 1);
        ADD_COMMAND("chimera_set_color", "chimera_category_enhancement", "client", set_color_command, true, 0, 1);
        ADD_COMMAND("chimera_throttle_fps", "chimera_category_enhancement", "client", throttle_fps_command, true, 0, 1);
        ADD_COMMAND("chimera_fp_reverb", "chimera_category_enhancement", "client_fp_reverb", fp_reverb_command, true, 0, 1);

        // Server
        ADD_COMMAND("chimera_spectate", "chimera_category_server", "client_spectate", spectate_command, false, 1, 1);
        ADD_COMMAND("chimera_spectate_next", "chimera_category_server", "client_spectate", spectate_next_command, false, 0, 0);
        ADD_COMMAND("chimera_spectate_previous", "chimera_category_server", "client_spectate", spectate_previous_command, false, 0, 0);
        ADD_COMMAND("chimera_spam_to_join", "chimera_category_server", "client", spam_to_join_command, true, 0, 1);
        ADD_COMMAND("chimera_spectate_team_only", "chimera_category_server", "client_spectate", spectate_team_only_command, true, 0, 1);
        ADD_COMMAND("chimera_delete_empty_weapons", "chimera_category_server", "core", delete_empty_weapons_command, true, 0, 1);
        ADD_COMMAND("chimera_player_list", "chimera_category_server", "core", player_list_command, false, 0, 0);
        ADD_COMMAND("chimera_block_equipment_rotation", "chimera_category_server", "core_null_rotation", block_equipment_rotation_command, true, 0, 1);
        ADD_COMMAND("chimera_allow_all_passengers", "chimera_category_server", "core_mtv", allow_all_passengers_command, true, 0, 1);
        ADD_COMMAND("chimera_master_server", "chimera_category_server", "core", master_server_command, true, 0, 4);

        // Visuals
        ADD_COMMAND("chimera_af", "chimera_category_visual", "client", af_command, true, 0, 1);
        ADD_COMMAND("chimera_block_auto_center", "chimera_category_visual", "client", block_auto_center_command, true, 0, 1);
        ADD_COMMAND("chimera_block_camera_shake", "chimera_category_visual", "client_camera_shake", block_camera_shake_command, true, 0, 1);
        ADD_COMMAND("chimera_block_gametype_indicator", "chimera_category_visual", "client_gametype_indicator", block_gametype_indicator_command, true, 0, 1);
        ADD_COMMAND("chimera_block_gametype_rules", "chimera_category_visual", "client_gametype_rules", block_gametype_rules_command, true, 0, 1);
        ADD_COMMAND("chimera_block_hold_f1", "chimera_category_visual", "client_hold_f1", block_hold_f1_command, true, 0, 1);
        ADD_COMMAND("chimera_block_letterbox", "chimera_category_visual", "client_letterbox", block_letterbox_command, true, 0, 1);
        ADD_COMMAND("chimera_block_loading_screen", "chimera_category_visual", "client_loading_screen", block_loading_screen_command, true, 0, 1);
        ADD_COMMAND("chimera_block_multitexture_overlays", "chimera_category_visual", "client_multitexture_overlays", block_multitexture_overlays_command, true, 0, 1);
        ADD_COMMAND("chimera_block_server_ip", "chimera_category_visual", "client_server_ip", block_server_ip_command, true, 0, 1);
        ADD_COMMAND("chimera_block_zoom_blur", "chimera_category_visual", "client_zoom_blur", block_zoom_blur_command, true, 0, 1);
        ADD_COMMAND("chimera_console_prompt_color", "chimera_category_visual", "client_console_prompt_color", console_prompt_color_command, true, 0, 3);
        ADD_COMMAND("chimera_fov", "chimera_category_visual", "client", fov_command, true, 0, 1);
        ADD_COMMAND("chimera_fov_vehicle", "chimera_category_visual", "client", fov_vehicle_command, true, 0, 1);
        ADD_COMMAND("chimera_fov_cinematic", "chimera_category_visual", "client", fov_cinematic_command, true, 0, 1);
        ADD_COMMAND("chimera_lock_fp_model_fov", "chimera_category_visual", "client", fov_fp_command, true, 0, 1);
        ADD_COMMAND("chimera_model_detail", "chimera_category_visual", "client_lod", model_detail_command, true, 0, 1);
        ADD_COMMAND("chimera_shrink_empty_weapons", "chimera_category_visual", "client", shrink_empty_weapons_command, true, 0, 1);
        ADD_COMMAND("chimera_simple_score_screen", "chimera_category_visual", "client_score_screen", simple_score_screen_command, true, 0, 1);
        ADD_COMMAND("chimera_split_screen_hud", "chimera_category_visual", "client_split_screen_hud", split_screen_hud_command, true, 0, 1);
        ADD_COMMAND("chimera_uncap_cinematic", "chimera_category_visual", "client_interpolate", uncap_cinematic_command, true, 0, 1);
        ADD_COMMAND("chimera_widescreen_fix", "chimera_category_visual", "client_widescreen", widescreen_fix_command, true, 0, 1);
        ADD_COMMAND("chimera_safe_zones", "chimera_category_visual", "client_widescreen", safe_zone_command, true, 0, 2);
        // ADD_COMMAND("chimera_meme_zone", "chimera_category_visual", "client_widescreen", meme_zone_command, true, 0, 1);

        // Lua
        ADD_COMMAND("chimera_lua_reload_scripts", "chimera_category_lua", "core", reload_scripts_command, false, 0, 0);

        // Mouse
        ADD_COMMAND("chimera_block_mouse_acceleration", "chimera_category_mouse", "client_mouse_acceleration", block_mouse_acceleration_command, true, 0, 1);
        ADD_COMMAND("chimera_mouse_sensitivity", "chimera_category_mouse", "client_mouse_sensitivity", mouse_sensitivity_command, true, 0, 2);

        // Controller
        ADD_COMMAND("chimera_aim_assist", "chimera_category_controller", "client", aim_assist_command, true, 0, 1);
        ADD_COMMAND("chimera_auto_uncrouch", "chimera_category_controller", "client_auto_uncrouch", auto_uncrouch_command, true, 0, 1);
        ADD_COMMAND("chimera_diagonals", "chimera_category_controller", "client_diagonals", diagonals_command, true, 0, 1);
        ADD_COMMAND("chimera_deadzones", "chimera_category_controller", "client_deadzones", deadzones_command, true, 0, 1);
        ADD_COMMAND("chimera_block_button_quotes", "chimera_category_controller", "client_quote_prompt", block_button_quotes_command, true, 0, 1);

        // Bookmark
        ADD_COMMAND("chimera_bookmark_list", "chimera_category_bookmark", "client", bookmark_list_command, false, 0, 0);
        ADD_COMMAND("chimera_bookmark_add", "chimera_category_bookmark", "client", bookmark_add_command, false, 0, 2);
        ADD_COMMAND("chimera_bookmark_connect", "chimera_category_bookmark", "client", bookmark_connect_command, false, 1, 1);
        ADD_COMMAND("chimera_bookmark_delete", "chimera_category_bookmark", "client", bookmark_delete_command, false, 0, 1);
        ADD_COMMAND("chimera_history_list", "chimera_category_bookmark", "client", history_list_command, false, 0, 0);
        ADD_COMMAND("chimera_history_connect", "chimera_category_bookmark", "client", history_connect_command, false, 1, 1);
    }
}
