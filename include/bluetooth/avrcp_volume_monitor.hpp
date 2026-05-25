#pragma once

#include "config/app_config.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

/**
 * @brief Listens for org.bluez.MediaTransport1.Volume property changes and runs
 *        the configured volume_set_command + volume_osd_command in response.
 *
 * The monitor runs a dedicated D-Bus thread because the libdbus dispatch loop is
 * single-threaded. AirPods stem swipes flood ~15 Volume events in quick
 * succession, so we debounce by 200ms and only apply the final absolute volume.
 */
class AvrcpVolumeMonitor {
public:
    explicit AvrcpVolumeMonitor(AppConfig config);
    ~AvrcpVolumeMonitor();

    AvrcpVolumeMonitor(const AvrcpVolumeMonitor&) = delete;
    AvrcpVolumeMonitor& operator=(const AvrcpVolumeMonitor&) = delete;

    void start();
    void stop();

private:
    static constexpr std::chrono::milliseconds DEBOUNCE {200};

    void run();
    void apply(int new_pct);

    AppConfig config_;
    std::atomic_bool running_ {false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<int> pending_pct_;
    int applied_pct_ {-1};
};
