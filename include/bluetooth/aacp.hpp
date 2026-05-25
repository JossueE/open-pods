#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>
#include <variant>

/**
 * @brief Identifies the type of audio currently reported by the AirPods.
 */
enum class AudioSourceType : uint8_t {
    None = 0x00,
    Call = 0x01,
    Media = 0x02,
};

inline std::optional<AudioSourceType> get_from_byte_audio_source_type(uint8_t value) {
    switch (value) {
        case 0x00: return AudioSourceType::None;
        case 0x01: return AudioSourceType::Call;
        case 0x02: return AudioSourceType::Media;
        default: return std::nullopt;
	}
}

/**
 * @brief Describes the device currently reported as the AirPods audio source.
 */
struct AudioSource {
    std::string mac;
    AudioSourceType type = AudioSourceType::None;
};

/**
 * @brief Describes the in-ear state reported for one AirPod.
 */
enum class EarDetectionStatus : uint8_t {
    InEar = 0x00,
    OutOfEar = 0x01,
    InCase = 0x02,
    Disconnected = 0x03,
};

inline std::ostream& operator<<(std::ostream& os, EarDetectionStatus identifier) {
    switch (identifier) {
        case EarDetectionStatus::InEar: return os << "In Ear";
        case EarDetectionStatus::OutOfEar: return os << "Out of Ear";
        case EarDetectionStatus::InCase: return os << "In Case";
        case EarDetectionStatus::Disconnected: return os << "Disconnected";
        default: return os << "Unknown";
    }
}

inline constexpr uint16_t PSM{0x1001};
inline constexpr std::chrono::seconds CONNECT_TIMEOUT{10};
inline constexpr std::chrono::milliseconds POLL_INTERVAL{200};
inline constexpr std::array<uint8_t, 4> HEADER_BYTES{ 0x04, 0x00, 0x04, 0x00};

namespace opcodes {
    inline constexpr uint8_t SET_FEATURE_FLAGS = 0x4D;
    inline constexpr uint8_t REQUEST_NOTIFICATIONS = 0x0F;
    inline constexpr uint8_t BATTERY_INFO = 0x04;
    inline constexpr uint8_t CONTROL_COMMAND = 0x09;
    inline constexpr uint8_t EAR_DETECTION = 0x06;
    inline constexpr uint8_t CONVERSATION_AWARENESS = 0x4B;
    inline constexpr uint8_t INFORMATION = 0x1D;
    inline constexpr uint8_t RENAME = 0x1A;
    inline constexpr uint8_t PROXIMITY_KEYS_REQ = 0x30;
    inline constexpr uint8_t PROXIMITY_KEYS_RSP = 0x31;
    inline constexpr uint8_t STEM_PRESS = 0x19;
    inline constexpr uint8_t EQ_DATA = 0x53;
    inline constexpr uint8_t CONNECTED_DEVICES = 0x2E;
    inline constexpr uint8_t AUDIO_SOURCE = 0x0E;
} //namespace opcodes

enum class ControlCommandIdentifiers : uint8_t {
    MicMode = 0x01,
    ButtonSendMode = 0x05,
    VoiceTrigger = 0x12,
    SingleClickMode = 0x14,
    DoubleClickMode = 0x15,
    ClickHoldMode = 0x16,
    DoubleClickInterval = 0x17,
    ClickHoldInterval = 0x18,
    ListeningModeConfigs = 0x1A,
    OneBudAncMode = 0x1B,
    CrownRotationDirection = 0x1C,
    ListeningMode = 0x0D,
    AutoAnswerMode = 0x1E,
    ChimeVolume = 0x1F,
    VolumeSwipeInterval = 0x23,
    CallManagementConfig = 0x24,
    VolumeSwipeMode = 0x25,
    AdaptiveVolumeConfig = 0x26,
    SoftwareMuteConfig = 0x27,
    ConversationDetectConfig = 0x28,
    Ssl = 0x29,
    HearingAid = 0x2C,
    AutoAncStrength = 0x2E,
    HpsGainSwipe = 0x2F,
    HrmState = 0x30,
    InCaseToneConfig = 0x31,
    SiriMultitoneConfig = 0x32,
    HearingAssistConfig = 0x33,
    AllowOffOption = 0x34,
    StemConfig = 0x39,
    SleepDetectionConfig = 0x35,
    AllowAutoConnect = 0x36,
    EarDetectionConfig = 0x0A,
    AutomaticConnectionConfig = 0x20,
    OwnsConnection = 0x06,
};


struct ControlCommandStatus {
    ControlCommandIdentifiers identifier;
    std::vector<uint8_t> value;
};

