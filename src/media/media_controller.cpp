#include "media/media_controller.hpp"

#include "bluetooth/aacp_manager.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace {

constexpr auto LISTENER_POLL = std::chrono::milliseconds{500};
// Settle window before reclaiming on a `None` packet. Long enough to absorb
// the AirPods' transient None blip during handoff (observed up to ~1s), short
// enough that legitimate "peer paused" reclaims feel snappy.
constexpr auto RECLAIM_SETTLE = std::chrono::milliseconds{1500};

bool mac_equals_ignore_case(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return std::equal(
        a.begin(),
        a.end(),
        b.begin(),
        [](char left, char right) {
            return std::tolower(static_cast<unsigned char>(left))
                == std::tolower(static_cast<unsigned char>(right));
        }
    );
}

} // namespace

MediaController::MediaController(
    std::string connected_device_mac,
    std::string local_mac,
    std::shared_ptr<AudioBackend> audio,
    std::shared_ptr<MediaSessionController> media_sessions,
    AppConfig config
)
    : device_{
        std::move(connected_device_mac),
        std::move(local_mac),
        std::nullopt,
    }
    , config_(std::move(config))
    , audio_(std::move(audio))
    , media_sessions_(std::move(media_sessions))
{
}

MediaController::~MediaController()
{
    stop_playback_listener();
}

void MediaController::activate_a2dp_profile() {
    std::string mac;
    std::optional<uint32_t> card_index;
    {
        std::lock_guard lock{state_mutex_};
        if (device_.connected_device_mac.empty() || !audio_) {
            return;
        }

        if (!device_.card_index.has_value()) {
            device_.card_index = audio_->find_card_by_mac(device_.connected_device_mac);
        }
        card_index = device_.card_index;
        mac = device_.connected_device_mac;
    }

    if (!card_index) {
        return;
    }

    if (!audio_->is_a2dp_available(*card_index)) {
        return;
    }

    const auto profile = audio_->best_available_a2dp_profile(*card_index);
    if (!profile) {
        return;
    }

    if (!audio_->set_card_profile(*card_index, *profile)) {
        return;
    }

    const auto sink = audio_->find_sink_by_mac(mac);
    if (!sink) {
        return;
    }

    audio_->set_default_sink(*sink);
    audio_->move_all_sink_inputs(*sink);
}

void MediaController::deactivate_a2dp_profile() {
    std::optional<uint32_t> card_index;
    {
        std::lock_guard lock{state_mutex_};
        if (device_.connected_device_mac.empty() || !audio_) {
            return;
        }

        if (!device_.card_index.has_value()) {
            device_.card_index = audio_->find_card_by_mac(device_.connected_device_mac);
        }
        card_index = device_.card_index;
    }

    if (!card_index) {
        return;
    }

    audio_->set_card_profile(*card_index, "off");
}

void MediaController::handle_ear_detection(
    std::optional<EarDetectionStatus> old_left,
    std::optional<EarDetectionStatus> old_right,
    std::optional<EarDetectionStatus> new_left,
    std::optional<EarDetectionStatus> new_right
)
{
    bool ear_detection_enabled = false;
    bool disconnect_when_not_wearing = false;
    {
        std::lock_guard lock{state_mutex_};
        if (!ear_detection_.enabled) {
            return;
        }
        ear_detection_enabled = ear_detection_.enabled;
        disconnect_when_not_wearing = ear_detection_.disconnect_when_not_wearing;
        ear_detection_.previous_status = { new_left, new_right };
    }
    (void)ear_detection_enabled;

    const auto count_in_ear = [](const std::optional<EarDetectionStatus>& l,
                                  const std::optional<EarDetectionStatus>& r) {
        return (is_in_ear(l) ? 1 : 0) + (is_in_ear(r) ? 1 : 0);
    };
    const int old_in = count_in_ear(old_left, old_right);
    const int new_in = count_in_ear(new_left, new_right);

    const bool old_any_in_ear = old_in > 0;
    const bool new_any_in_ear = new_in > 0;
    const bool new_all_out = new_in == 0;
    const bool removed_a_bud = new_in < old_in;
    const bool inserted_a_bud = new_in > old_in;

    OPENPODS_DEBUG("ear_detection old_in=" << old_in
        << " new_in=" << new_in
        << " removed=" << removed_a_bud
        << " inserted=" << inserted_a_bud);

    if (!old_any_in_ear && new_any_in_ear) {
        OPENPODS_DEBUG("ear_detection: first bud inserted -> activating A2DP");
        activate_a2dp_profile();
    }

    // Pause whenever a bud is removed (Apple's default behavior). The previous
    // implementation only paused when both pods were out, which felt broken
    // because most users expect playback to halt the moment one pod leaves the
    // ear, exactly like an iPhone.
    if (removed_a_bud) {
        OPENPODS_DEBUG("ear_detection: bud removed -> pause_and_remember");
        pause_and_remember();
        if (new_all_out && disconnect_when_not_wearing) {
            deactivate_a2dp_profile();
        }
    }

    if (inserted_a_bud && new_any_in_ear) {
        OPENPODS_DEBUG("ear_detection: bud inserted -> resume_remembered");
        resume_remembered();
    }
}

