#include "tui/tui_app.hpp"

#include "audio/pulse_audio/pulse_audio_backend.hpp"
#include "bluetooth/bluez_discovery.hpp"
#include "media/mpris_media_session.hpp"
#include "server/app_event.hpp"
#include "server/ipc.hpp"
#include "tui/tui_render.hpp"
#include "tui/tui_state.hpp"
#include "tui/tui_term.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <dbus/dbus.h>

namespace tui {

namespace {

constexpr auto MEDIA_REFRESH_INTERVAL = std::chrono::milliseconds{750};
constexpr int IDLE_POLL_MS = 80;

/**
 * @brief Lightweight wrapper around libdbus that fetches the Player
 *        properties (PlaybackStatus + Metadata.{title,artist}) for the first
 *        non-kdeconnect MPRIS service on the session bus.
 */
class MprisProbe {
public:
    MprisProbe();
    ~MprisProbe();

    struct Snapshot {
        std::optional<std::string> service;
        std::optional<std::string> playback_status;
        std::optional<std::string> track;
        std::optional<std::string> artist;
    };

    [[nodiscard]] Snapshot probe();

private:
    DBusConnection* connection_ = nullptr;

    [[nodiscard]] std::vector<std::string> list_mpris_services();
    [[nodiscard]] std::optional<std::string> get_string_property(
        const std::string& service,
        const char* property
    );
    [[nodiscard]] std::pair<std::optional<std::string>, std::optional<std::string>>
        get_metadata(const std::string& service);
};

MprisProbe::MprisProbe() {
    DBusError error;
    dbus_error_init(&error);
    connection_ = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        connection_ = nullptr;
    }
    if (connection_ != nullptr) {
        dbus_connection_set_exit_on_disconnect(connection_, FALSE);
    }
}

MprisProbe::~MprisProbe() {
    if (connection_ != nullptr) {
        dbus_connection_close(connection_);
        dbus_connection_unref(connection_);
    }
}

std::vector<std::string> MprisProbe::list_mpris_services() {
    std::vector<std::string> out;
    if (connection_ == nullptr) return out;

    DBusMessage* message = dbus_message_new_method_call(
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames"
    );
    if (message == nullptr) return out;

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        connection_, message, 1500, &error
    );
    dbus_message_unref(message);
    if (dbus_error_is_set(&error) || reply == nullptr) {
        dbus_error_free(&error);
        if (reply != nullptr) dbus_message_unref(reply);
        return out;
    }

    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter)
        && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        DBusMessageIter array;
        dbus_message_iter_recurse(&iter, &array);
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
            const char* value = nullptr;
            dbus_message_iter_get_basic(&array, &value);
            if (value != nullptr) {
                std::string s{value};
                if (s.starts_with("org.mpris.MediaPlayer2.")
                    && !s.starts_with("org.mpris.MediaPlayer2.kdeconnect.mpris_")) {
                    out.push_back(std::move(s));
                }
            }
            dbus_message_iter_next(&array);
        }
    }
    dbus_message_unref(reply);
    return out;
}

std::optional<std::string> MprisProbe::get_string_property(
    const std::string& service,
    const char* property
) {
    if (connection_ == nullptr) return std::nullopt;

    DBusMessage* message = dbus_message_new_method_call(
        service.c_str(),
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "Get"
    );
    if (message == nullptr) return std::nullopt;
    const char* iface = "org.mpris.MediaPlayer2.Player";
    if (!dbus_message_append_args(
            message,
            DBUS_TYPE_STRING, &iface,
            DBUS_TYPE_STRING, &property,
            DBUS_TYPE_INVALID)) {
        dbus_message_unref(message);
        return std::nullopt;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        connection_, message, 800, &error
    );
    dbus_message_unref(message);
    if (dbus_error_is_set(&error) || reply == nullptr) {
        dbus_error_free(&error);
        if (reply != nullptr) dbus_message_unref(reply);
        return std::nullopt;
    }

    std::optional<std::string> result;
    DBusMessageIter root;
    if (dbus_message_iter_init(reply, &root)
        && dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_VARIANT) {
        DBusMessageIter variant;
        dbus_message_iter_recurse(&root, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
            const char* value = nullptr;
            dbus_message_iter_get_basic(&variant, &value);
            if (value != nullptr) result = value;
        }
    }
    dbus_message_unref(reply);
    return result;
}

