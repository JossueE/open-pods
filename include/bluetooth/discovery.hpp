#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

inline constexpr std::string_view AIRPODS_SERVICE_UUID {
    "74ec2172-0bad-4d01-8f77-997b2be0722a"
};

/**
 * @brief Minimal Bluetooth device data needed for AirPods discovery.
 */
struct BluetoothDeviceInfo {
    std::string address;
    std::string name;
    std::string modalias;
    bool connected = false;
    std::vector<std::string> uuids;
};

/**
 * @brief Abstract source of Bluetooth devices.
 * @note A BlueZ/DBus implementation can provide these records later.
 */
class BluetoothDiscoveryBackend {
public:
    virtual ~BluetoothDiscoveryBackend() = default;

    virtual std::vector<BluetoothDeviceInfo> devices() = 0;
};

/**
 * @brief Finds the first connected device exposing the AirPods AACP service UUID.
 */
inline std::optional<BluetoothDeviceInfo> find_connected_airpods(
    BluetoothDiscoveryBackend& backend
)
{
    auto equals_ignore_case = [](std::string_view left, std::string_view right) {
        return left.size() == right.size()
            && std::equal(
                left.begin(),
                left.end(),
                right.begin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a))
                        == std::tolower(static_cast<unsigned char>(b));
                }
            );
    };

    for (const auto& device : backend.devices()) {
        if (!device.connected) {
            continue;
        }

        const auto has_airpods_uuid = std::any_of(
            device.uuids.begin(),
            device.uuids.end(),
            [&](const std::string& uuid) {
                return equals_ignore_case(uuid, AIRPODS_SERVICE_UUID);
            }
        );

        if (has_airpods_uuid) {
            return device;
        }
    }

    return std::nullopt;
}
