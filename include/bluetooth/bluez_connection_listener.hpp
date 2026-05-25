#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

/**
 * @brief Subscribes to BlueZ org.bluez.Device1 PropertiesChanged signals and
 *        invokes a callback when a device's `Connected` value flips.
 *
 * Notifications are delivered from a dedicated D-Bus thread. The callback is
 * invoked synchronously, so callers should not perform long-running work inline.
 */
class BluezConnectionListener {
public:
    using Callback = std::function<void(const std::string& mac, bool connected)>;

    explicit BluezConnectionListener(Callback callback);
    ~BluezConnectionListener();

    BluezConnectionListener(const BluezConnectionListener&) = delete;
    BluezConnectionListener& operator=(const BluezConnectionListener&) = delete;

    void start();
    void stop();

private:
    void run();

    Callback callback_;
    std::atomic_bool running_ {false};
    std::thread worker_;
};