std::pair<std::optional<std::string>, std::optional<std::string>>
MprisProbe::get_metadata(const std::string& service) {
    std::pair<std::optional<std::string>, std::optional<std::string>> out;
    if (connection_ == nullptr) return out;

    DBusMessage* message = dbus_message_new_method_call(
        service.c_str(),
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "Get"
    );
    if (message == nullptr) return out;
    const char* iface = "org.mpris.MediaPlayer2.Player";
    const char* property = "Metadata";
    if (!dbus_message_append_args(
            message,
            DBUS_TYPE_STRING, &iface,
            DBUS_TYPE_STRING, &property,
            DBUS_TYPE_INVALID)) {
        dbus_message_unref(message);
        return out;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        connection_, message, 800, &error
    );
    dbus_message_unref(message);
    if (dbus_error_is_set(&error) || reply == nullptr) {
        dbus_error_free(&error);
        if (reply != nullptr) dbus_message_unref(reply);
        return out;
    }

    DBusMessageIter root;
    if (!dbus_message_iter_init(reply, &root)
        || dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_VARIANT) {
        dbus_message_unref(reply);
        return out;
    }
    DBusMessageIter variant;
    dbus_message_iter_recurse(&root, &variant);
    if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return out;
    }
    DBusMessageIter dict_iter;
    dbus_message_iter_recurse(&variant, &dict_iter);
    while (dbus_message_iter_get_arg_type(&dict_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&dict_iter, &entry);

        const char* key = nullptr;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
        }
        dbus_message_iter_next(&entry);

        if (key == nullptr) {
            dbus_message_iter_next(&dict_iter);
            continue;
        }

        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            DBusMessageIter inner;
            dbus_message_iter_recurse(&entry, &inner);
            const auto inner_type = dbus_message_iter_get_arg_type(&inner);

            if (std::string_view{key} == "xesam:title"
                && inner_type == DBUS_TYPE_STRING) {
                const char* value = nullptr;
                dbus_message_iter_get_basic(&inner, &value);
                if (value != nullptr) out.first = value;
            } else if (std::string_view{key} == "xesam:artist"
                && inner_type == DBUS_TYPE_ARRAY) {
                DBusMessageIter artist_iter;
                dbus_message_iter_recurse(&inner, &artist_iter);
                if (dbus_message_iter_get_arg_type(&artist_iter) == DBUS_TYPE_STRING) {
                    const char* value = nullptr;
                    dbus_message_iter_get_basic(&artist_iter, &value);
                    if (value != nullptr) out.second = value;
                }
            }
        }
        dbus_message_iter_next(&dict_iter);
    }
    dbus_message_unref(reply);
    return out;
}

MprisProbe::Snapshot MprisProbe::probe() {
    Snapshot snap;
    const auto services = list_mpris_services();
    for (const auto& service : services) {
        const auto status = get_string_property(service, "PlaybackStatus");
        if (!status) continue;
        if (*status == "Playing"
            || (!snap.service.has_value() && *status == "Paused")) {
            snap.service = service;
            snap.playback_status = status;
            auto metadata = get_metadata(service);
            snap.track = std::move(metadata.first);
            snap.artist = std::move(metadata.second);
            if (*status == "Playing") {
                break;
            }
        }
    }
    return snap;
}

void refresh_volume(
    PulseAudioBackend& audio,
    const std::string& mac,
    TuiState& state
) {
    if (mac.empty()) {
        state.set_volume(std::nullopt);
        return;
    }
    const auto sink = audio.find_sink_by_mac(mac);
    if (!sink) {
        state.set_volume(std::nullopt);
        return;
    }
    const auto volume = audio.get_sink_volume(*sink);
    state.set_volume(volume);
}

