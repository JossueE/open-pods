#include "devices/airpods_device.hpp"

#include <chrono>
#include <iostream>
#include <utility>

#include "bluetooth/aacp.hpp"
#include "bluetooth/aacp_manager.hpp"
#include "devices/apple_models.hpp"
#include "media/media_controller.hpp"

namespace {

template <typename... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace

AirPodsDevice::~AirPodsDevice()
{
    // Clear the AACP callbacks before this object is destroyed; the receive
    // thread on the AACPManager (which may outlive this device when shared via
    // DeviceManagers) holds lambdas that capture `this` and would otherwise
    // dereference freed memory.
    if (aacp_manager_) {
        aacp_manager_->set_event_channel({});
    }
    if (media_controller_) {
        media_controller_->stop_playback_listener();
    }
}

bool AirPodsDevice::start()
{
    if (!aacp_manager_) {
        std::cerr << "Cannot start AirPodsDevice: AACP manager is not configured\n";
        return false;
    }

    aacp_manager_->set_event_channel([this](const AACPEvent& event) {
        if (app_event_sink_) {
            app_event_sink_(AppEvent::aacp_event(mac_address_, event));
        }

        std::visit(Overloaded{
            [](const std::vector<BatteryInfo>&) {
            },
            [](const ControlCommandStatus&) {
                // Control command subscribers handle the actionable control state.
            },
            [this](const EarDetection& ear_detection) {
                if (!media_controller_) {
                    return;
                }
                media_controller_->handle_ear_detection(
                    ear_detection.old_left,
                    ear_detection.old_right,
                    ear_detection.new_left,
                    ear_detection.new_right
                );
            },
            [this](const ConversationalAwareness& awareness) {
                if (!media_controller_) {
                    return;
                }
                media_controller_->handle_conversational_awareness(awareness.status);
            },
            [this](const AudioSource& source) {
                if (!media_controller_) {
                    return;
                }
                media_controller_->handle_audio_source_change(source);
            },
            [](const ConnectedDevices&) {
            },
            [this](const OwnershipToFalseRequest&) {
                aacp_manager_->send_control_command(
                    ControlCommandIdentifiers::OwnsConnection,
                    {0x00}
                );
                if (!media_controller_) {
                    return;
                }
                media_controller_->pause_without_remembering();
                media_controller_->deactivate_a2dp_profile();
            },
            [this](const StemPress& stem_press) {
                if (!media_controller_) {
                    return;
                }
                switch (stem_press.type) {
                case StemPressType::SingleTap:
                    media_controller_->toggle_play_pause();
                    break;
                case StemPressType::DoubleTap:
                    media_controller_->next_track();
                    break;
                case StemPressType::TripleTap:
                    media_controller_->previous_track();
                    break;
                case StemPressType::LongPress:
                    break;
                }
            },
            [](const EqualizerData&) {
            },
            [](const ConnectionLost&) {
            },
            [](const AirPodsInformation&) {
                // The daemon persists this to devices.json via the app event sink.
            },
        }, event);
    });

    // Subscriptions for control-command identifiers we want to track in the
    // app state. Updates are propagated to clients via the AACPEvent channel
    // (CONTROL_COMMAND opcode handler emits ControlCommandStatus); these
    // subscribers exist purely so that initial values delivered via the
    // subscribe-replay path are observed and pushed into the daemon snapshot.
    for (const auto command : {
        ControlCommandIdentifiers::ListeningMode,
        ControlCommandIdentifiers::AllowOffOption,
        ControlCommandIdentifiers::ConversationDetectConfig,
        ControlCommandIdentifiers::OneBudAncMode,
        ControlCommandIdentifiers::VolumeSwipeMode,
        ControlCommandIdentifiers::AdaptiveVolumeConfig,
        ControlCommandIdentifiers::AllowAutoConnect,
        ControlCommandIdentifiers::DoubleClickInterval,
        ControlCommandIdentifiers::ClickHoldInterval,
        ControlCommandIdentifiers::ChimeVolume,
        ControlCommandIdentifiers::VolumeSwipeInterval,
        ControlCommandIdentifiers::AutoAncStrength,
        ControlCommandIdentifiers::MicMode,
    }) {
        aacp_manager_->subscribe_to_control_command(
            command,
            [this, command](const std::vector<uint8_t>& value) {
                if (!app_event_sink_) {
                    return;
                }
                app_event_sink_(AppEvent::aacp_event(
                    mac_address_,
                    AACPEvent{ControlCommandStatus{command, value}}
                ));
            }
        );
    }

    aacp_manager_->subscribe_to_control_command(
        ControlCommandIdentifiers::OwnsConnection,
        [this](const std::vector<uint8_t>& value) {
            const bool owns = !value.empty() && value.front() != 0;
            if (!owns && media_controller_) {
                media_controller_->pause_without_remembering();
            }
        }
    );

    if (!aacp_manager_->connect(mac_address_)) {
        return false;
    }

    bool ok = true;
    ok = aacp_manager_->send_handshake() && ok;
    (void)aacp_manager_->wait_for_any_opcode(std::chrono::milliseconds{500});
    ok = aacp_manager_->send_set_feature_flags_packet() && ok;
    (void)aacp_manager_->wait_for_opcode(opcodes::SET_FEATURE_FLAGS, std::chrono::milliseconds{500});
    ok = aacp_manager_->send_notification_request() && ok;
    (void)aacp_manager_->wait_for_opcode(opcodes::REQUEST_NOTIFICATIONS, std::chrono::milliseconds{500});
    ok = aacp_manager_->send_ssl_request() && ok;

    AppleModels apple_models;
    if (apple_models.needs_aap_init_ext(product_id_)) {
        (void)aacp_manager_->wait_for_opcode(opcodes::SET_FEATURE_FLAGS, std::chrono::milliseconds{500});
        ok = aacp_manager_->send_init_ext() && ok;
    }

    ok = aacp_manager_->send_proximity_keys_request({
        ProximityKeyType::Irk,
        ProximityKeyType::EncKey,
    }) && ok;
    (void)aacp_manager_->wait_for_opcode(opcodes::PROXIMITY_KEYS_RSP, std::chrono::milliseconds{500});

    return ok;
}