void MediaController::handle_conversational_awareness(uint8_t status) {
    std::string mac;
    {
        std::lock_guard lock{state_mutex_};
        if (device_.connected_device_mac.empty() || !audio_) {
            return;
        }
        mac = device_.connected_device_mac;
    }

    const auto sink = audio_->find_sink_by_mac(mac);
    if (!sink) {
        return;
    }

    const auto current_volume = audio_->get_sink_volume(*sink);

    switch (status) {
    case 1: {
        const auto original = current_volume.value_or(0);
        {
            std::lock_guard lock{state_mutex_};
            if (!conversation_.started) {
                conversation_.original_volume = original;
                conversation_.started = true;
            }
        }
        if (original > 25) {
            audio_->set_sink_volume(*sink, 25);
        }
        break;
    }

    case 2: {
        std::optional<uint32_t> original_volume;
        {
            std::lock_guard lock{state_mutex_};
            original_volume = conversation_.original_volume;
        }
        if (original_volume.has_value() && *original_volume > 15) {
            audio_->set_sink_volume(*sink, 15);
        }
        break;
    }

    case 3: {
        std::optional<uint32_t> original_volume;
        bool started = false;
        {
            std::lock_guard lock{state_mutex_};
            started = conversation_.started;
            original_volume = conversation_.original_volume;
        }
        if (!started) {
            return;
        }
        if (original_volume.has_value()) {
            const auto target = *original_volume > 25 ? 25 : *original_volume;
            audio_->set_sink_volume(*sink, target);
        } else if (current_volume.has_value()) {
            const auto target = *current_volume > 25 ? 25 : *current_volume;
            audio_->set_sink_volume(*sink, target);
        }
        break;
    }

    case 4:
    case 6:
    case 7:
    case 8:
    case 9: {
        std::optional<uint32_t> original_volume;
        {
            std::lock_guard lock{state_mutex_};
            if (!conversation_.started) {
                return;
            }
            original_volume = conversation_.original_volume;
            conversation_.original_volume = std::nullopt;
            conversation_.started = false;
        }
        if (original_volume.has_value()) {
            audio_->set_sink_volume(*sink, *original_volume);
        }
        break;
    }

    default:
        break;
    }
}

