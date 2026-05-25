#include "bluetooth/bluez_discovery.hpp"

#include <dbus/dbus.h>

#include <memory>
#include <string>
#include <vector>

namespace {

struct DBusErrorDeleter {
    void operator()(DBusError* error) const {
        if (error != nullptr) {
            dbus_error_free(error);
        }
    }
};

struct DBusConnectionDeleter {
    void operator()(DBusConnection* connection) const {
        if (connection != nullptr) {
            dbus_connection_unref(connection);
        }
    }
};

struct DBusMessageDeleter {
    void operator()(DBusMessage* message) const {
        if (message != nullptr) {
            dbus_message_unref(message);
        }
    }
};

using DBusConnectionPtr = std::unique_ptr<DBusConnection, DBusConnectionDeleter>;
using DBusMessagePtr = std::unique_ptr<DBusMessage, DBusMessageDeleter>;

std::string read_string_variant(DBusMessageIter& variant) {
    if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_STRING) {
        return {};
    }

    const char* value = nullptr;
    dbus_message_iter_get_basic(&variant, &value);
    return value != nullptr ? std::string{value} : std::string{};
}

bool read_bool_variant(DBusMessageIter& variant) {
    if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_BOOLEAN) {
        return false;
    }

    dbus_bool_t value = false;
    dbus_message_iter_get_basic(&variant, &value);
    return value != 0;
}

std::vector<std::string> read_string_array_variant(DBusMessageIter& variant) {
    std::vector<std::string> values;
    if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY) {
        return values;
    }

    DBusMessageIter array;
    dbus_message_iter_recurse(&variant, &array);

    while (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
            const char* value = nullptr;
            dbus_message_iter_get_basic(&array, &value);
            if (value != nullptr) {
                values.emplace_back(value);
            }
        }
        dbus_message_iter_next(&array);
    }

    return values;
}

void read_device_property(
    BluetoothDeviceInfo& device,
    const std::string& property_name,
    DBusMessageIter& variant
) {
    if (property_name == "Address") {
        device.address = read_string_variant(variant);
    } else if (property_name == "Name" || property_name == "Alias") {
        const std::string value = read_string_variant(variant);
        if (!value.empty()) {
            device.name = value;
        }
    } else if (property_name == "Modalias") {
        device.modalias = read_string_variant(variant);
    } else if (property_name == "Connected") {
        device.connected = read_bool_variant(variant);
    } else if (property_name == "UUIDs") {
        device.uuids = read_string_array_variant(variant);
    }
}

void read_device_properties(BluetoothDeviceInfo& device, DBusMessageIter& properties) {
    while (dbus_message_iter_get_arg_type(&properties) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(&properties) != DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_next(&properties);
            continue;
        }

        DBusMessageIter entry;
        dbus_message_iter_recurse(&properties, &entry);

        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING) {
            dbus_message_iter_next(&properties);
            continue;
        }

        const char* property_name = nullptr;
        dbus_message_iter_get_basic(&entry, &property_name);
        dbus_message_iter_next(&entry);

        if (property_name == nullptr
            || dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
            dbus_message_iter_next(&properties);
            continue;
        }

        DBusMessageIter variant;
        dbus_message_iter_recurse(&entry, &variant);
        read_device_property(device, property_name, variant);

        dbus_message_iter_next(&properties);
    }
}

std::optional<BluetoothDeviceInfo> read_device_interface(DBusMessageIter& interfaces) {
    while (dbus_message_iter_get_arg_type(&interfaces) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(&interfaces) != DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_next(&interfaces);
            continue;
        }

        DBusMessageIter interface_entry;
        dbus_message_iter_recurse(&interfaces, &interface_entry);

        if (dbus_message_iter_get_arg_type(&interface_entry) != DBUS_TYPE_STRING) {
            dbus_message_iter_next(&interfaces);
            continue;
        }

        const char* interface_name = nullptr;
        dbus_message_iter_get_basic(&interface_entry, &interface_name);
        dbus_message_iter_next(&interface_entry);

        if (interface_name == nullptr
            || std::string{interface_name} != "org.bluez.Device1"
            || dbus_message_iter_get_arg_type(&interface_entry) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&interfaces);
            continue;
        }

        BluetoothDeviceInfo device;
        DBusMessageIter properties;
        dbus_message_iter_recurse(&interface_entry, &properties);
        read_device_properties(device, properties);
        return device;
    }

    return std::nullopt;
}

} // namespace

namespace {

std::string mac_to_object_path_segment(const std::string& mac) {
    std::string result;
    result.reserve(4 + mac.size());
    result.append("dev_");
    for (char ch : mac) {
        result.push_back(ch == ':' ? '_' : ch);
    }
    return result;
}

} // namespace

