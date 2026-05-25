#include "bluetooth/aacp_manager.hpp"

#include "utils/logging.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace {

std::string format_mac(
    uint8_t b0,
    uint8_t b1,
    uint8_t b2,
    uint8_t b3,
    uint8_t b4,
    uint8_t b5
)
{
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0')
        << std::setw(2) << static_cast<int>(b0) << ':'
        << std::setw(2) << static_cast<int>(b1) << ':'
        << std::setw(2) << static_cast<int>(b2) << ':'
        << std::setw(2) << static_cast<int>(b3) << ':'
        << std::setw(2) << static_cast<int>(b4) << ':'
        << std::setw(2) << static_cast<int>(b5);
    return out.str();
}

EarDetectionStatus ear_detection_status_from_byte(uint8_t value)
{
    switch (value) {
    case 0x00: return EarDetectionStatus::InEar;
    case 0x01: return EarDetectionStatus::OutOfEar;
    case 0x02: return EarDetectionStatus::InCase;
    case 0x03: return EarDetectionStatus::Disconnected;
    default:
        std::cerr << "Unknown ear detection status\n";
        return EarDetectionStatus::OutOfEar;
    }
}

std::optional<StemPressType> stem_press_type_from_byte(uint8_t value)
{
    switch (value) {
    case 0x05: return StemPressType::SingleTap;
    case 0x06: return StemPressType::DoubleTap;
    case 0x07: return StemPressType::TripleTap;
    case 0x08: return StemPressType::LongPress;
    default: return std::nullopt;
    }
}

std::optional<StemPressLocation> stem_press_location_from_byte(uint8_t value)
{
    switch (value) {
    case 0x01: return StemPressLocation::Left;
    case 0x02: return StemPressLocation::Right;
    default: return std::nullopt;
    }
}

} // namespace

AACPManager::AACPManager(std::shared_ptr<L2capTransport> transport)
    : transport_(std::move(transport))
{
}

AACPManager::~AACPManager()
{
    stop_receive_loop();
}

void AACPManager::record_opcode(uint8_t opcode)
{
    {
        std::lock_guard lock(state_.opcode_mutex);
        state_.opcode_history.push_back(opcode);
    }

    state_.opcode_cv.notify_all();
}

bool AACPManager::connect(std::string_view mac_address)
{
    stop_receive_loop();
    state_.airpods_mac = std::string(mac_address);

    if (!transport_) {
        std::cerr << "Cannot connect AACP: L2CAP transport is not configured\n";
        return false;
    }

    if (!transport_->connect(mac_address, PSM)) {
        return false;
    }

    start_receive_loop();
    return true;
}

void AACPManager::start_receive_loop()
{
    if (!transport_ || receive_running_) {
        return;
    }

    receive_running_ = true;
    receive_thread_ = std::thread([this]() {
        while (receive_running_) {
            auto packet = transport_->receive(2048);
            if (!packet) {
                break;
            }

            receive_packet(*packet);
        }

        AACPEventHandler handler;
        {
            std::lock_guard lock{state_.state_mutex};
            handler = state_.event_handler;
        }

        if (receive_running_ && handler) {
            handler(ConnectionLost{});
        }
    });
}

void AACPManager::stop_receive_loop()
{
    receive_running_ = false;

    if (transport_) {
        transport_->close();
    }

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
}

bool AACPManager::send_packet(const std::vector<uint8_t>& packet)
{
    if (!transport_) {
        std::cerr << "Cannot send AACP packet: L2CAP transport is not configured\n";
        return false;
    }

    std::lock_guard lock{send_mutex_};
    return transport_->send(packet);
}

bool AACPManager::send_data_packet(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> packet;
    packet.reserve(HEADER_BYTES.size() + data.size());
    packet.insert(packet.end(), HEADER_BYTES.begin(), HEADER_BYTES.end());
    packet.insert(packet.end(), data.begin(), data.end());

    return send_packet(packet);
}

bool AACPManager::set_event_channel(AACPEventHandler handler)
{
    std::lock_guard lock{state_.state_mutex};
    state_.event_handler = std::move(handler);
    return static_cast<bool>(state_.event_handler);
}