// ─────────────────── Section / row dispatch ───────────────────

constexpr std::array<ListeningMode, 4> NOISE_MODES = {
    ListeningMode::Transparency,
    ListeningMode::NoiseCancellation,
    ListeningMode::Adaptive,
    ListeningMode::Off,
};

bool listening_mode_supported(const DeviceSnapshot& dev, ListeningMode mode) {
    switch (mode) {
    case ListeningMode::Off:
        return true;
    case ListeningMode::NoiseCancellation:
    case ListeningMode::Transparency:
        return dev.model_info.has_noise_cancellation;
    case ListeningMode::Adaptive:
        return dev.model_info.has_adaptive_transparency;
    }
    return false;
}

/**
 * @brief Sends a Listening Mode command for the device's currently selected
 *        radio row, or toggles Conversation Awareness when row 4 is active.
 *        Logs the user-visible action message.
 *
 * @return true if a command was actually sent.
 */
bool apply_noise_row(
    ipc::IpcClient& client,
    TuiState& state,
    int adjust // -1, 0, +1; 0 = "select / toggle"
) {
    const auto& dev = state.device();
    const auto row = state.selected_row(Section::NoiseControl);

    if (row < NOISE_MODES.size()) {
        const auto mode = NOISE_MODES[row];
        if (!listening_mode_supported(dev, mode)) {
            state.log("audio-mode: unsupported on this model");
            return false;
        }
        client.send_command({
            dev.mac,
            DeviceCommand::control_command(
                ControlCommandIdentifiers::ListeningMode,
                {static_cast<uint8_t>(mode)}
            )
        });
        std::ostringstream msg;
        msg << "audio-mode: requesting ";
        switch (mode) {
        case ListeningMode::Off: msg << "off"; break;
        case ListeningMode::NoiseCancellation: msg << "noise_cancellation"; break;
        case ListeningMode::Transparency: msg << "transparency"; break;
        case ListeningMode::Adaptive: msg << "adaptive"; break;
        }
        state.log(msg.str());
        return true;
    }

    // Row 4 = Conversation Awareness toggle.
    if (!dev.model_info.has_conversation_awareness) {
        state.log("conv-aware: unsupported on this model");
        return false;
    }
    bool target = !(dev.conversation_detect_enabled.value_or(false));
    if (adjust > 0) target = true;
    else if (adjust < 0) target = false;
    client.send_command({
        dev.mac,
        DeviceCommand::control_command(
            ControlCommandIdentifiers::ConversationDetectConfig,
            {static_cast<uint8_t>(target ? 0x01 : 0x00)}
        )
    });
    state.log(std::string{"conv-aware: requesting "} + (target ? "on" : "off"));
    return true;
}

/**
 * @brief Adjusts the value of the highlighted Settings row. `direction` is
 *        +1 / -1 (LEFT/RIGHT) or 0 (ENTER = toggle / increment).
 */