std::optional<std::string> BluezDiscoveryBackend::adapter_address() {
    DBusError error;
    dbus_error_init(&error);
    std::unique_ptr<DBusError, DBusErrorDeleter> error_guard{&error};

    DBusConnectionPtr connection{dbus_bus_get(DBUS_BUS_SYSTEM, &error)};
    if (dbus_error_is_set(&error) || !connection) {
        return std::nullopt;
    }

    DBusMessagePtr message{
        dbus_message_new_method_call(
            "org.bluez",
            "/org/bluez/hci0",
            "org.freedesktop.DBus.Properties",
            "Get"
        )
    };
    if (!message) {
        return std::nullopt;
    }

    const char* iface = "org.bluez.Adapter1";
    const char* property = "Address";
    if (!dbus_message_append_args(
            message.get(),
            DBUS_TYPE_STRING, &iface,
            DBUS_TYPE_STRING, &property,
            DBUS_TYPE_INVALID
        )) {
        return std::nullopt;
    }

    DBusMessagePtr reply{
        dbus_connection_send_with_reply_and_block(
            connection.get(),
            message.get(),
            2000,
            &error
        )
    };
    if (dbus_error_is_set(&error) || !reply) {
        return std::nullopt;
    }

    DBusMessageIter root;
    if (!dbus_message_iter_init(reply.get(), &root)
        || dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_VARIANT) {
        return std::nullopt;
    }

    DBusMessageIter variant;
    dbus_message_iter_recurse(&root, &variant);
    return read_string_variant(variant);
}

bool BluezDiscoveryBackend::set_device_alias(
    const std::string& mac_address,
    const std::string& alias
) {
    DBusError error;
    dbus_error_init(&error);
    std::unique_ptr<DBusError, DBusErrorDeleter> error_guard{&error};

    DBusConnectionPtr connection{dbus_bus_get(DBUS_BUS_SYSTEM, &error)};
    if (dbus_error_is_set(&error) || !connection) {
        return false;
    }

    const std::string object_path = std::string{"/org/bluez/hci0/"} + mac_to_object_path_segment(mac_address);

    DBusMessagePtr message{
        dbus_message_new_method_call(
            "org.bluez",
            object_path.c_str(),
            "org.freedesktop.DBus.Properties",
            "Set"
        )
    };
    if (!message) {
        return false;
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(message.get(), &args);

    const char* iface = "org.bluez.Device1";
    const char* property = "Alias";
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &iface)
        || !dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &property)) {
        return false;
    }

    DBusMessageIter variant;
    if (!dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "s", &variant)) {
        return false;
    }

    const char* alias_value = alias.c_str();
    const bool appended = dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &alias_value);
    dbus_message_iter_close_container(&args, &variant);
    if (!appended) {
        return false;
    }

    DBusMessagePtr reply{
        dbus_connection_send_with_reply_and_block(
            connection.get(),
            message.get(),
            2000,
            &error
        )
    };
    return reply != nullptr && !dbus_error_is_set(&error);
}

std::vector<BluetoothDeviceInfo> BluezDiscoveryBackend::devices() {
    DBusError error;
    dbus_error_init(&error);
    std::unique_ptr<DBusError, DBusErrorDeleter> error_guard{&error};

    DBusConnectionPtr connection{
        dbus_bus_get(DBUS_BUS_SYSTEM, &error)
    };
    if (dbus_error_is_set(&error) || !connection) {
        return {};
    }

    DBusMessagePtr message{
        dbus_message_new_method_call(
            "org.bluez",
            "/",
            "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects"
        )
    };
    if (!message) {
        return {};
    }

    DBusMessagePtr reply{
        dbus_connection_send_with_reply_and_block(
            connection.get(),
            message.get(),
            5000,
            &error
        )
    };
    if (dbus_error_is_set(&error) || !reply) {
        return {};
    }

    DBusMessageIter root;
    if (!dbus_message_iter_init(reply.get(), &root)
        || dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
        return {};
    }

    std::vector<BluetoothDeviceInfo> devices;
    DBusMessageIter objects;
    dbus_message_iter_recurse(&root, &objects);

    while (dbus_message_iter_get_arg_type(&objects) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(&objects) != DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_next(&objects);
            continue;
        }

        DBusMessageIter object_entry;
        dbus_message_iter_recurse(&objects, &object_entry);

        if (dbus_message_iter_get_arg_type(&object_entry) != DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_next(&objects);
            continue;
        }

        dbus_message_iter_next(&object_entry);
        if (dbus_message_iter_get_arg_type(&object_entry) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&objects);
            continue;
        }

        DBusMessageIter interfaces;
        dbus_message_iter_recurse(&object_entry, &interfaces);
        auto device = read_device_interface(interfaces);
        if (device && !device->address.empty()) {
            devices.push_back(std::move(*device));
        }

        dbus_message_iter_next(&objects);
    }

    return devices;
}