inline std::ostream& operator<<(std::ostream& os, ControlCommandIdentifiers identifier) {
    switch (identifier) {
        case ControlCommandIdentifiers::MicMode: return os << "Mic Mode";
        case ControlCommandIdentifiers::ButtonSendMode: return os << "Button Send Mode";
        case ControlCommandIdentifiers::VoiceTrigger: return os << "Voice Trigger";
        case ControlCommandIdentifiers::SingleClickMode: return os << "Single Click Mode";
        case ControlCommandIdentifiers::DoubleClickMode: return os << "Double Click Mode";
        case ControlCommandIdentifiers::ClickHoldMode: return os << "Click Hold Mode";
        case ControlCommandIdentifiers::DoubleClickInterval: return os << "Double Click Interval";
        case ControlCommandIdentifiers::ClickHoldInterval: return os << "Click Hold Interval";
        case ControlCommandIdentifiers::ListeningModeConfigs: return os << "Listening Mode Configs";
        case ControlCommandIdentifiers::OneBudAncMode: return os << "One Bud ANC Mode";
        case ControlCommandIdentifiers::CrownRotationDirection: return os << "Crown Rotation Direction";
        case ControlCommandIdentifiers::ListeningMode: return os << "Listening Mode";
        case ControlCommandIdentifiers::AutoAnswerMode: return os << "Auto Answer Mode";
        case ControlCommandIdentifiers::ChimeVolume: return os << "Chime Volume";
        case ControlCommandIdentifiers::VolumeSwipeInterval: return os << "Volume Swipe Interval";
        case ControlCommandIdentifiers::CallManagementConfig: return os << "Call Management Config";
        case ControlCommandIdentifiers::VolumeSwipeMode: return os << "Volume Swipe Mode";
        case ControlCommandIdentifiers::AdaptiveVolumeConfig: return os << "Adaptive Volume Config";
        case ControlCommandIdentifiers::SoftwareMuteConfig: return os << "Software Mute Config";
        case ControlCommandIdentifiers::ConversationDetectConfig: return os << "Conversation Detect Config";
        case ControlCommandIdentifiers::Ssl: return os << "SSL";
        case ControlCommandIdentifiers::HearingAid: return os << "Hearing Aid";
        case ControlCommandIdentifiers::AutoAncStrength: return os << "Auto ANC Strength";
        case ControlCommandIdentifiers::HpsGainSwipe: return os << "HPS Gain Swipe";
        case ControlCommandIdentifiers::HrmState: return os << "HRM State";
        case ControlCommandIdentifiers::InCaseToneConfig: return os << "In-Case Tone Config";
        case ControlCommandIdentifiers::SiriMultitoneConfig: return os << "Siri Multitone Config";
        case ControlCommandIdentifiers::HearingAssistConfig: return os << "Hearing Assist Config";
        case ControlCommandIdentifiers::AllowOffOption: return os << "Allow Off Option";
        case ControlCommandIdentifiers::StemConfig: return os << "Stem Config";
        case ControlCommandIdentifiers::SleepDetectionConfig: return os << "Sleep Detection Config";
        case ControlCommandIdentifiers::AllowAutoConnect: return os << "Allow Auto Connect";
        case ControlCommandIdentifiers::EarDetectionConfig: return os << "Ear Detection Config";
        case ControlCommandIdentifiers::AutomaticConnectionConfig: return os << "Automatic Connection Config";
        case ControlCommandIdentifiers::OwnsConnection: return os << "Owns Connection";
        default: return os << "Unknown";
    }
}