bool apply_settings_row(
    ipc::IpcClient& client,
    TuiState& state,
    int direction
) {
    const auto& dev = state.device();
    const auto row = state.selected_row(Section::Settings);
    auto send_byte = [&](ControlCommandIdentifiers id, uint8_t value) {
        client.send_command({dev.mac, DeviceCommand::control_command(id, {value})});
    };

    auto toggle_bool = [&](ControlCommandIdentifiers id, std::optional<bool> current) {
        bool next = !(current.value_or(false));
        if (direction > 0) next = true;
        else if (direction < 0) next = false;
        send_byte(id, next ? 0x01 : 0x00);
        return next;
    };

    auto adjust_byte = [&](
        ControlCommandIdentifiers id,
        std::optional<uint8_t> current,
        uint8_t min,
        uint8_t max
    ) {
        uint8_t next = current.value_or(min);
        if (direction == 0) {
            next = (next >= max) ? min : static_cast<uint8_t>(next + 1);
        } else if (direction > 0 && next < max) {
            ++next;
        } else if (direction < 0 && next > min) {
            --next;
        }
        send_byte(id, next);
        return next;
    };

    switch (row) {
    case 0: { // NC with One AirPod
        if (!dev.model_info.has_noise_cancellation) {
            state.log("nc-one-bud: unsupported on this model");
            return false;
        }
        const bool next = toggle_bool(ControlCommandIdentifiers::OneBudAncMode, dev.nc_one_bud);
        state.log(std::string{"nc-one-bud: requesting "} + (next ? "on" : "off"));
        return true;
    }
    case 1: { // Personalized Volume
        const bool next = toggle_bool(ControlCommandIdentifiers::AdaptiveVolumeConfig, dev.personalized_volume);
        state.log(std::string{"personalized-volume: requesting "} + (next ? "on" : "off"));
        return true;
    }
    case 2: { // Volume Swipe
        const bool next = toggle_bool(ControlCommandIdentifiers::VolumeSwipeMode, dev.volume_swipe);
        state.log(std::string{"volume-swipe: requesting "} + (next ? "on" : "off"));
        return true;
    }
    case 3: { // Press Speed
        const auto next = adjust_byte(ControlCommandIdentifiers::DoubleClickInterval, dev.press_speed, 0, 2);
        std::ostringstream m; m << "press-speed: requesting " << static_cast<int>(next);
        state.log(m.str());
        return true;
    }
    case 4: { // Press & Hold
        const auto next = adjust_byte(ControlCommandIdentifiers::ClickHoldInterval, dev.press_hold, 0, 2);
        std::ostringstream m; m << "press-hold: requesting " << static_cast<int>(next);
        state.log(m.str());
        return true;
    }
    case 5: { // Tone Volume
        const auto next = adjust_byte(ControlCommandIdentifiers::ChimeVolume, dev.tone_volume, 0, 3);
        std::ostringstream m; m << "tone-volume: requesting " << static_cast<int>(next);
        state.log(m.str());
        return true;
    }
    case 6: { // Volume Swipe Length
        const auto next = adjust_byte(ControlCommandIdentifiers::VolumeSwipeInterval, dev.volume_swipe_length, 0, 2);
        std::ostringstream m; m << "swipe-length: requesting " << static_cast<int>(next);
        state.log(m.str());
        return true;
    }
    case 7: { // Mic Mode
        const auto next = adjust_byte(ControlCommandIdentifiers::MicMode, dev.mic_mode, 0, 2);
        std::ostringstream m; m << "mic-mode: requesting " << static_cast<int>(next);
        state.log(m.str());
        return true;
    }
    case 8: { // Auto Connect
        const bool next = toggle_bool(ControlCommandIdentifiers::AllowAutoConnect, dev.auto_connect);
        state.log(std::string{"auto-connect: requesting "} + (next ? "on" : "off"));
        return true;
    }
    default:
        return false;
    }
}

} // namespace