std::pair<std::optional<EarDetectionStatus>, std::optional<EarDetectionStatus>>
AACPManager::ear_detection_state()
{
    std::lock_guard lock{state_.state_mutex};
    return {state_.ear_detection_left, state_.ear_detection_right};
}

std::optional<AudioSource> AACPManager::last_audio_source()
{
    std::lock_guard lock{state_.state_mutex};
    return state_.audio_source;
}

bool AACPManager::subscribe_to_control_command(
    ControlCommandIdentifiers identifier,
    ControlCommandSubscriber subscriber
)
{
    if (!subscriber) {
        return false;
    }

    std::optional<std::vector<uint8_t>> initial_value;
    {
        std::lock_guard lock{state_.state_mutex};
        auto& subscribers = state_.control_command_subscribers[identifier];
        subscribers.push_back(subscriber);

        const auto status = std::find_if(
            state_.control_command_status_list.begin(),
            state_.control_command_status_list.end(),
            [identifier](const ControlCommandStatus& item) {
                return item.identifier == identifier;
            }
        );

        if (status != state_.control_command_status_list.end()) {
            initial_value = status->value;
        }
    }

    if (initial_value) {
        subscriber(*initial_value);
    }

    return true;
}

bool AACPManager::receive_packet(const std::vector<uint8_t>& packet)
{
    if (packet.size() < HEADER_BYTES.size()
        || !std::equal(HEADER_BYTES.begin(), HEADER_BYTES.end(), packet.begin())) {
        std::cerr << "Received AACP packet with invalid header\n";
        return false;
    }

    if (packet.size() < 5) {
        std::cerr << "Received AACP packet too short\n";
        return false;
    }

    const uint8_t opcode = packet[4];
    const auto payload_size = packet.size() - HEADER_BYTES.size();

    record_opcode(opcode);

    switch (opcode) {
    case opcodes::BATTERY_INFO: {
        if (payload_size < 3) {
            std::cerr << "Battery Info packet too short\n";
            return false;
        }

        const auto count = static_cast<std::size_t>(packet[6]);
        if (payload_size < 3 + count * 5) {
            std::cerr << "Battery Info packet length mismatch\n";
            return false;
        }

        std::vector<BatteryInfo> batteries;
        batteries.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            const auto base = HEADER_BYTES.size() + 3 + i * 5;

            std::optional<BatteryComponent> component;
            switch (packet[base]) {
            case 0x01: component = BatteryComponent::Headphone; break;
            case 0x02: component = BatteryComponent::RightBud; break;
            case 0x04: component = BatteryComponent::LeftBud; break;
            case 0x08: component = BatteryComponent::Case; break;
            default:
                std::cerr << "Unknown battery component\n";
                continue;
            }

            std::optional<BatteryStatus> status;
            switch (packet[base + 3]) {
            case 0x01: status = BatteryStatus::Charging; break;
            case 0x02: status = BatteryStatus::NotCharging; break;
            case 0x04: status = BatteryStatus::Disconnected; break;
            case 0x05: status = BatteryStatus::InUse; break;
            default:
                std::cerr << "Unknown battery status\n";
                continue;
            }

            batteries.push_back(BatteryInfo{
                .component = *component,
                .status = *status,
                .level = packet[base + 2],
            });
        }

        const auto primary = std::find_if(
            batteries.begin(),
            batteries.end(),
            [](const BatteryInfo& battery) {
                return battery.component == BatteryComponent::LeftBud
                    || battery.component == BatteryComponent::RightBud;
            }
        );
        const auto primary_component = primary != batteries.end()
            ? std::optional<BatteryComponent>(primary->component)
            : std::nullopt;

        AACPEventHandler handler;
        std::vector<BatteryInfo> snapshot;
        {
            std::lock_guard lock{state_.state_mutex};
            state_.battery_info = batteries;
            if (primary_component.has_value()) {
                state_.primary_pod = *primary_component;
            }
            snapshot = state_.battery_info;
            handler = state_.event_handler;
        }

        if (handler) {
            handler(snapshot);
        }

        return true;
    }

    case opcodes::CONTROL_COMMAND: {
        if (payload_size < 7) {
            std::cerr << "Control Command packet too short\n";
            return false;
        }

        const auto identifier = get_from_byte_control_command_identifier(packet[6]);
        if (!identifier.has_value()) {
            std::cerr << "Unknown Control Command identifier\n";
            return false;
        }

        const std::array<uint8_t, 4> value_bytes{
            packet[7],
            packet[8],
            packet[9],
            packet[10],
        };

        std::vector<uint8_t> value;
        const auto last_non_zero = std::find_if(
            value_bytes.rbegin(),
            value_bytes.rend(),
            [](uint8_t byte) {
                return byte != 0;
            }
        );

        if (last_non_zero == value_bytes.rend()) {
            value.push_back(0);
        } else {
            value.assign(value_bytes.begin(), last_non_zero.base());
        }

        ControlCommandStatus status{
            .identifier = *identifier,
            .value = value,
        };

        AACPEventHandler handler;
        std::vector<ControlCommandSubscriber> subscribers_snapshot;
        {
            std::lock_guard lock{state_.state_mutex};

            const auto existing = std::find_if(
                state_.control_command_status_list.begin(),
                state_.control_command_status_list.end(),
                [identifier](const ControlCommandStatus& item) {
                    return item.identifier == *identifier;
                }
            );

            if (existing != state_.control_command_status_list.end()) {
                existing->value = value;
            } else {
                state_.control_command_status_list.push_back(status);
            }

            if (*identifier == ControlCommandIdentifiers::OwnsConnection) {
                state_.owns = value_bytes[0] != 0;
            }

            if (const auto subscribers = state_.control_command_subscribers.find(*identifier);
                subscribers != state_.control_command_subscribers.end()) {
                subscribers_snapshot = subscribers->second;
            }

            handler = state_.event_handler;
        }

        for (const auto& subscriber : subscribers_snapshot) {
            subscriber(value);
        }

        if (handler) {
            handler(status);
        }

        return true;
    }

    case opcodes::EAR_DETECTION: {
        if (packet.size() < 8) {
            std::cerr << "Ear Detection packet too short\n";
            return false;
        }

        const auto primary_status = ear_detection_status_from_byte(packet[6]);
        const auto secondary_status = ear_detection_status_from_byte(packet[7]);

        AACPEventHandler handler;
        std::optional<EarDetectionStatus> old_left;
        std::optional<EarDetectionStatus> old_right;
        EarDetectionStatus left;
        EarDetectionStatus right;
        {
            std::lock_guard lock{state_.state_mutex};
            const bool right_is_primary = state_.primary_pod == BatteryComponent::RightBud;
            left = right_is_primary ? secondary_status : primary_status;
            right = right_is_primary ? primary_status : secondary_status;

            old_left = state_.ear_detection_left;
            old_right = state_.ear_detection_right;

            state_.ear_detection_left = left;
            state_.ear_detection_right = right;

            handler = state_.event_handler;
        }

        const auto label = [](EarDetectionStatus s) {
            switch (s) {
                case EarDetectionStatus::InEar: return "InEar";
                case EarDetectionStatus::OutOfEar: return "OutOfEar";
                case EarDetectionStatus::InCase: return "InCase";
                case EarDetectionStatus::Disconnected: return "Disconnected";
            }
            return "?";
        };
        OPENPODS_DEBUG("AACP EAR_DETECTION L=" << label(left) << " R=" << label(right));

        if (handler) {
            handler(EarDetection{
                .old_left = old_left,
                .old_right = old_right,
                .new_left = left,
                .new_right = right,
            });
        }

        return true;
    }

    case opcodes::CONVERSATION_AWARENESS: {
        if (packet.size() != 10) {
            std::cerr << "Conversation Awareness packet with unexpected length\n";
            return false;
        }

        const auto status = packet[9];

        AACPEventHandler handler;
        {
            std::lock_guard lock{state_.state_mutex};
            state_.conversational_awareness_status = status;
            handler = state_.event_handler;
        }

        if (handler) {
            handler(ConversationalAwareness{.status = status});
        }

        return true;
    }

    case opcodes::INFORMATION: {
        if (payload_size < 6) {
            std::cerr << "Information packet too short\n";
            return false;
        }

        // Skip the 4-byte header that Apple ships before the nul-separated string list.
        std::size_t cursor = HEADER_BYTES.size() + 4;
        const std::size_t end = packet.size();

        // Skip leading filler bytes until we hit the first nul terminator.
        while (cursor < end && packet[cursor] != 0x00) {
            ++cursor;
        }

        std::vector<std::string> strings;
        while (cursor < end) {
            while (cursor < end && packet[cursor] == 0x00) {
                ++cursor;
            }
            if (cursor >= end) {
                break;
            }
            const std::size_t start = cursor;
            while (cursor < end && packet[cursor] != 0x00) {
                ++cursor;
            }
            strings.emplace_back(
                reinterpret_cast<const char*>(packet.data() + start),
                cursor - start
            );
        }

        // First string in this packet is a header label that's not part of the
        // device information; drop it to mirror the Rust reference implementation.
        if (!strings.empty()) {
            strings.erase(strings.begin());
        }

        const auto take = [&](std::size_t index) -> std::string {
            return index < strings.size() ? strings[index] : std::string{};
        };

        AirPodsInformation info {
            .name = take(0),
            .model_number = take(1),
            .manufacturer = take(2),
            .serial_number = take(3),
            .version1 = take(4),
            .version2 = take(5),
            .hardware_revision = take(6),
            .updater_identifier = take(7),
            .left_serial_number = take(8),
            .right_serial_number = take(9),
            .version3 = take(10),
            .le_keys = AirPodsLEKeys{},
        };

        AACPEventHandler handler;
        AirPodsInformation snapshot;
        {
            std::lock_guard lock{state_.state_mutex};
            // Preserve already-collected proximity keys when re-receiving INFORMATION.
            if (state_.information.has_value()) {
                info.le_keys = state_.information->le_keys;
            }
            state_.information = info;
            snapshot = info;
            handler = state_.event_handler;
        }

        if (handler) {
            handler(snapshot);
        }

        return true;
    }

    case opcodes::PROXIMITY_KEYS_RSP: {
        if (payload_size < 4) {
            std::cerr << "Proximity Keys Response packet too short\n";
            return false;
        }

        const auto key_count = static_cast<std::size_t>(packet[6]);
        auto offset = HEADER_BYTES.size() + 3;

        std::optional<std::string> irk_hex;
        std::optional<std::string> enc_hex;

        for (std::size_t i = 0; i < key_count; ++i) {
            if (offset + 3 >= packet.size()) {
                std::cerr << "Proximity Keys Response truncated while reading key header\n";
                return false;
            }

            const auto key_type = get_from_byte_proximity_key_type(packet[offset]);
            const auto key_length = static_cast<std::size_t>(packet[offset + 2]);
            offset += 4;

            if (offset + key_length > packet.size()) {
                std::cerr << "Proximity Keys Response truncated while reading key data\n";
                return false;
            }

            std::string hex;
            hex.reserve(key_length * 2);
            for (std::size_t k = 0; k < key_length; ++k) {
                static constexpr char digits[] = "0123456789abcdef";
                const uint8_t byte = packet[offset + k];
                hex.push_back(digits[(byte >> 4) & 0x0F]);
                hex.push_back(digits[byte & 0x0F]);
            }

            if (key_type.has_value()) {
                switch (*key_type) {
                    case ProximityKeyType::Irk:
                        irk_hex = std::move(hex);
                        break;
                    case ProximityKeyType::EncKey:
                        enc_hex = std::move(hex);
                        break;
                }
            }
            offset += key_length;
        }

        AACPEventHandler handler;
        std::optional<AirPodsInformation> snapshot;
        {
            std::lock_guard lock{state_.state_mutex};
            if (!state_.information.has_value()) {
                state_.information = AirPodsInformation{};
            }
            if (irk_hex) {
                state_.information->le_keys.irk = std::move(*irk_hex);
            }
            if (enc_hex) {
                state_.information->le_keys.enc_key = std::move(*enc_hex);
            }
            snapshot = state_.information;
            handler = state_.event_handler;
        }

        if (handler && snapshot) {
            handler(*snapshot);
        }

        return true;
    }

    case opcodes::STEM_PRESS: {
        if (payload_size < 3) {
            std::cerr << "Stem Press packet too short\n";
            return false;
        }

        const auto press_type = stem_press_type_from_byte(packet[6]);
        if (!press_type.has_value()) {
            std::cerr << "Unknown Stem Press type 0x" << std::hex
                      << static_cast<unsigned>(packet[6]) << std::dec << '\n';
            return false;
        }

        std::optional<StemPressLocation> location;
        if (payload_size >= 4) {
            location = stem_press_location_from_byte(packet[7]);
        }

        const auto type_label = [](StemPressType t) {
            switch (t) {
                case StemPressType::SingleTap: return "SingleTap";
                case StemPressType::DoubleTap: return "DoubleTap";
                case StemPressType::TripleTap: return "TripleTap";
                case StemPressType::LongPress: return "LongPress";
            }
            return "?";
        };
        OPENPODS_DEBUG("AACP STEM_PRESS " << type_label(*press_type));

        AACPEventHandler handler;
        {
            std::lock_guard lock{state_.state_mutex};
            handler = state_.event_handler;
        }

        if (handler) {
            handler(StemPress{
                .type = *press_type,
                .location = location,
            });
        }

        return true;
    }

    case opcodes::AUDIO_SOURCE: {
        if (payload_size < 9) {
            std::cerr << "Audio Source packet too short\n";
            return false;
        }

        const auto audio_source_type = get_from_byte_audio_source_type(packet[12])
            .value_or(AudioSourceType::None);

        AudioSource audio_source{
            .mac = format_mac(packet[11], packet[10], packet[9], packet[8], packet[7], packet[6]),
            .type = audio_source_type,
        };

        AACPEventHandler handler;
        {
            std::lock_guard lock{state_.state_mutex};
            state_.audio_source = audio_source;
            handler = state_.event_handler;
        }

        if (handler) {
            handler(audio_source);
        }

        return true;
    }

    case opcodes::CONNECTED_DEVICES: {
        if (payload_size < 3) {
            std::cerr << "Connected Devices packet too short\n";
            return false;
        }

        const auto count = static_cast<std::size_t>(packet[6]);
        if (payload_size < 5 + count * 8) {
            std::cerr << "Connected Devices packet length mismatch\n";
            return false;
        }

        std::vector<ConnectedDevice> devices;
        devices.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            const auto base = HEADER_BYTES.size() + 5 + i * 8;
            devices.push_back(ConnectedDevice{
                .mac = format_mac(
                    packet[base],
                    packet[base + 1],
                    packet[base + 2],
                    packet[base + 3],
                    packet[base + 4],
                    packet[base + 5]
                ),
                .info1 = packet[base + 6],
                .info2 = packet[base + 7],
            });
        }

        AACPEventHandler handler;
        ConnectedDevices snapshot;
        {
            std::lock_guard lock{state_.state_mutex};
            state_.old_connected_devices = state_.connected_devices;
            state_.connected_devices = devices;
            snapshot = ConnectedDevices{
                .old_devices = state_.old_connected_devices,
                .new_devices = state_.connected_devices,
            };
            handler = state_.event_handler;
        }

        if (handler) {
            handler(snapshot);
        }

        return true;
    }

    case 0x11: {
        if (payload_size < 2) {
            return false;
        }

        const std::string message(packet.begin() + HEADER_BYTES.size() + 2, packet.end());
        if (message.find("SetOwnershipToFalse") != std::string::npos) {
            AACPEventHandler handler;
            {
                std::lock_guard lock{state_.state_mutex};
                handler = state_.event_handler;
            }
            if (handler) {
                handler(OwnershipToFalseRequest{});
            }
        }

        return true;
    }

    case opcodes::EQ_DATA: {
        if (payload_size < 16) {
            return false;
        }

        EqualizerData eq {};
        std::copy_n(packet.begin() + HEADER_BYTES.size() + 8, eq.data.size(), eq.data.begin());

        AACPEventHandler handler;
        {
            std::lock_guard lock{state_.state_mutex};
            handler = state_.event_handler;
        }

        if (handler) {
            handler(eq);
        }

        return true;
    }

    default: {
        std::ostringstream os;
        os << "Received unknown AACP opcode 0x" << std::hex << std::setw(2)
           << std::setfill('0') << static_cast<unsigned>(opcode);
        if (packet.size() > HEADER_BYTES.size()) {
            os << " payload[";
            for (std::size_t i = HEADER_BYTES.size(); i < packet.size(); ++i) {
                os << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned>(packet[i]);
                if (i + 1 < packet.size()) {
                    os << ' ';
                }
            }
            os << ']';
        }
        OPENPODS_DEBUG(os.str());
        std::cerr << "Received unknown AACP opcode 0x" << std::hex
                  << static_cast<unsigned>(opcode) << std::dec << '\n';
        return false;
    }
    }
}

