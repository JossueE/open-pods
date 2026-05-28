#include "tui/tui_state.hpp"

#include <algorithm>
#include <chrono>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

namespace tui {

namespace {

constexpr std::size_t MAX_LOG_ENTRIES = 5;

void push_log(std::deque<LogEntry>& log, std::string message) {
    LogEntry entry {
        .timestamp = std::chrono::system_clock::now(),
        .message = std::move(message),
    };
    log.push_back(std::move(entry));
    while (log.size() > MAX_LOG_ENTRIES) {
        log.pop_front();
    }
}

std::optional<ListeningMode> decode_listening_mode(const std::vector<uint8_t>& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    switch (value.front()) {
    case 0x01: return ListeningMode::Off;
    case 0x02: return ListeningMode::NoiseCancellation;
    case 0x03: return ListeningMode::Transparency;
    case 0x04: return ListeningMode::Adaptive;
    default: return std::nullopt;
    }
}

const char* listening_mode_label(ListeningMode mode) {
    switch (mode) {
    case ListeningMode::Off: return "off";
    case ListeningMode::NoiseCancellation: return "noise_cancellation";
    case ListeningMode::Transparency: return "transparency";
    case ListeningMode::Adaptive: return "adaptive";
    }
    return "unknown";
}

const char* ear_status_short(EarDetectionStatus status) {
    switch (status) {
    case EarDetectionStatus::InEar: return "IN_EAR";
    case EarDetectionStatus::OutOfEar: return "OUT_OF_EAR";
    case EarDetectionStatus::InCase: return "IN_CASE";
    case EarDetectionStatus::Disconnected: return "DISCONNECTED";
    }
    return "UNKNOWN";
}

std::optional<bool> decode_bool(const std::vector<uint8_t>& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    return value.front() != 0;
}

std::optional<uint8_t> decode_byte(const std::vector<uint8_t>& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    return value.front();
}

} // namespace

TuiState::TuiState() = default;

void TuiState::handle_event(const AppEvent& event) {
    if (const auto* connected = std::get_if<DeviceConnectedEvent>(&event.payload)) {
        device_.mac = connected->mac;
        device_.name = connected->name;
        device_.product_id = connected->product_id;
        device_.connected = true;

        AppleModels models;
        device_.model_info = models.model_info(connected->product_id);

        std::ostringstream log_line;
        log_line << "device-connected: " << device_.name;
        if (device_.product_id != 0) {
            log_line << " (0x" << std::hex << device_.product_id << ")";
        }
        push_log(log_, log_line.str());
        return;
    }

    if (const auto* disconnected = std::get_if<DeviceDisconnectedEvent>(&event.payload)) {
        if (disconnected->mac == device_.mac) {
            device_.connected = false;
            push_log(log_, "device-disconnected: " + disconnected->mac);
        }
        return;
    }

    if (const auto* aacp = std::get_if<AacpAppEvent>(&event.payload)) {
        if (aacp->mac != device_.mac) {
            // Take ownership of the most recent device only — the TUI is
            // single-device by design.
            device_.mac = aacp->mac;
            if (device_.name.empty()) {
                device_.name = "AirPods";
            }
            device_.connected = true;
        }
        apply_aacp_event(aacp->event);
        return;
    }

    if (std::holds_alternative<AudioUnavailableEvent>(event.payload)) {
        audio_unavailable_ = true;
        push_log(log_, "audio: PulseAudio unavailable");
    }
}

void TuiState::apply_aacp_event(const AACPEvent& event) {
    if (const auto* batteries = std::get_if<std::vector<BatteryInfo>>(&event)) {
        for (const auto& battery : *batteries) {
            switch (battery.component) {
            case BatteryComponent::LeftBud:
                device_.left_battery = battery.level;
                device_.left_status = battery.status;
                break;
            case BatteryComponent::RightBud:
                device_.right_battery = battery.level;
                device_.right_status = battery.status;
                break;
            case BatteryComponent::Case:
                if (battery.status != BatteryStatus::Disconnected) {
                    device_.case_battery = battery.level;
                    device_.case_status = battery.status;
                } else {
                    device_.case_battery.reset();
                    device_.case_status.reset();
                }
                break;
            case BatteryComponent::Headphone:
                device_.headphone_battery = battery.level;
                device_.headphone_status = battery.status;
                break;
            }
        }
        log_battery_summary();
        return;
    }

    if (const auto* status = std::get_if<ControlCommandStatus>(&event)) {
        switch (status->identifier) {
        case ControlCommandIdentifiers::ListeningMode: {
            const auto mode = decode_listening_mode(status->value);
            if (mode) {
                device_.listening_mode = mode;
                push_log(log_, std::string{"audio-mode: "} + listening_mode_label(*mode));
            }
            break;
        }
        case ControlCommandIdentifiers::ConversationDetectConfig:
            device_.conversation_detect_enabled = decode_bool(status->value);
            break;
        case ControlCommandIdentifiers::OneBudAncMode:
            device_.nc_one_bud = decode_bool(status->value);
            break;
        case ControlCommandIdentifiers::AdaptiveVolumeConfig:
            device_.personalized_volume = decode_bool(status->value);
            break;
        case ControlCommandIdentifiers::VolumeSwipeMode:
            device_.volume_swipe = decode_bool(status->value);
            break;
        case ControlCommandIdentifiers::DoubleClickInterval:
            device_.press_speed = decode_byte(status->value);
            break;
        case ControlCommandIdentifiers::ClickHoldInterval:
            device_.press_hold = decode_byte(status->value);
            break;
        case ControlCommandIdentifiers::ChimeVolume:
            device_.tone_volume = decode_byte(status->value);
            break;
        case ControlCommandIdentifiers::VolumeSwipeInterval:
            device_.volume_swipe_length = decode_byte(status->value);
            break;
        case ControlCommandIdentifiers::MicMode:
            device_.mic_mode = decode_byte(status->value);
            break;
        case ControlCommandIdentifiers::AllowAutoConnect:
            device_.auto_connect = decode_bool(status->value);
            break;
        default:
            break;
        }
        return;
    }

    if (const auto* ear = std::get_if<EarDetection>(&event)) {
        device_.ear_left = ear->new_left;
        device_.ear_right = ear->new_right;
        std::ostringstream line;
        line << "ear-detection: L=";
        line << (ear->new_left ? ear_status_short(*ear->new_left) : "-");
        line << " R=";
        line << (ear->new_right ? ear_status_short(*ear->new_right) : "-");
        push_log(log_, line.str());
        return;
    }

    if (const auto* awareness = std::get_if<ConversationalAwareness>(&event)) {
        device_.conversation_awareness = awareness->status;
        return;
    }

    if (const auto* source = std::get_if<AudioSource>(&event)) {
        device_.audio_source = *source;
        return;
    }

    if (std::holds_alternative<ConnectionLost>(event)) {
        device_.connected = false;
        push_log(log_, "aacp: L2CAP connection lost");
        return;
    }

    if (const auto* info = std::get_if<AirPodsInformation>(&event)) {
        if (!info->name.empty()) {
            device_.name = info->name;
        }
        return;
    }

    if (const auto* press = std::get_if<StemPress>(&event)) {
        std::ostringstream line;
        line << "stem-press: ";
        switch (press->type) {
        case StemPressType::SingleTap: line << "single"; break;
        case StemPressType::DoubleTap: line << "double"; break;
        case StemPressType::TripleTap: line << "triple"; break;
        case StemPressType::LongPress: line << "long"; break;
        }
        push_log(log_, line.str());
        return;
    }
}

void TuiState::set_media_status(
    std::optional<std::string> service,
    std::optional<std::string> playback_status,
    std::optional<std::string> track,
    std::optional<std::string> artist
) {
    media_service_ = std::move(service);
    media_status_ = std::move(playback_status);
    media_track_ = std::move(track);
    media_artist_ = std::move(artist);
}

void TuiState::set_volume(std::optional<uint32_t> percent) {
    volume_percent_ = percent;
}

void TuiState::set_codec(std::optional<std::string> codec) {
    codec_ = std::move(codec);
}

void TuiState::set_local_address(std::string mac) {
    local_address_ = std::move(mac);
}

void TuiState::mark_disconnected() {
    device_.connected = false;
}

void TuiState::set_audio_unavailable(bool unavailable) {
    audio_unavailable_ = unavailable;
}

void TuiState::log(std::string message) {
    push_log(log_, std::move(message));
}

void TuiState::set_selected_section(Section section) {
    selected_section_ = section;
    if (section != Section::Settings) {
        settings_open_ = false;
    }
}

void TuiState::open_settings() {
    selected_section_ = Section::Settings;
    settings_open_ = true;
}

void TuiState::close_settings() {
    selected_section_ = Section::Settings;
    settings_open_ = false;
}

void TuiState::log_battery_summary() {
    std::ostringstream line;
    line << "battery:";
    if (device_.left_battery) line << " L=" << static_cast<int>(*device_.left_battery);
    if (device_.right_battery) line << " R=" << static_cast<int>(*device_.right_battery);
    if (device_.case_battery) line << " case=" << static_cast<int>(*device_.case_battery);
    if (device_.headphone_battery) line << " hp=" << static_cast<int>(*device_.headphone_battery);
    push_log(log_, line.str());
}

void TuiState::cycle_section(int delta) {
    settings_open_ = false;
    const auto count = static_cast<int>(SECTION_COUNT);
    int idx = static_cast<int>(selected_section_) + delta;
    idx = ((idx % count) + count) % count;
    selected_section_ = static_cast<Section>(idx);
}

std::size_t TuiState::selected_row(Section section) const {
    const auto idx = static_cast<std::size_t>(section);
    if (idx >= SECTION_COUNT) {
        return 0;
    }
    return selected_rows_[idx];
}

void TuiState::move_cursor(int delta) {
    if (selected_section_ == Section::Settings && !settings_open_) {
        return;
    }

    const auto idx = static_cast<std::size_t>(selected_section_);
    const auto count = static_cast<int>(row_count(selected_section_));
    if (count <= 0) {
        return;
    }
    int row = static_cast<int>(selected_rows_[idx]) + delta;
    row = ((row % count) + count) % count;
    selected_rows_[idx] = static_cast<std::size_t>(row);
}

std::size_t TuiState::row_count(Section section) {
    switch (section) {
    case Section::Battery: return BATTERY_ROWS;
    case Section::NoiseControl: return NOISE_ROWS;
    case Section::Settings: return SETTINGS_ROWS;
    }
    return 0;
}

bool model_supports_spatial_audio(uint16_t product_id) {
    switch (product_id) {
    case 0x2013: // AirPods 3
    case 0x2019: // AirPods 4
    case 0x201b: // AirPods 4 ANC
    case 0x200e: // AirPods Pro
    case 0x2014: // AirPods Pro 2
    case 0x2027: // AirPods Pro 3
    case 0x2024: // AirPods Pro USB-C
    case 0x200a: // AirPods Max
    case 0x201f: // AirPods Max 2024
    case 0x202d: // AirPods Max 2
        return true;
    default:
        return false;
    }
}

} // namespace tui