int run() {
    TerminalSession terminal;
    if (!terminal.ready()) {
        std::cerr << "Could not initialise terminal in raw mode.\n"
                  << "open-pods TUI needs an interactive TTY. Use --daemon for headless.\n";
        return 1;
    }

    TuiState state;

    {
        BluezDiscoveryBackend backend;
        const auto local_mac = backend.adapter_address();
        if (local_mac) {
            state.set_local_address(*local_mac);
        }
    }

    ipc::IpcClient client{
        serialize_device_command_envelope,
        deserialize_app_event,
    };
    if (!client.connect()) {
        std::ostringstream msg;
        msg << "\x1b[2J\x1b[H"
            << "\x1b[1mopen-pods\x1b[0m\r\n\r\n"
            << "Could not reach the daemon at " << ipc::socket_path().string() << ".\r\n\r\n"
            << "Start it with:\r\n"
            << "  \x1b[1mopen-pods --daemon\x1b[0m\r\n";
        TerminalSession::write_raw(msg.str());
        (void)terminal.read_key(5000);
        return 1;
    }

    MprisProbe mpris;
    PulseAudioBackend audio;
    auto last_media_refresh = std::chrono::steady_clock::time_point::min();

    auto redraw = [&]() {
        const auto sz = TerminalSession::size();
        const auto frame = render_frame(state, sz);
        TerminalSession::write_raw(frame);
    };

    redraw();
    state.log("ipc: connected to daemon");
    bool status_refresh_requested = false;

    auto has_startup_status = [&]() {
        const auto& dev = state.device();
        const bool has_battery = dev.left_battery
            || dev.right_battery
            || dev.case_battery
            || dev.headphone_battery;
        return has_battery && dev.listening_mode;
    };

    auto request_status_refresh = [&]() {
        const auto& dev = state.device();
        if (status_refresh_requested || dev.mac.empty() || has_startup_status()) {
            return;
        }

        if (client.send_command({dev.mac, DeviceCommand::refresh_battery()})) {
            state.log("status: refresh requested");
            status_refresh_requested = true;
        }
    };

    while (!g_shutdown_pending.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_media_refresh >= MEDIA_REFRESH_INTERVAL) {
            const auto media = mpris.probe();
            state.set_media_status(
                media.service,
                media.playback_status,
                media.track,
                media.artist
            );
            refresh_volume(audio, state.device().mac, state);
            last_media_refresh = now;
        }

        bool dirty = false;

        if (auto event = client.read_event_for(std::chrono::milliseconds{0})) {
            state.handle_event(*event);
            dirty = true;
            while (auto extra = client.read_event_for(std::chrono::milliseconds{0})) {
                state.handle_event(*extra);
            }
            request_status_refresh();
        }

        if (terminal.consume_resize_event()) {
            dirty = true;
            TerminalSession::write_raw("\x1b[2J");
        }

        if (auto key = terminal.read_key(IDLE_POLL_MS)) {
            const auto& seq = *key;

            // Quit
            if (seq == "q" || seq == "Q" || seq == "\x03" /* Ctrl-C */) {
                break;
            }

            // Section / row navigation -----------------------------------
            if (seq == "\t") {
                state.cycle_section(+1);
                dirty = true;
            } else if (seq == "\x1b[Z") {
                // Shift+Tab → cycle backwards
                state.cycle_section(-1);
                dirty = true;
            } else if (seq == "\x1b[A") {
                // ↑
                state.move_cursor(-1);
                dirty = true;
            } else if (seq == "\x1b[B") {
                // ↓
                state.move_cursor(+1);
                dirty = true;
            } else if (seq == "\x1b[D") {
                // ←
                switch (state.selected_section()) {
                case Section::NoiseControl:
                    apply_noise_row(client, state, -1);
                    break;
                case Section::Settings:
                    apply_settings_row(client, state, -1);
                    break;
                case Section::Battery:
                    break; // read-only
                }
                dirty = true;
            } else if (seq == "\x1b[C") {
                // →
                switch (state.selected_section()) {
                case Section::NoiseControl:
                    apply_noise_row(client, state, +1);
                    break;
                case Section::Settings:
                    apply_settings_row(client, state, +1);
                    break;
                case Section::Battery:
                    break;
                }
                dirty = true;
            } else if (seq == "\r" || seq == "\n") {
                // ENTER — apply / select
                switch (state.selected_section()) {
                case Section::NoiseControl:
                    apply_noise_row(client, state, 0);
                    break;
                case Section::Settings:
                    apply_settings_row(client, state, 0);
                    break;
                case Section::Battery:
                    break;
                }
                dirty = true;
            }

            // Reserved single-letter shortcuts ---------------------------
            else if (seq == "r" || seq == "R") {
                state.log("rename: TODO — implement R when ready");
                dirty = true;
            } else if (seq == "i" || seq == "I") {
                state.log("info: open-pods • Linux AirPods TUI");
                dirty = true;
            } else if (seq == " ") {
                // Reserved for future media play/pause; today still proxied
                // through MPRIS so the user gets immediate feedback.
                MprisMediaSessionController controller;
                controller.play_pause_first();
                state.log("media: play/pause toggled");
                dirty = true;
            }
        }

        if (dirty) {
            redraw();
        }
    }

    return 0;
}

} // namespace tui