bool AACPManager::wait_for_opcode(uint8_t expected_opcode, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(state_.opcode_mutex);
    const auto start_index = state_.opcode_history.size();

    return state_.opcode_cv.wait_for(lock, timeout, [this, start_index, expected_opcode] {
        return std::find(
            state_.opcode_history.begin() + static_cast<std::ptrdiff_t>(start_index),
            state_.opcode_history.end(),
            expected_opcode
        ) != state_.opcode_history.end();
    });
}

bool AACPManager::wait_for_any_opcode(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(state_.opcode_mutex);
    const auto start_index = state_.opcode_history.size();

    return state_.opcode_cv.wait_for(lock, timeout, [this, start_index] {
        return state_.opcode_history.size() > start_index;
    });
}

bool AACPManager::send_notification_request()
{
    return send_data_packet({
        opcodes::REQUEST_NOTIFICATIONS,
        0x00,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
    });
}

bool AACPManager::send_set_feature_flags_packet()
{
    return send_data_packet({
        opcodes::SET_FEATURE_FLAGS,
        0x00,
        0xFF,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    });
}

bool AACPManager::send_init_ext()
{
    return send_data_packet({
        0x4D,
        0x00,
        0x0E,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    });
}

bool AACPManager::send_handshake()
{
    return send_packet({
        0x00,
        0x00,
        0x04,
        0x00,
        0x01,
        0x00,
        0x02,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    });
}

