#pragma once

#include <string>
#include <string_view>
#include <vector>

struct MainArgs {
    bool debug = false;
    bool version = false;
    bool waybar = false;
    bool waybar_watch = false;
    bool daemon = false;
    bool reclaim = false;
    std::string set_noise;
};

MainArgs parse_args(const std::vector<std::string_view>& args);
void print_usage(std::string_view program_name);
void check_bluetooth_config();
int run_waybar_mode(bool watch);