void MediaController::handle_audio_source_change(const AudioSource& source)
{
    // Faithful port of airpods-tui's handle_audio_source_change:
    //   1. Peer takes audio while Linux had audio  -> pause and arm reclaim flag.
    //   2. We armed the flag and now see `type=None` -> schedule a delayed
    //      reclaim. The AirPods routinely emit a transient None packet during
    //      handoff that is followed within ~1s by a fresh non-None source, so
    //      we settle for `RECLAIM_SETTLE` and let any newer AUDIO_SOURCE event
    //      cancel the pending task via the generation counter.
    //   3. Otherwise just update state.

    enum class Action { Pause, ScheduleReclaim, Nothing };

    // Probe PulseAudio for any non-corked sink input on the bluez sink before
    // touching state. This catches Discord/games/browser audio that does not
    // expose MPRIS, so the reclaim flag arms even when `is_playing` is false.
    bool pa_active = false;
    {
        std::string mac;
        {
            std::lock_guard lock{state_mutex_};
            mac = device_.connected_device_mac;
        }
        if (audio_ && !mac.empty()) {
            if (const auto sink = audio_->find_sink_by_mac(mac)) {
                pa_active = audio_->has_active_sink_input(*sink);
            }
        }
    }

    Action action = Action::Nothing;
    uint64_t my_gen = 0;
    {
        std::lock_guard lock{state_mutex_};

        // Bump the generation on every event. Any in-flight reclaim task
        // captured an older value and will bail when it sees the mismatch.
        audio_source_.generation += 1;
        my_gen = audio_source_.generation;

        const bool source_is_none = source.type == AudioSourceType::None;
        const bool source_is_other = !mac_equals_ignore_case(source.mac, device_.local_mac);

        // Keep current_source as the *last non-None* source. None packets do
        // not clear it. Matches airpods-tui exactly so peer-vs-local ownership
        // stays reliable across the AirPods' transient None blips.
        if (!source_is_none) {
            audio_source_.current_source = source;
        }

        if (source_is_other && !source_is_none) {
            const bool had_audio = playback_.is_playing || pa_active;
            if (had_audio) {
                audio_source_.should_reclaim_on_none = true;
            }
            action = Action::Pause;
        } else if (source_is_none && audio_source_.should_reclaim_on_none) {
            // Leave the flag set; the reclaim task clears it on success. If a
            // fresh non-None AUDIO_SOURCE arrives within the settle window the
            // generation check cancels the task and the flag stays armed for
            // the next None packet.
            action = Action::ScheduleReclaim;
        }
    }

    switch (action) {
    case Action::Pause:
        OPENPODS_LOG("Audio ownership moved to peer device, pausing local media");
        pause_and_remember();
        break;
    case Action::ScheduleReclaim:
        OPENPODS_LOG("Peer source went None, scheduling reclaim in "
            << RECLAIM_SETTLE.count() << "ms (gen " << my_gen << ")");
        schedule_reclaim(my_gen);
        break;
    case Action::Nothing:
        break;
    }
}

void MediaController::toggle_play_pause() {
    OPENPODS_DEBUG("MediaController::toggle_play_pause");
    if (media_sessions_) {
        media_sessions_->play_pause_first();
    }
}

void MediaController::next_track() {
    OPENPODS_DEBUG("MediaController::next_track");
    if (media_sessions_) {
        media_sessions_->next();
    }
}

void MediaController::previous_track() {
    OPENPODS_DEBUG("MediaController::previous_track");
    if (media_sessions_) {
        media_sessions_->previous();
    }
}

void MediaController::pause_and_remember() {
    if (!media_sessions_) {
        OPENPODS_DEBUG("pause_and_remember: media_sessions_ is null");
        return;
    }

    auto paused = media_sessions_->pause_playing_players();
    OPENPODS_DEBUG("pause_and_remember: paused " << paused.size() << " player(s)");
    if (paused.empty()) {
        return;
    }

    std::lock_guard lock{state_mutex_};
    playback_.paused_services = std::move(paused);
    playback_.is_playing = false;
}

void MediaController::resume_remembered()
{
    std::vector<std::string> services;
    {
        std::lock_guard lock{state_mutex_};
        if (playback_.paused_services.empty()) {
            return;
        }
        services = std::move(playback_.paused_services);
    }

    if (media_sessions_) {
        media_sessions_->resume_players(services);
    }
}

void MediaController::pause_without_remembering() {
    if (media_sessions_) {
        (void)media_sessions_->pause_playing_players();
    }

    std::lock_guard lock{state_mutex_};
    playback_.is_playing = false;
}

void MediaController::reclaim_audio_source()
{
    OPENPODS_LOG("Manual audio reclaim requested");

    {
        std::lock_guard lock{state_mutex_};
        audio_source_.should_reclaim_on_none = false;
        // Bump the generation so any in-flight delayed reclaim from
        // handle_audio_source_change is invalidated and we don't double-fire.
        audio_source_.generation += 1;
    }

    if (const auto aacp = aacp_manager_.lock()) {
        aacp->send_control_command(
            ControlCommandIdentifiers::OwnsConnection,
            {0x01}
        );
    }

    force_audio_stream_restart_locked();
}

void MediaController::start_playback_listener(std::shared_ptr<AACPManager> aacp_manager)
{
    {
        std::lock_guard lock{state_mutex_};
        if (playback_.listener_running) {
            return;
        }
        playback_.listener_running = true;
    }
    aacp_manager_ = aacp_manager;
    listener_running_ = true;

    auto self = shared_from_this();
    listener_thread_ = std::thread([self]() {
        self->playback_listener_loop();
    });
}

void MediaController::stop_playback_listener()
{
    listener_running_ = false;
    listener_cv_.notify_all();
    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }

    std::lock_guard lock{state_mutex_};
    playback_.listener_running = false;
}