bool AACPManager::send_proximity_keys_request(const std::vector<ProximityKeyType>& key_types)
{
    uint8_t key_flags = 0;
    for (const auto key_type : key_types) {
        key_flags |= static_cast<uint8_t>(key_type);
    }

    return send_data_packet({
        opcodes::PROXIMITY_KEYS_REQ,
        0x00,
        key_flags,
        0x00,
    });
}

bool AACPManager::send_rename_packet(std::string_view new_name)
{
    std::vector<uint8_t> packet;
    packet.reserve(5 + new_name.size());
    packet.push_back(opcodes::RENAME);
    packet.push_back(0x00);
    packet.push_back(0x01);
    packet.push_back(static_cast<uint8_t>(new_name.size()));
    packet.push_back(0x00);
    packet.insert(packet.end(), new_name.begin(), new_name.end());

    return send_data_packet(packet);
}

bool AACPManager::send_control_command(
    ControlCommandIdentifiers identifier,
    const std::vector<uint8_t>& data
)
{
    std::vector<uint8_t> packet;
    packet.reserve(7);
    packet.push_back(opcodes::CONTROL_COMMAND);
    packet.push_back(0x00);
    packet.push_back(static_cast<uint8_t>(identifier));

    for (std::size_t i = 0; i < 4; ++i) {
        packet.push_back(i < data.size() ? data[i] : 0x00);
    }

    return send_data_packet(packet);
}

bool AACPManager::send_ssl_request()
{
    return send_data_packet({
        0x29,
        0x00,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
    });
}
