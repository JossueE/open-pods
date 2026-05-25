#include "media/mpris_media_session.hpp"

#include "utils/logging.hpp"

#include <dbus/dbus.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct DBusErrorGuard {
    DBusError error {};

    DBusErrorGuard() {
        dbus_error_init(&error);
    }

    ~DBusErrorGuard() {
        dbus_error_free(&error);
    }

    DBusErrorGuard(const DBusErrorGuard&) = delete;
    DBusErrorGuard& operator=(const DBusErrorGuard&) = delete;
};

struct DBusConnectionDeleter {
    void operator()(DBusConnection* connection) const noexcept {
        if (connection != nullptr) {
            // Session bus references are private connections (see open_private below).
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
        }
    }
};

struct DBusMessageDeleter {
    void operator()(DBusMessage* message) const noexcept {
        if (message != nullptr) {
            dbus_message_unref(message);
        }
    }
};

using DBusConnectionPtr = std::unique_ptr<DBusConnection, DBusConnectionDeleter>;
using DBusMessagePtr = std::unique_ptr<DBusMessage, DBusMessageDeleter>;

constexpr const char* MPRIS_PREFIX = "org.mpris.MediaPlayer2.";
constexpr const char* MPRIS_PATH = "/org/mpris/MediaPlayer2";
constexpr const char* MPRIS_PLAYER_IFACE = "org.mpris.MediaPlayer2.Player";

bool is_kdeconnect_service(std::string_view service) {
    return service.starts_with("org.mpris.MediaPlayer2.kdeconnect.mpris_");
}

DBusConnectionPtr open_session_bus() {
    DBusErrorGuard error_guard;
    // Use a private connection so closing the connection on shutdown does not
    // affect the shared bus state. The default dbus_bus_get returns a connection
    // that, when unref'd, asserts in the daemon thread.
    DBusConnectionPtr connection{
        dbus_bus_get_private(DBUS_BUS_SESSION, &error_guard.error)
    };
    if (!connection || dbus_error_is_set(&error_guard.error)) {
        std::cerr << "MPRIS: failed to open session bus: "
                  << error_guard.error.message << '\n';
        return {};
    }

    dbus_connection_set_exit_on_disconnect(connection.get(), FALSE);
    return connection;
}

std::vector<std::string> list_names(DBusConnection* connection) {
    std::vector<std::string> names;

    DBusMessagePtr message{
        dbus_message_new_method_call(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "ListNames"
        )
    };
    if (!message) {
        return names;
    }

    DBusErrorGuard error_guard;
    DBusMessagePtr reply{
        dbus_connection_send_with_reply_and_block(
            connection,
            message.get(),
            2000,
            &error_guard.error
        )
    };
    if (!reply || dbus_error_is_set(&error_guard.error)) {
        return names;
    }

    DBusMessageIter root;
    if (!dbus_message_iter_init(reply.get(), &root)
        || dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
        return names;
    }

    DBusMessageIter array;
    dbus_message_iter_recurse(&root, &array);
    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
        const char* value = nullptr;
        dbus_message_iter_get_basic(&array, &value);
        if (value != nullptr) {
            names.emplace_back(value);
        }
        dbus_message_iter_next(&array);
    }

    return names;
}

std::vector<std::string> filter_mpris_services(
    const std::vector<std::string>& names
) {
    std::vector<std::string> services;
    services.reserve(names.size());
    for (const auto& name : names) {
        if (name.starts_with(MPRIS_PREFIX) && !is_kdeconnect_service(name)) {
            services.push_back(name);
        }
    }
    return services;
}

std::optional<std::string> get_playback_status(
    DBusConnection* connection,
    const std::string& service
) {
    DBusMessagePtr message{
        dbus_message_new_method_call(
            service.c_str(),
            MPRIS_PATH,
            "org.freedesktop.DBus.Properties",
            "Get"
        )
    };
    if (!message) {
        return std::nullopt;
    }

    const char* iface = MPRIS_PLAYER_IFACE;
    const char* property = "PlaybackStatus";
    if (!dbus_message_append_args(
            message.get(),
            DBUS_TYPE_STRING, &iface,
            DBUS_TYPE_STRING, &property,
            DBUS_TYPE_INVALID
        )) {
        return std::nullopt;
    }

    DBusErrorGuard error_guard;
    DBusMessagePtr reply{
        dbus_connection_send_with_reply_and_block(
            connection,
            message.get(),
            1500,
            &error_guard.error
        )
    };
    if (!reply || dbus_error_is_set(&error_guard.error)) {
        return std::nullopt;
    }

    DBusMessageIter root;
    if (!dbus_message_iter_init(reply.get(), &root)
        || dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_VARIANT) {
        return std::nullopt;
    }

    DBusMessageIter variant;
    dbus_message_iter_recurse(&root, &variant);
    if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_STRING) {
        return std::nullopt;
    }

    const char* status = nullptr;
    dbus_message_iter_get_basic(&variant, &status);
    if (status == nullptr) {
        return std::nullopt;
    }

    return std::string{status};
}

