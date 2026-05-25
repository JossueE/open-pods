#include "bluetooth/avrcp_volume_monitor.hpp"

#include <dbus/dbus.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

struct DBusMessageDeleter {
    void operator()(DBusMessage* message) const noexcept {
        if (message != nullptr) {
            dbus_message_unref(message);
        }
    }
};

using DBusMessagePtr = std::unique_ptr<DBusMessage, DBusMessageDeleter>;

struct DBusConnectionDeleter {
    void operator()(DBusConnection* connection) const noexcept {
        if (connection != nullptr) {
            dbus_connection_unref(connection);
        }
    }
};

using DBusConnectionPtr = std::unique_ptr<DBusConnection, DBusConnectionDeleter>;

std::optional<uint16_t> read_u16_variant(DBusMessageIter& variant)
{
    const int type = dbus_message_iter_get_arg_type(&variant);
    switch (type) {
        case DBUS_TYPE_BYTE: {
            uint8_t value = 0;
            dbus_message_iter_get_basic(&variant, &value);
            return value;
        }
        case DBUS_TYPE_UINT16: {
            uint16_t value = 0;
            dbus_message_iter_get_basic(&variant, &value);
            return value;
        }
        case DBUS_TYPE_UINT32: {
            uint32_t value = 0;
            dbus_message_iter_get_basic(&variant, &value);
            return static_cast<uint16_t>(std::min<uint32_t>(value, 0xFFFFu));
        }
        case DBUS_TYPE_INT16: {
            int16_t value = 0;
            dbus_message_iter_get_basic(&variant, &value);
            return value < 0 ? std::optional<uint16_t>{} : std::optional<uint16_t>{static_cast<uint16_t>(value)};
        }
        case DBUS_TYPE_INT32: {
            int32_t value = 0;
            dbus_message_iter_get_basic(&variant, &value);
            if (value < 0 || value > 0xFFFF) {
                return std::nullopt;
            }
            return static_cast<uint16_t>(value);
        }
        default:
            return std::nullopt;
    }
}

std::optional<int> read_volume_from_changed(DBusMessage* message)
{
    DBusMessageIter root;
    if (!dbus_message_iter_init(message, &root)) {
        return std::nullopt;
    }

    if (dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_STRING) {
        return std::nullopt;
    }
    const char* iface = nullptr;
    dbus_message_iter_get_basic(&root, &iface);
    if (iface == nullptr || std::string{iface} != "org.bluez.MediaTransport1") {
        return std::nullopt;
    }

    if (!dbus_message_iter_next(&root)
        || dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
        return std::nullopt;
    }

    DBusMessageIter changed;
    dbus_message_iter_recurse(&root, &changed);

    while (dbus_message_iter_get_arg_type(&changed) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&changed, &entry);

        const char* key = nullptr;
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING) {
            dbus_message_iter_next(&changed);
            continue;
        }
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);

        if (key != nullptr && std::string{key} == "Volume"
            && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            DBusMessageIter variant;
            dbus_message_iter_recurse(&entry, &variant);
            const auto raw = read_u16_variant(variant);
            if (!raw) {
                return std::nullopt;
            }
            const double percentage = (static_cast<double>(*raw) / 127.0) * 100.0;
            return static_cast<int>(std::lround(percentage));
        }

        dbus_message_iter_next(&changed);
    }

    return std::nullopt;
}

} // namespace

AvrcpVolumeMonitor::AvrcpVolumeMonitor(AppConfig config)
    : config_(std::move(config))
{
}

AvrcpVolumeMonitor::~AvrcpVolumeMonitor()
{
    stop();
}

void AvrcpVolumeMonitor::start()
{
    if (running_) {
        return;
    }
    running_ = true;
    worker_ = std::thread([this]() {
        run();
    });
}

void AvrcpVolumeMonitor::stop()
{
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AvrcpVolumeMonitor::apply(int new_pct)
{
    if (applied_pct_ < 0) {
        applied_pct_ = new_pct;
        return;
    }
    if (new_pct == applied_pct_) {
        return;
    }

    char fraction[16];
    std::snprintf(fraction, sizeof(fraction), "%.4f", static_cast<double>(new_pct) / 100.0);
    config_.run_template_cmd(config_.volume_set_command(), fraction);
    config_.run_template_cmd(config_.volume_osd_command(), "+0");
    applied_pct_ = new_pct;
}

void AvrcpVolumeMonitor::run()
{
    DBusError error;
    dbus_error_init(&error);
    DBusConnectionPtr connection{dbus_bus_get_private(DBUS_BUS_SYSTEM, &error)};
    if (!connection || dbus_error_is_set(&error)) {
        std::cerr << "AVRCP monitor: cannot connect to system bus: "
                  << error.message << '\n';
        dbus_error_free(&error);
        return;
    }
    dbus_connection_set_exit_on_disconnect(connection.get(), FALSE);

    constexpr const char* match_rule =
        "type='signal',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged'";
    dbus_bus_add_match(connection.get(), match_rule, &error);
    dbus_connection_flush(connection.get());
    if (dbus_error_is_set(&error)) {
        std::cerr << "AVRCP monitor: failed to add match rule: "
                  << error.message << '\n';
        dbus_error_free(&error);
        dbus_connection_close(connection.get());
        return;
    }

    while (running_) {
        // Drive the dispatch loop with a 100ms timeout so we can react to the
        // running_ flag and to the debounce condvar promptly.
        if (!dbus_connection_read_write(connection.get(), 100)) {
            break;
        }

        while (running_) {
            DBusMessagePtr message{dbus_connection_pop_message(connection.get())};
            if (!message) {
                break;
            }

            if (dbus_message_get_type(message.get()) != DBUS_MESSAGE_TYPE_SIGNAL) {
                continue;
            }
            const char* path = dbus_message_get_path(message.get());
            if (path == nullptr || std::string{path}.find("/org/bluez/") == std::string::npos) {
                continue;
            }
            const auto pct = read_volume_from_changed(message.get());
            if (!pct) {
                continue;
            }

            {
                std::lock_guard lock{mutex_};
                pending_pct_ = *pct;
            }
            cv_.notify_all();
        }

        // Debounce window: wait up to DEBOUNCE for a quiet period; if no further
        // events arrive in the window, apply the latest pending value.
        std::unique_lock lock{mutex_};
        if (pending_pct_) {
            const auto status = cv_.wait_for(lock, DEBOUNCE);
            if (status == std::cv_status::timeout && pending_pct_) {
                const int target = *pending_pct_;
                pending_pct_.reset();
                lock.unlock();
                apply(target);
            }
        }
    }

    dbus_connection_close(connection.get());
    dbus_error_free(&error);
}
