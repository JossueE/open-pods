#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Stores user-configurable commands used by the application.
 */
struct Config {

    std::vector<std::string> volume_osd_command {};

    std::vector<std::string> volume_set_command {
        "wpctl",
        "set-volume",
        "@DEFAULT_AUDIO_SINK@",
        "{}"
    };

    /**
     * @brief Optional command used to restart the audio server when A2DP recovery is needed.
     * @note std::nullopt and an empty vector both disable automatic restart.
     * Example: {"systemctl", "--user", "restart", "wireplumber"}.
     */
    std::optional<std::vector<std::string>> restart_audio_server {};

    std::vector<std::string> battery_alert_command {
        "notify-send",
        "AirPods",
        "{}"
    };
};

class AppConfig {
public:
    AppConfig() = default;
    static AppConfig load();
    std::filesystem::path config_path() const;
    std::filesystem::path config_dir() const;
    bool run_template_cmd(const std::vector<std::string>& command, std::string_view value) const;

    const std::vector<std::string>& volume_osd_command() const { return config_.volume_osd_command; }
    const std::vector<std::string>& volume_set_command() const { return config_.volume_set_command; }
    const std::optional<std::vector<std::string>>& restart_audio_server() const { return config_.restart_audio_server; }
    const std::vector<std::string>& battery_alert_command() const { return config_.battery_alert_command; }

private:
    Config config_;
};
