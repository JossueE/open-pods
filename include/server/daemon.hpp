#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "audio/audio_backend.hpp"
#include "bluetooth/avrcp_volume_monitor.hpp"
#include "bluetooth/bluez_connection_listener.hpp"
#include "bluetooth/discovery.hpp"
#include "bluetooth/managers.hpp"
#include "config/app_config.hpp"
#include "devices/airpods_device.hpp"
#include "media/media_session_controller.hpp"
#include "server/ipc.hpp"

struct AppEvent;
struct BatteryInfo;

/**
 * @brief Headless runtime coordinator for Bluetooth, IPC, and device managers.
 */
class Daemon {
public:
    explicit Daemon(AppConfig config);
    ~Daemon();

    /**
     * @brief Runs the daemon IPC loop.
     * @note Blocking until the IPC server exits.
     */
    bool run();

    /**
     * @brief Applies one app event to daemon state and publishes it to IPC clients.
     */
    void handle_event(const AppEvent& event);

    void register_device(std::string mac, DeviceManagers managers);
    void remove_device(const std::string& mac);

    std::shared_ptr<ipc::SnapshotStore> snapshot() const;

private:
    struct BatteryAlertKey {
        std::string mac;
        int component = 0;

        bool operator==(const BatteryAlertKey& other) const noexcept {
            return mac == other.mac && component == other.component;
        }
    };

    struct BatteryAlertHash {
        std::size_t operator()(const BatteryAlertKey& key) const noexcept {
            return std::hash<std::string>{}(key.mac)
                ^ (std::hash<int>{}(key.component) << 1);
        }
    };

    void handle_command(const ipc::DeviceCommandEnvelope& command);
    void start_bluetooth_runtime();
    void stop_bluetooth_runtime();
    void bluetooth_runtime_loop();
    void initialize_airpods(const BluetoothDeviceInfo& device);
    bool has_manager(const std::string& mac) const;
    void publish_battery_alerts(const std::string& mac, const std::vector<BatteryInfo>& batteries);
    std::string ensure_local_mac();

    AppConfig config_;
    std::shared_ptr<ipc::SnapshotStore> snapshot_;
    std::unique_ptr<ipc::IpcServer> ipc_server_;
    std::shared_ptr<AudioBackend> audio_backend_;
    std::shared_ptr<MediaSessionController> media_session_;
    std::unique_ptr<AvrcpVolumeMonitor> avrcp_monitor_;
    std::unique_ptr<BluezConnectionListener> connection_listener_;
    std::string local_mac_;
    mutable std::mutex managers_mutex_;
    std::unordered_map<std::string, DeviceManagers> device_managers_;
    std::unordered_map<std::string, std::unique_ptr<AirPodsDevice>> airpods_devices_;
    std::set<std::string> lost_connections_;
    std::unordered_map<BatteryAlertKey, int, BatteryAlertHash> battery_alerted_;
    std::mutex battery_alerted_mutex_;
    std::atomic_bool bluetooth_running_ {false};
    std::thread bluetooth_thread_;
};
