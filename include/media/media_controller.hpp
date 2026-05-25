#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_backend.hpp"
#include "bluetooth/aacp.hpp"
#include "config/app_config.hpp"
#include "media/media_session_controller.hpp"

class AACPManager;

class MediaController : public std::enable_shared_from_this<MediaController> {
public:
    MediaController(
        std::string connected_device_mac,
        std::string local_mac,
        std::shared_ptr<AudioBackend> audio,
        std::shared_ptr<MediaSessionController> media_sessions,
        AppConfig config = {}
    );

    ~MediaController();

    MediaController(const MediaController&) = delete;
    MediaController& operator=(const MediaController&) = delete;

    void activate_a2dp_profile();
    void deactivate_a2dp_profile();

    void handle_ear_detection(
        std::optional<EarDetectionStatus> old_left,
        std::optional<EarDetectionStatus> old_right,
        std::optional<EarDetectionStatus> new_left,
        std::optional<EarDetectionStatus> new_right
    );
    void handle_conversational_awareness(uint8_t status);
    void handle_audio_source_change(const AudioSource& source);

    void toggle_play_pause();
    void next_track();
    void previous_track();

    void pause_and_remember();
    void resume_remembered();
    void pause_without_remembering();

    /**
     * @brief Spawns the background loop that polls MPRIS playback state and reclaims
     *        audio ownership when the user resumes media on the local machine.
     */
    void start_playback_listener(std::shared_ptr<AACPManager> aacp_manager);

    /**
     * @brief Stops the playback listener thread if it is running.
     */
    void stop_playback_listener();

    /**
     * @brief Manually reclaim audio ownership from a peer device.
     *
     * Sends `OwnsConnection=01` and forces a fresh AVDTP_START handshake by
     * suspending and resuming the bluez sink. Intended for users to invoke
     * (e.g. via `open-pods --reclaim`) when an iPhone has stolen the audio
     * source and the AirPods refuse to route playback back to this host.
     */
    void reclaim_audio_source();

private:
    /**
     * @brief Stores identity and cached audio card index for the connected device.
     */
    struct DeviceState {
        std::string connected_device_mac;
        std::string local_mac;
        std::optional<uint32_t> card_index;
    };

    /**
     * @brief Tracks local media playback state and services paused by the controller.
     */
    struct PlaybackState {
        bool is_playing = false;
        std::vector<std::string> paused_services;
        bool listener_running = false;
    };

    /**
     * @brief Tracks ear detection settings and the previous left/right ear states.
     */
    struct EarDetectionState {
        bool enabled = true;
        bool disconnect_when_not_wearing = true;
        std::array<std::optional<EarDetectionStatus>, 2> previous_status {
            std::nullopt,
            std::nullopt
        };
    };

    /**
     * @brief Tracks conversation state, including original volume before a conversation.
     */
    struct ConversationState {
        std::optional<uint32_t> original_volume;
        bool started = false;
    };

    /**
     * @brief Tracks audio-source ownership reported by the AirPods.
     *
     * `current_source` is updated only on non-None packets — None blips during
     * peer handoff are absorbed so the last "real" owner survives.
     * `should_reclaim_on_none` is armed when a peer steals audio while Linux
     * was actively producing; it triggers a delayed reclaim after the AirPods
     * report `None`. `generation` invalidates a pending reclaim task whenever a
     * fresher AUDIO_SOURCE event supersedes it.
     */
    struct AudioSourceState {
        std::optional<AudioSource> current_source;
        bool should_reclaim_on_none = false;
        uint64_t generation = 0;
    };

    static bool is_in_ear(const std::optional<EarDetectionStatus>& status);

    void playback_listener_loop();
    void schedule_reclaim(uint64_t generation);
    void perform_reclaim(uint64_t generation);
    void force_audio_stream_restart_locked();
    std::string ensure_card_index_locked();

    DeviceState device_;
    PlaybackState playback_;
    EarDetectionState ear_detection_;
    ConversationState conversation_;
    AudioSourceState audio_source_;
    AppConfig config_;

    std::shared_ptr<AudioBackend> audio_;
    std::shared_ptr<MediaSessionController> media_sessions_;
    std::weak_ptr<AACPManager> aacp_manager_;

    mutable std::mutex state_mutex_;
    std::atomic_bool listener_running_ {false};
    std::thread listener_thread_;
    std::condition_variable listener_cv_;
};
