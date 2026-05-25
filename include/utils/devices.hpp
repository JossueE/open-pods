#pragma once

#include "bluetooth/aacp.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace utils {

/**
 * @brief Path to the persisted device metadata file.
 */
std::filesystem::path get_devices_path();

/**
 * @brief Updates the entry for `mac` in the on-disk devices.json with the given
 *        AirPods information. Existing fields not represented by `info` are preserved.
 */
bool persist_device_information(const std::string& mac, const AirPodsInformation& info);

/**
 * @brief Updates only the `name` field of the device entry, useful after a rename.
 */
bool persist_device_name(const std::string& mac, const std::string& name);

} // namespace utils
