#include "server/daemon.hpp"

#include "audio/pulse_audio/pulse_audio_backend.hpp"
#include "bluetooth/aacp_manager.hpp"
#include "bluetooth/bluez_discovery.hpp"
#include "bluetooth/bluez_l2cap_transport.hpp"
#include "devices/apple_models.hpp"
#include "media/media_controller.hpp"
#include "media/mpris_media_session.hpp"
#include "server/app_event.hpp"
#include "utils/devices.hpp"
#include "utils/runtime.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::string component_label(BatteryComponent component) {
    switch (component) {
        case BatteryComponent::LeftBud: return "Left";
        case BatteryComponent::RightBud: return "Right";
        case BatteryComponent::Case: return "Case";
        case BatteryComponent::Headphone: return "Headphone";
    }
    return "Unknown";
}

void update_battery_env_file(const std::vector<BatteryInfo>& batteries) {
    std::optional<uint8_t> left;
    std::optional<uint8_t> right;
    std::optional<uint8_t> case_battery;
    std::optional<uint8_t> headphone;

    for (const BatteryInfo& battery : batteries) {
        switch (battery.component) {
            case BatteryComponent::LeftBud:
                left = battery.level;
                break;
            case BatteryComponent::RightBud:
                right = battery.level;
                break;
            case BatteryComponent::Case:
                if (battery.status != BatteryStatus::Disconnected) {
                    case_battery = battery.level;
                }
                break;
            case BatteryComponent::Headphone:
                headphone = battery.level;
                break;
        }
    }

    utils::write_battery_env(left, right, case_battery, headphone);
}

} // namespace

Daemon::Daemon(AppConfig config)
    : config_(std::move(config))
    , snapshot_(std::make_shared<ipc::SnapshotStore>())
    , audio_backend_(std::make_shared<PulseAudioBackend>())
    , media_session_(std::make_shared<MprisMediaSessionController>())
{
    ipc_server_ = std::make_unique<ipc::IpcServer>(
        snapshot_,
        [this](const ipc::DeviceCommandEnvelope& command) {
            handle_command(command);
        },
        serialize_app_event,
        deserialize_device_command
    );

    avrcp_monitor_ = std::make_unique<AvrcpVolumeMonitor>(config_);
    connection_listener_ = std::make_unique<BluezConnectionListener>(
        [this](const std::string& mac, bool connected) {
            // Only schedule a teardown for known devices; the bluetooth thread
            // re-scans on every loop and will pick up new devices on its own.
            if (connected) {
                return;
            }
            if (!has_manager(mac)) {
                return;
            }
            remove_device(mac);
            handle_event(AppEvent::device_disconnected(mac));
        }
    );
}

Daemon::~Daemon() {
    stop_bluetooth_runtime();
}

bool Daemon::run() {
    if (!ipc_server_) {
        return false;
    }

    if (avrcp_monitor_) {
        avrcp_monitor_->start();
    }
    if (connection_listener_) {
        connection_listener_->start();
    }

    start_bluetooth_runtime();
    const bool ok = ipc_server_->run();

    stop_bluetooth_runtime();
    if (connection_listener_) {
        connection_listener_->stop();
    }
    if (avrcp_monitor_) {
        avrcp_monitor_->stop();
    }

    // Tear down devices before the audio backend / media session shared_ptrs
    // disappear, so MediaController shutdown still has somewhere to dispatch.
    {
        std::vector<std::string> to_remove;
        {
            std::lock_guard lock{managers_mutex_};
            to_remove.reserve(airpods_devices_.size());
            for (const auto& [mac, _] : airpods_devices_) {
                to_remove.push_back(mac);
            }
        }
        for (const auto& mac : to_remove) {
            remove_device(mac);
        }
    }

    return ok;
}

void Daemon::handle_event(const AppEvent& event) {
    if (const auto* aacp = std::get_if<AacpAppEvent>(&event.payload)) {
        if (std::holds_alternative<ConnectionLost>(aacp->event)) {
            std::lock_guard lock{managers_mutex_};
            lost_connections_.insert(aacp->mac);
        }

        if (const auto* batteries = std::get_if<std::vector<BatteryInfo>>(&aacp->event)) {
            update_battery_env_file(*batteries);
            publish_battery_alerts(aacp->mac, *batteries);
        }

        if (const auto* info = std::get_if<AirPodsInformation>(&aacp->event)) {
            utils::persist_device_information(aacp->mac, *info);
        }
    }

    if (ipc_server_) {
        ipc_server_->broadcast(event);
        return;
    }

    if (snapshot_) {
        snapshot_->update(event);
    }
}

