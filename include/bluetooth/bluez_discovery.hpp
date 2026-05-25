#pragma once

#include "bluetooth/discovery.hpp"

#include <optional>
#include <string>

/**
 * @brief BlueZ-backed implementation of BluetoothDiscoveryBackend.
 */
class BluezDiscoveryBackend final : public BluetoothDiscoveryBackend {
public:
    std::vector<BluetoothDeviceInfo> devices() override;

    /**
     * @brief Reads the Bluetooth address of the default adapter (hci0) from BlueZ.
     * @return The MAC address as a colon-separated uppercase string, or std::nullopt if BlueZ is unreachable.
     */
    std::optional<std::string> adapter_address();

    /**
     * @brief Sets the Alias property of a BlueZ device (org.bluez.Device1.Alias).
     * @return True on success, false if BlueZ rejected the call or the device is unknown.
     */
    bool set_device_alias(const std::string& mac_address, const std::string& alias);
};
