#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace utils {

/**
 * @brief Returns XDG_RUNTIME_DIR, or std::nullopt if it is not set.
 * @note Runtime sockets should not silently fall back to world-writable paths.
 */
std::optional<std::filesystem::path> runtime_dir();

/**
 * @brief Writes current AirPods battery values for external consumers.
 */
bool write_battery_env(
    std::optional<uint8_t> left,
    std::optional<uint8_t> right,
    std::optional<uint8_t> case_battery,
    std::optional<uint8_t> headphone
);

} // namespace utils
