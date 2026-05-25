#include "bluetooth/bluez_connection_listener.hpp"

#include <dbus/dbus.h>

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

/**
 * @brief Extracts a MAC address from a BlueZ object path like
 *        /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF.
 * @return The address with `_` replaced by `:`, or std::nullopt if the path is malformed.
 */
std::optional<std::string> mac_from_object_path(const char* path)
{
    if (path == nullptr) {
        return std::nullopt;
    }
    std::string p{path};
    const auto last = p.find_last_of('/');
    if (last == std::string::npos) {
        return std::nullopt;
    }
    std::string segment = p.substr(last + 1);
    if (!segment.starts_with("dev_")) {
        return std::nullopt;
    }
    segment.erase(0, 4);
    for (char& ch : segment) {
        if (ch == '_') {
            ch = ':';
        }
    }
    return segment;
}

std::optional<bool> read_connected_from_changed(DBusMessage* message)
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
    if (iface == nullptr || std::string{iface} != "org.bluez.Device1") {
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

        if (key != nullptr && std::string{key} == "Connected"
            && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            DBusMessageIter variant;
            dbus_message_iter_recurse(&entry, &variant);
            if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_BOOLEAN) {
                return std::nullopt;
            }
            dbus_bool_t value = FALSE;
            dbus_message_iter_get_basic(&variant, &value);
            return value != FALSE;
        }

        dbus_message_iter_next(&changed);
    }

    return std::nullopt;
}

} // namespace

BluezConnectionListener::BluezConnectionListener(Callback callback)
    : callback_(std::move(callback))
{
}

BluezConnectionListener::~BluezConnectionListener()
{
    stop();
}

void BluezConnectionListener::start()
{
    if (running_) {
        return;
    }
    running_ = true;
    worker_ = std::thread([this]() {
        run();
    });
}

void BluezConnectionListener::stop()
{
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void BluezConnectionListener::run()
{
    DBusError error;
    dbus_error_init(&error);
    DBusConnectionPtr connection{dbus_bus_get_private(DBUS_BUS_SYSTEM, &error)};
    if (!connection || dbus_error_is_set(&error)) {
        std::cerr << "BlueZ connection listener: cannot connect to system bus: "
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
        std::cerr << "BlueZ connection listener: failed to add match rule: "
                  << error.message << '\n';
        dbus_error_free(&error);
        dbus_connection_close(connection.get());
        return;
    }

    while (running_) {
        if (!dbus_connection_read_write(connection.get(), 250)) {
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
            const auto mac = mac_from_object_path(path);
            if (!mac) {
                continue;
            }

            const auto connected = read_connected_from_changed(message.get());
            if (!connected) {
                continue;
            }

            if (callback_) {
                callback_(*mac, *connected);
            }
        }
    }

    dbus_connection_close(connection.get());
    dbus_error_free(&error);
}