void Daemon::register_device(std::string mac, DeviceManagers managers) {
    std::lock_guard lock{managers_mutex_};
    device_managers_[std::move(mac)] = std::move(managers);
}

void Daemon::remove_device(const std::string& mac) {
    std::unique_ptr<AirPodsDevice> device_to_destroy;
    DeviceManagers managers_to_destroy;

    {
        std::lock_guard lock{managers_mutex_};
        if (auto it = airpods_devices_.find(mac); it != airpods_devices_.end()) {
            device_to_destroy = std::move(it->second);
            airpods_devices_.erase(it);
        }
        if (auto it = device_managers_.find(mac); it != device_managers_.end()) {
            managers_to_destroy = std::move(it->second);
            device_managers_.erase(it);
        }
        lost_connections_.erase(mac);
    }

    {
        std::lock_guard alert_lock{battery_alerted_mutex_};
        for (auto it = battery_alerted_.begin(); it != battery_alerted_.end();) {
            if (it->first.mac == mac) {
                it = battery_alerted_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // device_to_destroy and managers_to_destroy fall out of scope here, releasing
    // the AACP receive thread and MediaController under no daemon lock.
}

std::shared_ptr<ipc::SnapshotStore> Daemon::snapshot() const {
    return snapshot_;
}

void Daemon::handle_command(const ipc::DeviceCommandEnvelope& command) {
    const auto& [mac, device_command] = command;

    std::shared_ptr<AACPManager> aacp;
    std::shared_ptr<MediaController> media;
    {
        std::lock_guard lock{managers_mutex_};
        const auto manager_it = device_managers_.find(mac);
        if (manager_it == device_managers_.end()) {
            return;
        }
        aacp = manager_it->second.get_aacp();
        if (const auto device_it = airpods_devices_.find(mac);
            device_it != airpods_devices_.end()) {
            media = device_it->second->media_controller();
        }
    }

    if (!aacp) {
        return;
    }

    if (const auto* control =
            std::get_if<ControlCommandDeviceCommand>(&device_command.payload)) {
        aacp->send_control_command(control->identifier, control->value);
        return;
    }

    if (const auto* rename = std::get_if<RenameDeviceCommand>(&device_command.payload)) {
        aacp->send_rename_packet(rename->name);
        utils::persist_device_name(mac, rename->name);

        // Setting the BlueZ alias makes the renamed device persist across
        // disconnects and prevents the iPhone from reclaiming the original name.
        BluezDiscoveryBackend backend;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (backend.set_device_alias(mac, rename->name)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
        return;
    }

    if (std::holds_alternative<RefreshBatteryDeviceCommand>(device_command.payload)) {
        aacp->send_notification_request();
        return;
    }

    if (std::holds_alternative<ReclaimAudioDeviceCommand>(device_command.payload)) {
        if (media) {
            media->reclaim_audio_source();
        }
        return;
    }
}

void Daemon::start_bluetooth_runtime() {
    if (bluetooth_running_) {
        return;
    }

    bluetooth_running_ = true;
    bluetooth_thread_ = std::thread([this]() {
        bluetooth_runtime_loop();
    });
}

void Daemon::stop_bluetooth_runtime() {
    bluetooth_running_ = false;
    if (bluetooth_thread_.joinable()) {
        bluetooth_thread_.join();
    }
}

bool Daemon::has_manager(const std::string& mac) const {
    std::lock_guard lock{managers_mutex_};
    return device_managers_.contains(mac);
}

std::string Daemon::ensure_local_mac() {
    if (!local_mac_.empty()) {
        return local_mac_;
    }

    BluezDiscoveryBackend backend;
    if (auto address = backend.adapter_address()) {
        local_mac_ = std::move(*address);
    }

    return local_mac_;
}

void Daemon::publish_battery_alerts(
    const std::string& mac,
    const std::vector<BatteryInfo>& batteries
) {
    if (config_.battery_alert_command().empty()) {
        return;
    }

    std::lock_guard lock{battery_alerted_mutex_};

    for (const BatteryInfo& battery : batteries) {
        if (battery.status != BatteryStatus::NotCharging) {
            continue;
        }

        const BatteryAlertKey key{mac, static_cast<int>(static_cast<uint8_t>(battery.component))};
        const int previous = battery_alerted_.contains(key)
            ? battery_alerted_.at(key)
            : 100;

        int threshold = 0;
        if (battery.level <= 10) {
            threshold = 10;
        } else if (battery.level <= 20) {
            threshold = 20;
        }

        if (threshold > 0 && threshold < previous) {
            battery_alerted_[key] = threshold;
            const std::string message = component_label(battery.component)
                + " battery: "
                + std::to_string(battery.level)
                + "%";
            config_.run_template_cmd(config_.battery_alert_command(), message);
        } else if (threshold == 0 && previous < 100) {
            battery_alerted_[key] = 100;
        }
    }
}

void Daemon::bluetooth_runtime_loop() {
    BluezDiscoveryBackend backend;

    while (bluetooth_running_) {
        std::set<std::string> connected_airpods;

        for (const auto& device : backend.devices()) {
            const auto has_airpods_uuid = std::any_of(
                device.uuids.begin(),
                device.uuids.end(),
                [](const std::string& uuid) {
                    return uuid.size() == AIRPODS_SERVICE_UUID.size()
                        && std::equal(
                            uuid.begin(),
                            uuid.end(),
                            AIRPODS_SERVICE_UUID.begin(),
                            [](char a, char b) {
                                return std::tolower(static_cast<unsigned char>(a))
                                    == std::tolower(static_cast<unsigned char>(b));
                            }
                        );
                }
            );

            if (!device.connected || !has_airpods_uuid || device.address.empty()) {
                continue;
            }

            connected_airpods.insert(device.address);
            bool needs_init = !has_manager(device.address);
            {
                std::lock_guard lock{managers_mutex_};
                needs_init = needs_init || lost_connections_.contains(device.address);
            }

            if (needs_init) {
                remove_device(device.address);
                std::this_thread::sleep_for(std::chrono::seconds{2});
                initialize_airpods(device);
            }
        }

        std::vector<std::string> disconnected;
        {
            std::lock_guard lock{managers_mutex_};
            for (const auto& [mac, _] : device_managers_) {
                if (!connected_airpods.contains(mac)) {
                    disconnected.push_back(mac);
                }
            }
        }

        for (const auto& mac : disconnected) {
            remove_device(mac);
            handle_event(AppEvent::device_disconnected(mac));
        }

        for (int i = 0; i < 20 && bluetooth_running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }
}

void Daemon::initialize_airpods(const BluetoothDeviceInfo& device) {
    {
        std::lock_guard lock{managers_mutex_};
        if (device_managers_.contains(device.address)) {
            return;
        }
        device_managers_[device.address] = DeviceManagers::placeholder();
    }

    AppleModels models;
    uint16_t product_id = 0;
    if (const auto parsed = models.parse_modalias(device.modalias);
        parsed && parsed->first == APPLE_COMPANY_ID) {
        product_id = parsed->second;
    }

    auto manager = std::make_shared<AACPManager>(
        std::make_shared<BluezL2capTransport>()
    );

    const std::string local_mac = ensure_local_mac();
    auto media_controller = std::make_shared<MediaController>(
        device.address,
        local_mac,
        audio_backend_,
        media_session_,
        config_
    );

    auto airpods = std::make_unique<AirPodsDevice>(
        device.address,
        product_id,
        config_,
        manager,
        media_controller,
        [this](const AppEvent& event) {
            handle_event(event);
        }
    );

    if (!airpods->start()) {
        remove_device(device.address);
        return;
    }

    media_controller->start_playback_listener(manager);

    {
        std::lock_guard lock{managers_mutex_};
        device_managers_[device.address] = DeviceManagers::with_aacp(manager);
        airpods_devices_[device.address] = std::move(airpods);
    }

    const std::string name = !device.name.empty() ? device.name : "AirPods";
    handle_event(AppEvent::device_connected(device.address, name, product_id));
}
