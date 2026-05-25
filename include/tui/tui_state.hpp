#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include "bluetooth/aacp.hpp"
#include "devices/apple_models.hpp"
#include "server/app_event.hpp"

namespace tui {

enum class ListeningMode : uint8_t {
    Off = 0x01,
    NoiseCancellation = 0x02,
    Transparency = 0x03,
    Adaptive = 0x04,
};

/**
 * @brief Per-device snapshot rendered by the TUI. Mirrors the durable subset of
 *        events the daemon broadcasts plus a few transient values (track/volume)
 *        the TUI itself queries from MPRIS and PulseAudio.
 */
struct DeviceSnapshot {
    std::string mac;
    std::string name;
    uint16_t product_id = 0;
    AppleModelInfo model_info {};

    // Battery levels (0–100). `optional` means "not received yet".
    std::optional<uint8_t> left_battery;
    std::optional<uint8_t> right_battery;
    std::optional<uint8_t> case_battery;
    std::optional<uint8_t> headphone_battery;

    std::optional<BatteryStatus> left_status;
    std::optional<BatteryStatus> right_status;
    std::optional<BatteryStatus> case_status;
    std::optional<BatteryStatus> headphone_status;

    std::optional<EarDetectionStatus> ear_left;
    std::optional<EarDetectionStatus> ear_right;

    // Active listening mode (Off / NC / Transparency / Adaptive).
    std::optional<ListeningMode> listening_mode;
    // Runtime "Conversation Awareness" status reported by AACP (0 = idle).
    std::optional<uint8_t> conversation_awareness;
    std::optional<AudioSource> audio_source;

    // Noise-control toggles.
    std::optional<bool> conversation_detect_enabled; // ConversationDetectConfig 0x28
    std::optional<bool> nc_one_bud;                  // OneBudAncMode          0x1B

    // Settings panel values.
    std::optional<bool>    personalized_volume;     // AdaptiveVolumeConfig  0x26
    std::optional<bool>    volume_swipe;            // VolumeSwipeMode       0x25
    std::optional<uint8_t> press_speed;             // DoubleClickInterval   0x17
    std::optional<uint8_t> press_hold;              // ClickHoldInterval     0x18
    std::optional<uint8_t> tone_volume;             // ChimeVolume           0x1F
    std::optional<uint8_t> volume_swipe_length;     // VolumeSwipeInterval   0x23
    std::optional<uint8_t> mic_mode;                // MicMode               0x01
    std::optional<bool>    auto_connect;            // AllowAutoConnect      0x36

    bool connected = false;
};

/**
 * @brief A single line in the action log (most recent first when rendered).
 */
struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    std::string message;
};

/**
 * @brief Sections the user can move between with TAB.
 */
enum class Section : uint8_t {
    Battery = 0,
    NoiseControl = 1,
    Settings = 2,
};

constexpr std::size_t SECTION_COUNT = 3;

constexpr std::size_t BATTERY_ROWS = 3;       // Left, Right, Case
constexpr std::size_t NOISE_ROWS = 5;         // 4 modes + Conversation Awareness
constexpr std::size_t SETTINGS_ROWS = 9;      // NC-OneBud … Auto Connect

/**
 * @brief Aggregated TUI state. Updated from AppEvents (daemon IPC), MPRIS
 *        (session bus), and PulseAudio (sink polling).
 */
class TuiState {
public:
    TuiState();

    void handle_event(const AppEvent& event);

    void set_media_status(
        std::optional<std::string> service,
        std::optional<std::string> playback_status,
        std::optional<std::string> track,
        std::optional<std::string> artist
    );
    void set_volume(std::optional<uint32_t> percent);
    void set_codec(std::optional<std::string> codec);
    void set_local_address(std::string mac);
    void mark_disconnected();
    void set_audio_unavailable(bool unavailable);

    void log(std::string message);

    [[nodiscard]] const DeviceSnapshot& device() const { return device_; }
    [[nodiscard]] const std::optional<std::string>& media_service() const { return media_service_; }
    [[nodiscard]] const std::optional<std::string>& media_status() const { return media_status_; }
    [[nodiscard]] const std::optional<std::string>& media_track() const { return media_track_; }
    [[nodiscard]] const std::optional<std::string>& media_artist() const { return media_artist_; }
    [[nodiscard]] const std::optional<uint32_t>& volume_percent() const { return volume_percent_; }
    [[nodiscard]] const std::optional<std::string>& codec() const { return codec_; }
    [[nodiscard]] const std::string& local_address() const { return local_address_; }
    [[nodiscard]] bool audio_unavailable() const { return audio_unavailable_; }
    [[nodiscard]] const std::deque<LogEntry>& log_entries() const { return log_; }

    // Section / row navigation -------------------------------------------------
    [[nodiscard]] Section selected_section() const { return selected_section_; }
    void set_selected_section(Section section) { selected_section_ = section; }
    /// Advance the active section by `delta` slots (wraps).
    void cycle_section(int delta);

    [[nodiscard]] std::size_t selected_row(Section section) const;
    void move_cursor(int delta);
    [[nodiscard]] static std::size_t row_count(Section section);

private:
    DeviceSnapshot device_;
    std::optional<std::string> media_service_;
    std::optional<std::string> media_status_;
    std::optional<std::string> media_track_;
    std::optional<std::string> media_artist_;
    std::optional<uint32_t> volume_percent_;
    std::optional<std::string> codec_;
    std::string local_address_;
    bool audio_unavailable_ = false;
    std::deque<LogEntry> log_;
    Section selected_section_ = Section::NoiseControl;
    std::array<std::size_t, SECTION_COUNT> selected_rows_ {};

    void apply_aacp_event(const AACPEvent& event);
    void log_battery_summary();
};

/**
 * @brief Heuristic for whether a model supports Apple Spatial Audio.
 */
[[nodiscard]] bool model_supports_spatial_audio(uint16_t product_id);

} // namespace tui
