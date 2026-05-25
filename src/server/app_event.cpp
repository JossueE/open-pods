#include "server/app_event.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace {

using json = nlohmann::json;

json bytes_to_json(const std::vector<uint8_t>& bytes) {
    json result = json::array();
    for (uint8_t byte : bytes) {
        result.push_back(static_cast<unsigned int>(byte));
    }
    return result;
}

std::optional<uint8_t> byte_from_json(const json& value) {
    if (!value.is_number_unsigned() && !value.is_number_integer()) {
        return std::nullopt;
    }

    const int number = value.get<int>();
    if (number < 0 || number > 0xFF) {
        return std::nullopt;
    }

    return static_cast<uint8_t>(number);
}

std::optional<std::vector<uint8_t>> bytes_from_json(const json& value) {
    if (!value.is_array()) {
        return std::nullopt;
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(value.size());

    for (const auto& item : value) {
        const auto byte = byte_from_json(item);
        if (!byte) {
            return std::nullopt;
        }
        bytes.push_back(*byte);
    }

    return bytes;
}

std::optional<BatteryComponent> battery_component_from_json(const json& value) {
    const auto byte = byte_from_json(value);
    if (!byte) {
        return std::nullopt;
    }

    switch (*byte) {
        case 0x01: return BatteryComponent::Headphone;
        case 0x02: return BatteryComponent::RightBud;
        case 0x04: return BatteryComponent::LeftBud;
        case 0x08: return BatteryComponent::Case;
        default: return std::nullopt;
    }
}

std::optional<BatteryStatus> battery_status_from_json(const json& value) {
    const auto byte = byte_from_json(value);
    if (!byte) {
        return std::nullopt;
    }

    switch (*byte) {
        case 0x01: return BatteryStatus::Charging;
        case 0x02: return BatteryStatus::NotCharging;
        case 0x04: return BatteryStatus::Disconnected;
        case 0x05: return BatteryStatus::InUse;
        default: return std::nullopt;
    }
}

std::optional<EarDetectionStatus> ear_detection_from_json(const json& value) {
    const auto byte = byte_from_json(value);
    if (!byte) {
        return std::nullopt;
    }

    switch (*byte) {
        case 0x00: return EarDetectionStatus::InEar;
        case 0x01: return EarDetectionStatus::OutOfEar;
        case 0x02: return EarDetectionStatus::InCase;
        case 0x03: return EarDetectionStatus::Disconnected;
        default: return std::nullopt;
    }
}

std::optional<StemPressType> stem_press_type_from_json(const json& value) {
    const auto byte = byte_from_json(value);
    if (!byte) {
        return std::nullopt;
    }

    switch (*byte) {
        case 0x05: return StemPressType::SingleTap;
        case 0x06: return StemPressType::DoubleTap;
        case 0x07: return StemPressType::TripleTap;
        case 0x08: return StemPressType::LongPress;
        default: return std::nullopt;
    }
}

std::optional<StemPressLocation> stem_press_location_from_json(const json& value) {
    const auto byte = byte_from_json(value);
    if (!byte) {
        return std::nullopt;
    }

    switch (*byte) {
        case 0x01: return StemPressLocation::Left;
        case 0x02: return StemPressLocation::Right;
        default: return std::nullopt;
    }
}

bool optional_ear_detection_from_json(
    const json& value,
    std::optional<EarDetectionStatus>& out
) {
    if (value.is_null()) {
        out = std::nullopt;
        return true;
    }

    const auto status = ear_detection_from_json(value);
    if (!status) {
        return false;
    }

    out = *status;
    return true;
}

json optional_ear_detection_to_json(std::optional<EarDetectionStatus> status) {
    if (!status) {
        return nullptr;
    }
    return static_cast<unsigned int>(static_cast<uint8_t>(*status));
}

json optional_stem_location_to_json(std::optional<StemPressLocation> location) {
    if (!location) {
        return nullptr;
    }
    return static_cast<unsigned int>(static_cast<uint8_t>(*location));
}

bool optional_stem_location_from_json(
    const json& value,
    std::optional<StemPressLocation>& out
) {
    if (value.is_null()) {
        out = std::nullopt;
        return true;
    }

    const auto location = stem_press_location_from_json(value);
    if (!location) {
        return false;
    }

    out = *location;
    return true;
}

json battery_info_to_json(const BatteryInfo& battery) {
    return json{
        {"component", static_cast<unsigned int>(static_cast<uint8_t>(battery.component))},
        {"level", static_cast<unsigned int>(battery.level)},
        {"status", static_cast<unsigned int>(static_cast<uint8_t>(battery.status))}
    };
}

std::optional<BatteryInfo> battery_info_from_json(const json& value) {
    if (!value.is_object()
        || !value.contains("component")
        || !value.contains("level")
        || !value.contains("status")) {
        return std::nullopt;
    }

    const auto component = battery_component_from_json(value.at("component"));
    const auto level = byte_from_json(value.at("level"));
    const auto status = battery_status_from_json(value.at("status"));
    if (!component || !level || !status) {
        return std::nullopt;
    }

    return BatteryInfo{*component, *status, *level};
}

json connected_device_to_json(const ConnectedDevice& device) {
    return json{
        {"mac", device.mac},
        {"info1", static_cast<unsigned int>(device.info1)},
        {"info2", static_cast<unsigned int>(device.info2)}
    };
}

std::optional<ConnectedDevice> connected_device_from_json(const json& value) {
    if (!value.is_object()
        || !value.contains("mac")
        || !value.contains("info1")
        || !value.contains("info2")
        || !value.at("mac").is_string()) {
        return std::nullopt;
    }

    const auto info1 = byte_from_json(value.at("info1"));
    const auto info2 = byte_from_json(value.at("info2"));
    if (!info1 || !info2) {
        return std::nullopt;
    }

    return ConnectedDevice{
        value.at("mac").get<std::string>(),
        *info1,
        *info2
    };
}

json connected_devices_vector_to_json(const std::vector<ConnectedDevice>& devices) {
    json result = json::array();
    for (const ConnectedDevice& device : devices) {
        result.push_back(connected_device_to_json(device));
    }
    return result;
}

std::optional<std::vector<ConnectedDevice>> connected_devices_vector_from_json(
    const json& value
) {
    if (!value.is_array()) {
        return std::nullopt;
    }

    std::vector<ConnectedDevice> devices;
    devices.reserve(value.size());

    for (const auto& item : value) {
        auto device = connected_device_from_json(item);
        if (!device) {
            return std::nullopt;
        }
        devices.push_back(std::move(*device));
    }

    return devices;
}

json aacp_event_to_json(const AACPEvent& event) {
    if (const auto* batteries = std::get_if<std::vector<BatteryInfo>>(&event)) {
        json items = json::array();
        for (const BatteryInfo& battery : *batteries) {
            items.push_back(battery_info_to_json(battery));
        }
        return json{{"BatteryInfo", std::move(items)}};
    }

    if (const auto* command = std::get_if<ControlCommandStatus>(&event)) {
        return json{{"ControlCommand", {
            {"identifier", static_cast<unsigned int>(static_cast<uint8_t>(command->identifier))},
            {"value", bytes_to_json(command->value)}
        }}};
    }

    if (const auto* ear = std::get_if<EarDetection>(&event)) {
        return json{{"EarDetection", {
            {"old_left", optional_ear_detection_to_json(ear->old_left)},
            {"old_right", optional_ear_detection_to_json(ear->old_right)},
            {"new_left", optional_ear_detection_to_json(ear->new_left)},
            {"new_right", optional_ear_detection_to_json(ear->new_right)}
        }}};
    }

    if (const auto* awareness = std::get_if<ConversationalAwareness>(&event)) {
        return json{{"ConversationalAwareness", static_cast<unsigned int>(awareness->status)}};
    }

    if (const auto* source = std::get_if<AudioSource>(&event)) {
        return json{{"AudioSource", {
            {"mac", source->mac},
            {"type", static_cast<unsigned int>(static_cast<uint8_t>(source->type))}
        }}};
    }

    if (const auto* devices = std::get_if<ConnectedDevices>(&event)) {
        return json{{"ConnectedDevices", json::array({
            connected_devices_vector_to_json(devices->old_devices),
            connected_devices_vector_to_json(devices->new_devices)
        })}};
    }

    if (std::holds_alternative<OwnershipToFalseRequest>(event)) {
        return "OwnershipToFalseRequest";
    }

    if (const auto* stem = std::get_if<StemPress>(&event)) {
        return json{{"StemPress", json::array({
            static_cast<unsigned int>(static_cast<uint8_t>(stem->type)),
            optional_stem_location_to_json(stem->location)
        })}};
    }

    if (const auto* eq = std::get_if<EqualizerData>(&event)) {
        json data = json::array();
        for (uint8_t band : eq->data) {
            data.push_back(static_cast<unsigned int>(band));
        }
        return json{{"EqData", std::move(data)}};
    }

    if (std::holds_alternative<ConnectionLost>(event)) {
        return "ConnectionLost";
    }

    if (const auto* info = std::get_if<AirPodsInformation>(&event)) {
        return json{{"DeviceInfo", json{
            {"name", info->name},
            {"model_number", info->model_number},
            {"manufacturer", info->manufacturer},
            {"serial_number", info->serial_number},
            {"version1", info->version1},
            {"version2", info->version2},
            {"hardware_revision", info->hardware_revision},
            {"updater_identifier", info->updater_identifier},
            {"left_serial_number", info->left_serial_number},
            {"right_serial_number", info->right_serial_number},
            {"version3", info->version3},
            {"le_keys", json{
                {"irk", info->le_keys.irk},
                {"enc_key", info->le_keys.enc_key}
            }}
        }}};
    }

    return nullptr;
}

std::optional<AACPEvent> aacp_event_from_json(const json& value) {
    if (value.is_string()) {
        const std::string variant = value.get<std::string>();
        if (variant == "OwnershipToFalseRequest") {
            return AACPEvent{OwnershipToFalseRequest{}};
        }
        if (variant == "ConnectionLost") {
            return AACPEvent{ConnectionLost{}};
        }
        return std::nullopt;
    }

    if (!value.is_object() || value.size() != 1) {
        return std::nullopt;
    }

    const auto it = value.begin();
    const std::string variant = it.key();
    const json& payload = it.value();

    if (variant == "BatteryInfo") {
        if (!payload.is_array()) {
            return std::nullopt;
        }

        std::vector<BatteryInfo> batteries;
        batteries.reserve(payload.size());
        for (const auto& item : payload) {
            auto battery = battery_info_from_json(item);
            if (!battery) {
                return std::nullopt;
            }
            batteries.push_back(*battery);
        }
        return AACPEvent{std::move(batteries)};
    }

    if (variant == "ControlCommand") {
        if (!payload.is_object()
            || !payload.contains("identifier")
            || !payload.contains("value")) {
            return std::nullopt;
        }

        const auto identifier_byte = byte_from_json(payload.at("identifier"));
        if (!identifier_byte) {
            return std::nullopt;
        }

        const auto identifier = get_from_byte_control_command_identifier(*identifier_byte);
        const auto command_value = bytes_from_json(payload.at("value"));
        if (!identifier || !command_value) {
            return std::nullopt;
        }

        return AACPEvent{ControlCommandStatus{*identifier, *command_value}};
    }

    if (variant == "EarDetection") {
        if (!payload.is_object()) {
            return std::nullopt;
        }

        EarDetection event;
        if (!optional_ear_detection_from_json(payload.value("old_left", json(nullptr)), event.old_left)
            || !optional_ear_detection_from_json(payload.value("old_right", json(nullptr)), event.old_right)
            || !optional_ear_detection_from_json(payload.value("new_left", json(nullptr)), event.new_left)
            || !optional_ear_detection_from_json(payload.value("new_right", json(nullptr)), event.new_right)) {
            return std::nullopt;
        }
        return AACPEvent{event};
    }

    if (variant == "ConversationalAwareness") {
        const auto status = byte_from_json(payload);
        if (!status) {
            return std::nullopt;
        }
        return AACPEvent{ConversationalAwareness{*status}};
    }

    if (variant == "AudioSource") {
        if (!payload.is_object()
            || !payload.contains("mac")
            || !payload.contains("type")
            || !payload.at("mac").is_string()) {
            return std::nullopt;
        }

        const auto type_byte = byte_from_json(payload.at("type"));
        if (!type_byte) {
            return std::nullopt;
        }

        const auto type = get_from_byte_audio_source_type(*type_byte);
        if (!type) {
            return std::nullopt;
        }

        return AACPEvent{AudioSource{payload.at("mac").get<std::string>(), *type}};
    }

    if (variant == "ConnectedDevices") {
        if (!payload.is_array() || payload.size() != 2) {
            return std::nullopt;
        }

        auto old_devices = connected_devices_vector_from_json(payload.at(0));
        auto new_devices = connected_devices_vector_from_json(payload.at(1));
        if (!old_devices || !new_devices) {
            return std::nullopt;
        }

        return AACPEvent{ConnectedDevices{
            std::move(*old_devices),
            std::move(*new_devices)
        }};
    }

    if (variant == "StemPress") {
        if (!payload.is_array() || payload.size() != 2) {
            return std::nullopt;
        }

        const auto press_type = stem_press_type_from_json(payload.at(0));
        std::optional<StemPressLocation> location;
        if (!press_type || !optional_stem_location_from_json(payload.at(1), location)) {
            return std::nullopt;
        }

        return AACPEvent{StemPress{*press_type, location}};
    }

    if (variant == "EqData") {
        const auto bytes = bytes_from_json(payload);
        if (!bytes || bytes->size() != 8) {
            return std::nullopt;
        }

        EqualizerData eq;
        std::copy(bytes->begin(), bytes->end(), eq.data.begin());
        return AACPEvent{eq};
    }

    if (variant == "DeviceInfo") {
        if (!payload.is_object()) {
            return std::nullopt;
        }
        AirPodsInformation info {
            .name = payload.value("name", std::string{}),
            .model_number = payload.value("model_number", std::string{}),
            .manufacturer = payload.value("manufacturer", std::string{}),
            .serial_number = payload.value("serial_number", std::string{}),
            .version1 = payload.value("version1", std::string{}),
            .version2 = payload.value("version2", std::string{}),
            .hardware_revision = payload.value("hardware_revision", std::string{}),
            .updater_identifier = payload.value("updater_identifier", std::string{}),
            .left_serial_number = payload.value("left_serial_number", std::string{}),
            .right_serial_number = payload.value("right_serial_number", std::string{}),
            .version3 = payload.value("version3", std::string{}),
            .le_keys = AirPodsLEKeys{},
        };
        if (payload.contains("le_keys") && payload.at("le_keys").is_object()) {
            const json& keys = payload.at("le_keys");
            info.le_keys.irk = keys.value("irk", std::string{});
            info.le_keys.enc_key = keys.value("enc_key", std::string{});
        }
        return AACPEvent{std::move(info)};
    }

    return std::nullopt;
}

json app_event_to_json(const AppEvent& event) {
    if (const auto* connected = std::get_if<DeviceConnectedEvent>(&event.payload)) {
        return json{{"DeviceConnected", {
            {"mac", connected->mac},
            {"name", connected->name},
            {"product_id", connected->product_id}
        }}};
    }

    if (const auto* disconnected = std::get_if<DeviceDisconnectedEvent>(&event.payload)) {
        return json{{"DeviceDisconnected", disconnected->mac}};
    }

    if (const auto* aacp = std::get_if<AacpAppEvent>(&event.payload)) {
        return json{{"AACPEvent", json::array({aacp->mac, aacp_event_to_json(aacp->event)})}};
    }

    if (std::holds_alternative<AudioUnavailableEvent>(event.payload)) {
        return "AudioUnavailable";
    }

    return nullptr;
}

std::optional<AppEvent> app_event_from_json(const json& value) {
    if (value.is_string()) {
        if (value.get<std::string>() == "AudioUnavailable") {
            return AppEvent::audio_unavailable();
        }
        return std::nullopt;
    }

    if (!value.is_object() || value.size() != 1) {
        return std::nullopt;
    }

    const auto it = value.begin();
    const std::string variant = it.key();
    const json& payload = it.value();

    if (variant == "DeviceConnected") {
        if (!payload.is_object()
            || !payload.contains("mac")
            || !payload.contains("name")
            || !payload.contains("product_id")
            || !payload.at("mac").is_string()
            || !payload.at("name").is_string()
            || !payload.at("product_id").is_number_unsigned()) {
            return std::nullopt;
        }

        const auto product_id = payload.at("product_id").get<unsigned int>();
        if (product_id > 0xFFFF) {
            return std::nullopt;
        }

        return AppEvent::device_connected(
            payload.at("mac").get<std::string>(),
            payload.at("name").get<std::string>(),
            static_cast<uint16_t>(product_id)
        );
    }

    if (variant == "DeviceDisconnected") {
        if (!payload.is_string()) {
            return std::nullopt;
        }
        return AppEvent::device_disconnected(payload.get<std::string>());
    }

    if (variant == "AACPEvent") {
        if (!payload.is_array() || payload.size() != 2 || !payload.at(0).is_string()) {
            return std::nullopt;
        }

        auto event = aacp_event_from_json(payload.at(1));
        if (!event) {
            return std::nullopt;
        }

        return AppEvent::aacp_event(payload.at(0).get<std::string>(), std::move(*event));
    }

    return std::nullopt;
}

json device_command_to_json(const DeviceCommand& command) {
    if (const auto* control = std::get_if<ControlCommandDeviceCommand>(&command.payload)) {
        return json{{"ControlCommand", json::array({
            static_cast<unsigned int>(static_cast<uint8_t>(control->identifier)),
            bytes_to_json(control->value)
        })}};
    }

    if (const auto* rename = std::get_if<RenameDeviceCommand>(&command.payload)) {
        return json{{"Rename", rename->name}};
    }

    if (std::holds_alternative<RefreshBatteryDeviceCommand>(command.payload)) {
        return "RefreshBattery";
    }

    if (std::holds_alternative<ReclaimAudioDeviceCommand>(command.payload)) {
        return "ReclaimAudio";
    }

    return nullptr;
}

std::optional<DeviceCommand> device_command_from_json(const json& value) {
    if (value.is_string() && value.get<std::string>() == "RefreshBattery") {
        return DeviceCommand::refresh_battery();
    }

    if (value.is_string() && value.get<std::string>() == "ReclaimAudio") {
        return DeviceCommand::reclaim_audio();
    }

    if (!value.is_object() || value.size() != 1) {
        return std::nullopt;
    }

    const auto it = value.begin();
    const std::string variant = it.key();
    const json& payload = it.value();

    if (variant == "ControlCommand") {
        if (!payload.is_array() || payload.size() != 2) {
            return std::nullopt;
        }

        const auto identifier_byte = byte_from_json(payload.at(0));
        const auto command_value = bytes_from_json(payload.at(1));
        if (!identifier_byte || !command_value) {
            return std::nullopt;
        }

        const auto identifier = get_from_byte_control_command_identifier(*identifier_byte);
        if (!identifier) {
            return std::nullopt;
        }

        return DeviceCommand::control_command(*identifier, *command_value);
    }

    if (variant == "Rename") {
        if (!payload.is_string()) {
            return std::nullopt;
        }
        return DeviceCommand::rename(payload.get<std::string>());
    }

    return std::nullopt;
}

std::vector<uint8_t> dump_json_bytes(const json& value) {
    const std::string text = value.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::optional<json> parse_json_bytes(const std::vector<uint8_t>& data) {
    json value = json::parse(data.begin(), data.end(), nullptr, false);
    if (value.is_discarded()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

AppEvent AppEvent::device_connected(
    std::string mac,
    std::string name,
    uint16_t product_id
) {
    return AppEvent{
        DeviceConnectedEvent{
            std::move(mac),
            std::move(name),
            product_id
        }
    };
}

AppEvent AppEvent::device_disconnected(std::string mac) {
    return AppEvent{DeviceDisconnectedEvent{std::move(mac)}};
}

AppEvent AppEvent::aacp_event(std::string mac, AACPEvent event) {
    return AppEvent{
        AacpAppEvent{
            std::move(mac),
            std::move(event)
        }
    };
}

AppEvent AppEvent::audio_unavailable() {
    return AppEvent{AudioUnavailableEvent{}};
}

DeviceCommand DeviceCommand::control_command(
    ControlCommandIdentifiers identifier,
    std::vector<uint8_t> value
) {
    return DeviceCommand{
        ControlCommandDeviceCommand{
            identifier,
            std::move(value)
        }
    };
}

DeviceCommand DeviceCommand::rename(std::string name) {
    return DeviceCommand{RenameDeviceCommand{std::move(name)}};
}

DeviceCommand DeviceCommand::refresh_battery() {
    return DeviceCommand{RefreshBatteryDeviceCommand{}};
}

DeviceCommand DeviceCommand::reclaim_audio() {
    return DeviceCommand{ReclaimAudioDeviceCommand{}};
}

std::optional<std::vector<uint8_t>> serialize_app_event(const AppEvent& event) {
    const json value = app_event_to_json(event);
    if (value.is_null()) {
        return std::nullopt;
    }
    return dump_json_bytes(value);
}

std::optional<AppEvent> deserialize_app_event(const std::vector<uint8_t>& data) {
    const auto value = parse_json_bytes(data);
    if (!value) {
        return std::nullopt;
    }
    return app_event_from_json(*value);
}

std::optional<std::vector<uint8_t>> serialize_device_command(
    std::string_view mac,
    const DeviceCommand& command
) {
    const json command_json = device_command_to_json(command);
    if (command_json.is_null()) {
        return std::nullopt;
    }

    return dump_json_bytes(json::array({std::string{mac}, command_json}));
}

std::optional<std::vector<uint8_t>> serialize_device_command_envelope(
    const std::pair<std::string, DeviceCommand>& command
) {
    return serialize_device_command(command.first, command.second);
}

std::optional<std::pair<std::string, DeviceCommand>> deserialize_device_command(
    const std::vector<uint8_t>& data
) {
    const auto value = parse_json_bytes(data);
    if (!value || !value->is_array() || value->size() != 2 || !value->at(0).is_string()) {
        return std::nullopt;
    }

    auto command = device_command_from_json(value->at(1));
    if (!command) {
        return std::nullopt;
    }

    return std::make_pair(value->at(0).get<std::string>(), std::move(*command));
}