void MediaController::playback_listener_loop()
{
    std::mutex wait_mutex;
    std::unique_lock wait_lock{wait_mutex};

    while (listener_running_) {
        if (listener_cv_.wait_for(
                wait_lock,
                LISTENER_POLL,
                [this] { return !listener_running_; }
            )) {
            break;
        }

        const bool is_playing = media_sessions_
            ? media_sessions_->any_player_playing()
            : false;

        bool was_playing = false;
        {
            std::lock_guard lock{state_mutex_};
            was_playing = playback_.is_playing;
            playback_.is_playing = is_playing;
        }

        if (was_playing || !is_playing) {
            continue;
        }

        // Transition: not playing → playing. Re-claim ownership and activate A2DP
        // when the buds are in ear and Linux is not already the active source.
        const auto aacp = aacp_manager_.lock();
        if (!aacp) {
            continue;
        }

        const auto [left, right] = aacp->ear_detection_state();
        const bool ear_ok = left == EarDetectionStatus::InEar
            || right == EarDetectionStatus::InEar;
        if (!ear_ok) {
            OPENPODS_DEBUG("playback_listener: started but no bud in ear, skipping");
            continue;
        }

        bool already_owned = false;
        {
            std::lock_guard lock{state_mutex_};
            already_owned = audio_source_.current_source.has_value()
                && mac_equals_ignore_case(
                    audio_source_.current_source->mac,
                    device_.local_mac
                );
            // Disarm any pending handoff reclaim — Linux is now the active
            // player, regardless of whether we claim or just observed the
            // already-owned state. Matches airpods-tui.
            audio_source_.should_reclaim_on_none = false;
        }

        if (already_owned) {
            OPENPODS_DEBUG(
                "playback_listener: Linux already owns audio per AirPods, no claim needed"
            );
            continue;
        }

        OPENPODS_LOG("Media playback started, claiming ownership and activating A2DP");
        aacp->send_control_command(
            ControlCommandIdentifiers::OwnsConnection,
            {0x01}
        );
        activate_a2dp_profile();
    }
}

void MediaController::schedule_reclaim(uint64_t generation)
{
    auto self = shared_from_this();
    std::thread([self, generation]() {
        std::this_thread::sleep_for(RECLAIM_SETTLE);
        self->perform_reclaim(generation);
    }).detach();
}

void MediaController::perform_reclaim(uint64_t generation)
{
    {
        std::lock_guard lock{state_mutex_};
        if (audio_source_.generation != generation) {
            OPENPODS_DEBUG("perform_reclaim: gen " << generation
                << " superseded by " << audio_source_.generation << ", cancelling");
            return;
        }
        if (!audio_source_.should_reclaim_on_none) {
            OPENPODS_DEBUG("perform_reclaim: flag cleared by another path, cancelling");
            return;
        }
        audio_source_.should_reclaim_on_none = false;
    }

    OPENPODS_LOG("Settle window expired, reclaiming ownership");

    if (const auto aacp = aacp_manager_.lock()) {
        aacp->send_control_command(
            ControlCommandIdentifiers::OwnsConnection,
            {0x01}
        );
    }

    // Suspend/resume the bluez sink to force a fresh AVDTP_START handshake.
    // Deliberately do NOT Play the previously paused MPRIS players: doing so
    // would feed the listener loop a Playing transition that re-fires
    // OwnsConnection=01 and cascades against the peer device. The user can
    // press play themselves; ownership has already transferred.
    force_audio_stream_restart_locked();
}

void MediaController::force_audio_stream_restart_locked()
{
    std::string mac;
    {
        std::lock_guard lock{state_mutex_};
        mac = device_.connected_device_mac;
    }

    if (!audio_) {
        activate_a2dp_profile();
        return;
    }

    const auto sink = audio_->find_sink_by_mac(mac);
    if (!sink) {
        OPENPODS_DEBUG("force_audio_stream_restart: no sink for " << mac
            << ", falling back to profile activation");
        activate_a2dp_profile();
        return;
    }

    OPENPODS_DEBUG("force_audio_stream_restart: suspending sink " << *sink);
    if (!audio_->suspend_sink(*sink, true)) {
        activate_a2dp_profile();
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    OPENPODS_DEBUG("force_audio_stream_restart: resuming sink " << *sink);
    if (!audio_->suspend_sink(*sink, false)) {
        activate_a2dp_profile();
    }
}

bool MediaController::is_in_ear(const std::optional<EarDetectionStatus>& status) {
    return status.has_value() && *status == EarDetectionStatus::InEar;
}
