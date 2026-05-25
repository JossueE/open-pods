#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bluetooth/aacp.hpp"

struct DeviceConnectedEvent {
    std::string mac;
    std::string name;
    uint16_t product_id = 0;
};

struct DeviceDisconnectedEvent {
    std::string mac;
};

struct AacpAppEvent {
    std::string mac;
    AACPEvent event;
};

struct AudioUnavailableEvent {};

struct AppEvent {
    using Payload = std::variant<
        DeviceConnectedEvent,
        DeviceDisconnectedEvent,
        AacpAppEvent,
        AudioUnavailableEvent
    >;

    Payload payload;

    static AppEvent device_connected(std::string mac, std::string name, uint16_t product_id);
    static AppEvent device_disconnected(std::string mac);
    static AppEvent aacp_event(std::string mac, AACPEvent event);
    static AppEvent audio_unavailable();
};

struct ControlCommandDeviceCommand {
    ControlCommandIdentifiers identifier;
    std::vector<uint8_t> value;
};

struct RenameDeviceCommand {
    std::string name;
};

struct RefreshBatteryDeviceCommand {};

/**
 * @brief Force the AirPods to route audio back to this host even when another
 *        peer (e.g. an iPhone) currently owns the audio source.
 */
struct ReclaimAudioDeviceCommand {};

struct DeviceCommand {
    using Payload = std::variant<
        ControlCommandDeviceCommand,
        RenameDeviceCommand,
        RefreshBatteryDeviceCommand,
        ReclaimAudioDeviceCommand
    >;

    Payload payload;

    static DeviceCommand control_command(
        ControlCommandIdentifiers identifier,
        std::vector<uint8_t> value
    );
    static DeviceCommand rename(std::string name);
    static DeviceCommand refresh_battery();
    static DeviceCommand reclaim_audio();
};

std::optional<std::vector<uint8_t>> serialize_app_event(const AppEvent& event);
std::optional<AppEvent> deserialize_app_event(const std::vector<uint8_t>& data);

std::optional<std::vector<uint8_t>> serialize_device_command(
    std::string_view mac,
    const DeviceCommand& command
);
std::optional<std::vector<uint8_t>> serialize_device_command_envelope(
    const std::pair<std::string, DeviceCommand>& command
);
std::optional<std::pair<std::string, DeviceCommand>> deserialize_device_command(
    const std::vector<uint8_t>& data
);