inline std::optional<ControlCommandIdentifiers> get_from_byte_control_command_identifier(uint8_t value) {
    switch (value) {
        case 0x01: return ControlCommandIdentifiers::MicMode;
        case 0x05: return ControlCommandIdentifiers::ButtonSendMode;
        case 0x12: return ControlCommandIdentifiers::VoiceTrigger;
        case 0x14: return ControlCommandIdentifiers::SingleClickMode;
        case 0x15: return ControlCommandIdentifiers::DoubleClickMode;
        case 0x16: return ControlCommandIdentifiers::ClickHoldMode;
        case 0x17: return ControlCommandIdentifiers::DoubleClickInterval;
        case 0x18: return ControlCommandIdentifiers::ClickHoldInterval;
        case 0x1A: return ControlCommandIdentifiers::ListeningModeConfigs;
        case 0x1B: return ControlCommandIdentifiers::OneBudAncMode;
        case 0x1C: return ControlCommandIdentifiers::CrownRotationDirection;
        case 0x0D: return ControlCommandIdentifiers::ListeningMode;
        case 0x1E: return ControlCommandIdentifiers::AutoAnswerMode;
        case 0x1F: return ControlCommandIdentifiers::ChimeVolume;
        case 0x23: return ControlCommandIdentifiers::VolumeSwipeInterval;
        case 0x24: return ControlCommandIdentifiers::CallManagementConfig;
        case 0x25: return ControlCommandIdentifiers::VolumeSwipeMode;
        case 0x26: return ControlCommandIdentifiers::AdaptiveVolumeConfig;
        case 0x27: return ControlCommandIdentifiers::SoftwareMuteConfig;
        case 0x28: return ControlCommandIdentifiers::ConversationDetectConfig;
        case 0x29: return ControlCommandIdentifiers::Ssl;
        case 0x2C: return ControlCommandIdentifiers::HearingAid;
        case 0x2E: return ControlCommandIdentifiers::AutoAncStrength;
        case 0x2F: return ControlCommandIdentifiers::HpsGainSwipe;
        case 0x30: return ControlCommandIdentifiers::HrmState;
        case 0x31: return ControlCommandIdentifiers::InCaseToneConfig;
        case 0x32: return ControlCommandIdentifiers::SiriMultitoneConfig;
        case 0x33: return ControlCommandIdentifiers::HearingAssistConfig;
        case 0x34: return ControlCommandIdentifiers::AllowOffOption;
        case 0x39: return ControlCommandIdentifiers::StemConfig;
        case 0x35: return ControlCommandIdentifiers::SleepDetectionConfig;
        case 0x36: return ControlCommandIdentifiers::AllowAutoConnect;
        case 0x0A: return ControlCommandIdentifiers::EarDetectionConfig;
        case 0x20: return ControlCommandIdentifiers::AutomaticConnectionConfig;
        case 0x06: return ControlCommandIdentifiers::OwnsConnection;
        default: return std::nullopt;
	}
}

enum class ProximityKeyType : uint8_t {
    Irk = 0x01,
    EncKey = 0x04,
};

inline std::optional<ProximityKeyType> get_from_byte_proximity_key_type(uint8_t value) {
    switch (value) {
        case 0x01: return ProximityKeyType::Irk;
        case 0x04: return ProximityKeyType::EncKey;
        default: return std::nullopt;
	}
}

enum class StemPressType : uint8_t {
    SingleTap = 0x05,
    DoubleTap = 0x06,
    TripleTap = 0x07,
    LongPress = 0x08,
};

enum class StemPressLocation : uint8_t {
    Left = 0x01,
    Right = 0x02,
};

struct StemPress {
    StemPressType type;
    std::optional<StemPressLocation> location;
};

enum class BatteryComponent : int8_t {
    Headphone = 1,
    LeftBud = 4,
    RightBud = 2,
    Case = 8,
};

enum class BatteryStatus : int8_t {
    Charging = 1,
    NotCharging = 2,
    Disconnected = 4,
    InUse = 5, // 0x05 — active/playing state on AirPods Pro 3rd gen
};

struct BatteryInfo {
    BatteryComponent component;
    BatteryStatus status;
    uint8_t level; // 0-100
};

struct ConnectedDevice {
    std::string mac;
    uint8_t info1;
    uint8_t info2;
};

struct ConnectedDevices {
    std::vector<ConnectedDevice> old_devices;
    std::vector<ConnectedDevice> new_devices;
};

struct ConversationalAwareness {
    uint8_t status = 0;
};

struct EqualizerData {
    std::array<uint8_t, 8> data;
};

struct EarDetection {
    std::optional<EarDetectionStatus> old_left;
    std::optional<EarDetectionStatus> old_right;
    std::optional<EarDetectionStatus> new_left;
    std::optional<EarDetectionStatus> new_right;
};

struct OwnershipToFalseRequest {};
struct ConnectionLost {};

struct AirPodsLEKeys {
    std::string irk;
    std::string enc_key;
};

struct AirPodsInformation {
    std::string name;
    std::string model_number;
    std::string manufacturer;
    std::string serial_number;
    std::string version1;
    std::string version2;
    std::string hardware_revision;
    std::string updater_identifier;
    std::string left_serial_number;
    std::string right_serial_number;
    std::string version3;
    AirPodsLEKeys le_keys;
};

using AACPEvent = std::variant<
    std::vector<BatteryInfo>,
    ControlCommandStatus,
    EarDetection,
    ConversationalAwareness,
    AudioSource,
    ConnectedDevices,
    OwnershipToFalseRequest,
    StemPress,
    EqualizerData,
    ConnectionLost,
    AirPodsInformation
>;




