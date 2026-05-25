#include "config/app_config.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

extern char** environ;

namespace {

std::string replace_all(std::string text, std::string_view from, std::string_view to)
{
    if (from.empty()) {
        return text;
    }

    std::string::size_type position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }

    return text;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

/**
 * @brief Parses a single TOML basic string starting at `position` (must point at the opening quote).
 * @return The decoded string and the new position past the closing quote, or std::nullopt on parse error.
 */
std::optional<std::pair<std::string, std::size_t>> parse_string(
    const std::string& source,
    std::size_t position
) {
    if (position >= source.size() || source[position] != '"') {
        return std::nullopt;
    }

    ++position;
    std::string out;
    while (position < source.size() && source[position] != '"') {
        if (source[position] == '\\' && position + 1 < source.size()) {
            const char esc = source[position + 1];
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                default: out.push_back(esc); break;
            }
            position += 2;
            continue;
        }
        out.push_back(source[position]);
        ++position;
    }

    if (position >= source.size()) {
        return std::nullopt;
    }

    return std::make_pair(std::move(out), position + 1);
}

/**
 * @brief Parses an inline TOML array of strings starting at `position` (must point at '[').
 */
std::optional<std::pair<std::vector<std::string>, std::size_t>> parse_string_array(
    const std::string& source,
    std::size_t position
) {
    if (position >= source.size() || source[position] != '[') {
        return std::nullopt;
    }

    ++position;
    std::vector<std::string> out;
    while (position < source.size()) {
        while (position < source.size()
               && std::isspace(static_cast<unsigned char>(source[position]))) {
            ++position;
        }
        if (position >= source.size()) {
            return std::nullopt;
        }
        if (source[position] == ']') {
            return std::make_pair(std::move(out), position + 1);
        }

        auto string = parse_string(source, position);
        if (!string) {
            return std::nullopt;
        }
        out.push_back(std::move(string->first));
        position = string->second;

        while (position < source.size()
               && std::isspace(static_cast<unsigned char>(source[position]))) {
            ++position;
        }
        if (position < source.size() && source[position] == ',') {
            ++position;
        }
    }

    return std::nullopt;
}

void apply_assignment(Config& config, std::string_view key, const std::string& source, std::size_t value_pos)
{
    while (value_pos < source.size()
           && std::isspace(static_cast<unsigned char>(source[value_pos]))) {
        ++value_pos;
    }

    if (value_pos >= source.size()) {
        return;
    }

    if (source[value_pos] == '[') {
        auto array = parse_string_array(source, value_pos);
        if (!array) {
            return;
        }

        if (key == "volume_osd_command") {
            config.volume_osd_command = std::move(array->first);
        } else if (key == "volume_set_command") {
            config.volume_set_command = std::move(array->first);
        } else if (key == "battery_alert_command") {
            config.battery_alert_command = std::move(array->first);
        } else if (key == "restart_audio_server") {
            if (array->first.empty()) {
                config.restart_audio_server = std::nullopt;
            } else {
                config.restart_audio_server = std::move(array->first);
            }
        }
    }
}

Config parse_config(const std::string& source)
{
    Config config;

    std::size_t position = 0;
    while (position < source.size()) {
        // Skip whitespace and comments
        while (position < source.size()
               && std::isspace(static_cast<unsigned char>(source[position]))) {
            ++position;
        }
        if (position >= source.size()) {
            break;
        }
        if (source[position] == '#') {
            while (position < source.size() && source[position] != '\n') {
                ++position;
            }
            continue;
        }
        // Section headers like [General] are tolerated but ignored — all known
        // keys live at the top level of the document.
        if (source[position] == '[') {
            while (position < source.size() && source[position] != '\n') {
                ++position;
            }
            continue;
        }

        // Read key
        std::size_t key_start = position;
        while (position < source.size()
               && source[position] != '='
               && source[position] != '\n') {
            ++position;
        }
        if (position >= source.size() || source[position] != '=') {
            // Skip malformed line
            while (position < source.size() && source[position] != '\n') {
                ++position;
            }
            continue;
        }

        std::string_view key = trim(std::string_view(source).substr(key_start, position - key_start));
        ++position; // skip '='

        apply_assignment(config, key, source, position);

        // Advance to the next newline so the outer loop picks up the following entry.
        while (position < source.size() && source[position] != '\n') {
            ++position;
        }
    }

    return config;
}

} // namespace

AppConfig AppConfig::load()
{
    AppConfig config;
    const auto path = config.config_path();

    std::ifstream file{path};
    if (!file) {
        return config;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    config.config_ = parse_config(buffer.str());
    return config;
}

std::filesystem::path AppConfig::config_path() const
{
    return config_dir() / "config.toml";
}

std::filesystem::path AppConfig::config_dir() const
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path {xdg} / "open-pods";
    }

    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path {home} / ".config" / "open-pods";
    }

    return std::filesystem::path {".config"} / "open-pods";
}

bool AppConfig::run_template_cmd(
    const std::vector<std::string>& command,
    std::string_view value
) const
{
    if (command.empty()) {
        return false;
    }

    std::vector<std::string> args;
    args.reserve(command.size());

    for (const auto& arg : command) {
        args.push_back(replace_all(arg, "{}", value));
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawn_result = posix_spawnp(
        &pid,
        argv.front(),
        nullptr,
        nullptr,
        argv.data(),
        environ
    );

    if (spawn_result != 0) {
        return false;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