bool call_player_method(
    DBusConnection* connection,
    const std::string& service,
    const char* method
) {
    DBusMessagePtr message{
        dbus_message_new_method_call(
            service.c_str(),
            MPRIS_PATH,
            MPRIS_PLAYER_IFACE,
            method
        )
    };
    if (!message) {
        return false;
    }

    // Fire-and-forget; some players reply asynchronously and blocking can
    // deadlock if the player itself is paused on D-Bus. Send + flush is enough
    // because we don't need the reply.
    if (!dbus_connection_send(connection, message.get(), nullptr)) {
        return false;
    }
    dbus_connection_flush(connection);
    return true;
}

} // namespace

struct MprisMediaSessionController::Impl {
    DBusConnectionPtr connection;
};

MprisMediaSessionController::MprisMediaSessionController()
    : impl_(std::make_unique<Impl>())
{
    impl_->connection = open_session_bus();
}

MprisMediaSessionController::~MprisMediaSessionController() = default;

bool MprisMediaSessionController::any_player_playing()
{
    std::lock_guard lock{mutex_};
    if (!impl_->connection) {
        return false;
    }

    for (const auto& service : filter_mpris_services(list_names(impl_->connection.get()))) {
        const auto status = get_playback_status(impl_->connection.get(), service);
        if (status && *status == "Playing") {
            return true;
        }
    }

    return false;
}

std::vector<std::string> MprisMediaSessionController::pause_playing_players()
{
    std::lock_guard lock{mutex_};
    std::vector<std::string> paused;

    if (!impl_->connection) {
        OPENPODS_DEBUG("MPRIS: no session-bus connection");
        return paused;
    }

    auto* connection = impl_->connection.get();
    const auto services = filter_mpris_services(list_names(connection));
    OPENPODS_DEBUG("MPRIS: " << services.size() << " media-player(s) on the bus");
    for (const auto& service : services) {
        const auto status = get_playback_status(connection, service);
        OPENPODS_DEBUG("MPRIS: " << service << " -> "
            << (status ? *status : std::string{"<no PlaybackStatus>"}));
        if (!status || *status != "Playing") {
            continue;
        }

        if (call_player_method(connection, service, "Pause")) {
            paused.push_back(service);
        }
    }

    return paused;
}

void MprisMediaSessionController::resume_players(
    const std::vector<std::string>& services
)
{
    std::lock_guard lock{mutex_};
    if (!impl_->connection) {
        return;
    }

    auto* connection = impl_->connection.get();
    for (const auto& service : services) {
        if (is_kdeconnect_service(service)) {
            continue;
        }
        call_player_method(connection, service, "Play");
    }
}

void MprisMediaSessionController::play_pause_first()
{
    std::lock_guard lock{mutex_};
    if (!impl_->connection) {
        return;
    }

    auto* connection = impl_->connection.get();
    for (const auto& service : filter_mpris_services(list_names(connection))) {
        if (call_player_method(connection, service, "PlayPause")) {
            return;
        }
    }
}

void MprisMediaSessionController::next()
{
    std::lock_guard lock{mutex_};
    if (!impl_->connection) {
        return;
    }

    auto* connection = impl_->connection.get();
    for (const auto& service : filter_mpris_services(list_names(connection))) {
        if (call_player_method(connection, service, "Next")) {
            return;
        }
    }
}

void MprisMediaSessionController::previous()
{
    std::lock_guard lock{mutex_};
    if (!impl_->connection) {
        return;
    }

    auto* connection = impl_->connection.get();
    for (const auto& service : filter_mpris_services(list_names(connection))) {
        if (call_player_method(connection, service, "Previous")) {
            return;
        }
    }
}
