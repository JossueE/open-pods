#include "server/ipc.hpp"

#include "server/app_event.hpp"
#include "utils/runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

    bool write_all(int fd, const uint8_t* data, std::size_t size) {
        std::size_t written = 0;
        while (written < size) {
            const ssize_t result = ::send(fd, data + written, size - written, MSG_NOSIGNAL);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (result == 0) {
                return false;
            }
            written += static_cast<std::size_t>(result);
        }
        return true;
    }

    bool read_all(int fd, uint8_t* data, std::size_t size) {
        std::size_t read_bytes = 0;
        while (read_bytes < size) {
            const ssize_t result = ::read(fd, data + read_bytes, size - read_bytes);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (result == 0) {
                return false;
            }
            read_bytes += static_cast<std::size_t>(result);
        }
        return true;
    }

    bool same_device(const AppEvent& event, const std::string& mac) {
        if (const auto* connected = std::get_if<DeviceConnectedEvent>(&event.payload)) {
            return connected->mac == mac;
        }
        if (const auto* disconnected = std::get_if<DeviceDisconnectedEvent>(&event.payload)) {
            return disconnected->mac == mac;
        }
        if (const auto* aacp = std::get_if<AacpAppEvent>(&event.payload)) {
            return aacp->mac == mac;
        }
        return false;
    }

    template <typename T>
    bool is_aacp_payload(const AppEvent& event, const std::string& mac) {
        const auto* aacp = std::get_if<AacpAppEvent>(&event.payload);
        return aacp != nullptr && aacp->mac == mac && std::holds_alternative<T>(aacp->event);
    }

    std::optional<BatteryInfo> previous_case_battery(
        const std::vector<AppEvent>& events,
        const std::string& mac
    ) {
        for (const AppEvent& event : events) {
            const auto* aacp = std::get_if<AacpAppEvent>(&event.payload);
            if (aacp == nullptr || aacp->mac != mac) {
                continue;
            }

            const auto* batteries = std::get_if<std::vector<BatteryInfo>>(&aacp->event);
            if (batteries == nullptr) {
                continue;
            }

            const auto it = std::find_if(
                batteries->begin(),
                batteries->end(),
                [](const BatteryInfo& battery) {
                    return battery.component == BatteryComponent::Case
                        && battery.status != BatteryStatus::Disconnected;
                }
            );
            if (it != batteries->end()) {
                return *it;
            }
        }

        return std::nullopt;
    }

} // namespace

namespace ipc {

struct IpcServer::Client {
    explicit Client(int socket)
        : fd(socket) {}

    ~Client() {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    int fd = -1;
    std::mutex write_mutex;
};

std::filesystem::path socket_path() {
    const auto dir = utils::runtime_dir();
    if (!dir) {
        return {};
    }

    return *dir / "airpods-tui.sock";
}

    bool write_msg(int fd, const std::vector<uint8_t>& data) {
        if (data.size() > MAX_MESSAGE_BYTES) {
            return false;
        }

        const uint32_t size = static_cast<uint32_t>(data.size());
        const uint8_t length[] = {
            static_cast<uint8_t>((size >> 24) & 0xFF),
            static_cast<uint8_t>((size >> 16) & 0xFF),
            static_cast<uint8_t>((size >> 8) & 0xFF),
            static_cast<uint8_t>(size & 0xFF)
        };

        return write_all(fd, length, sizeof(length))
            && (data.empty() || write_all(fd, data.data(), data.size()));
    }

    std::optional<std::vector<uint8_t>> read_msg(int fd) {
        uint8_t length[4] = {};
        if (!read_all(fd, length, sizeof(length))) {
            return std::nullopt;
        }

        const std::size_t size =
            (static_cast<std::size_t>(length[0]) << 24)
            | (static_cast<std::size_t>(length[1]) << 16)
            | (static_cast<std::size_t>(length[2]) << 8)
            | static_cast<std::size_t>(length[3]);

        if (size > MAX_MESSAGE_BYTES) {
            return std::nullopt;
        }

        std::vector<uint8_t> data(size);
        if (!data.empty() && !read_all(fd, data.data(), data.size())) {
            return std::nullopt;
        }

        return data;
    }

    void SnapshotStore::update(const AppEvent& event) {
        std::lock_guard snapshot_lock{mutex_};

        if (const auto* connected = std::get_if<DeviceConnectedEvent>(&event.payload)) {
            events_.erase(
                std::remove_if(
                    events_.begin(),
                    events_.end(),
                    [&](const AppEvent& current) {
                        return same_device(current, connected->mac);
                    }
                ),
                events_.end()
            );
            events_.push_back(event);
            return;
        }

        if (const auto* disconnected = std::get_if<DeviceDisconnectedEvent>(&event.payload)) {
            events_.erase(
                std::remove_if(
                    events_.begin(),
                    events_.end(),
                    [&](const AppEvent& current) {
                        return same_device(current, disconnected->mac);
                    }
                ),
                events_.end()
            );
            return;
        }

        if (const auto* aacp = std::get_if<AacpAppEvent>(&event.payload)) {
            if (const auto* batteries = std::get_if<std::vector<BatteryInfo>>(&aacp->event)) {
                const bool has_connected_case = std::any_of(
                    batteries->begin(),
                    batteries->end(),
                    [](const BatteryInfo& battery) {
                        return battery.component == BatteryComponent::Case
                            && battery.status != BatteryStatus::Disconnected;
                    }
                );

                if (!has_connected_case) {
                    if (const auto case_battery = previous_case_battery(events_, aacp->mac)) {
                        std::vector<BatteryInfo> merged;
                        merged.reserve(batteries->size() + 1);
                        for (const BatteryInfo& battery : *batteries) {
                            if (battery.component != BatteryComponent::Case) {
                                merged.push_back(battery);
                            }
                        }
                        merged.push_back(*case_battery);

                        events_.erase(
                            std::remove_if(
                                events_.begin(),
                                events_.end(),
                                [&](const AppEvent& current) {
                                    return is_aacp_payload<std::vector<BatteryInfo>>(current, aacp->mac);
                                }
                            ),
                            events_.end()
                        );
                        events_.push_back(AppEvent::aacp_event(aacp->mac, std::move(merged)));
                        return;
                    }
                }

                events_.erase(
                    std::remove_if(
                        events_.begin(),
                        events_.end(),
                        [&](const AppEvent& current) {
                            return is_aacp_payload<std::vector<BatteryInfo>>(current, aacp->mac);
                        }
                    ),
                    events_.end()
                );
                events_.push_back(event);
                return;
            }

            if (const auto* command = std::get_if<ControlCommandStatus>(&aacp->event)) {
                events_.erase(
                    std::remove_if(
                        events_.begin(),
                        events_.end(),
                        [&](const AppEvent& current) {
                            const auto* current_aacp = std::get_if<AacpAppEvent>(&current.payload);
                            if (current_aacp == nullptr || current_aacp->mac != aacp->mac) {
                                return false;
                            }

                            const auto* current_command =
                                std::get_if<ControlCommandStatus>(&current_aacp->event);
                            return current_command != nullptr
                                && current_command->identifier == command->identifier;
                        }
                    ),
                    events_.end()
                );
                events_.push_back(event);
                return;
            }

            if (std::holds_alternative<EarDetection>(aacp->event)) {
                events_.erase(
                    std::remove_if(
                        events_.begin(),
                        events_.end(),
                        [&](const AppEvent& current) {
                            return is_aacp_payload<EarDetection>(current, aacp->mac);
                        }
                    ),
                    events_.end()
                );
                events_.push_back(event);
                return;
            }

            if (std::holds_alternative<ConnectedDevices>(aacp->event)) {
                events_.erase(
                    std::remove_if(
                        events_.begin(),
                        events_.end(),
                        [&](const AppEvent& current) {
                            return is_aacp_payload<ConnectedDevices>(current, aacp->mac);
                        }
                    ),
                    events_.end()
                );
                events_.push_back(event);
                return;
            }

            if (std::holds_alternative<EqualizerData>(aacp->event)) {
                events_.erase(
                    std::remove_if(
                        events_.begin(),
                        events_.end(),
                        [&](const AppEvent& current) {
                            return is_aacp_payload<EqualizerData>(current, aacp->mac);
                        }
                    ),
                    events_.end()
                );
                events_.push_back(event);
            }

            return;
        }

        if (std::holds_alternative<AudioUnavailableEvent>(event.payload)) {
            const bool already_present = std::any_of(
                events_.begin(),
                events_.end(),
                [](const AppEvent& current) {
                    return std::holds_alternative<AudioUnavailableEvent>(current.payload);
                }
            );
            if (!already_present) {
                events_.push_back(event);
            }
        }
    }

    std::vector<AppEvent> SnapshotStore::snapshot() const {
        std::lock_guard snapshot_lock{mutex_};
        return events_;
    }

    IpcServer::IpcServer(
        StateSnapshotPtr snapshot,
        CommandHandler command_handler,
        EventSerializer event_serializer,
        CommandDeserializer command_deserializer
    )
        : snapshot_(std::move(snapshot)),
        command_handler_(std::move(command_handler)),
        event_serializer_(std::move(event_serializer)),
        command_deserializer_(std::move(command_deserializer)) {}

    void IpcServer::broadcast(const AppEvent& event) {
        if (snapshot_) {
            snapshot_->update(event);
        }

        if (!event_serializer_) {
            return;
        }

        const auto data = event_serializer_(event);
        if (!data) {
            return;
        }

        std::lock_guard clients_lock{clients_mutex_};
        clients_.erase(
            std::remove_if(
                clients_.begin(),
                clients_.end(),
                [&](const std::shared_ptr<Client>& client) {
                    std::lock_guard write_lock{client->write_mutex};
                    return !write_msg(client->fd, *data);
                }
            ),
            clients_.end()
        );
    }

    bool IpcServer::run() {
    const std::filesystem::path path = socket_path();
    if (path.empty()) {
        return false;
    }

    std::error_code error;
        std::filesystem::remove(path, error);

        const int server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd < 0) {
            return false;
        }

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        const std::string path_string = path.string();
        if (path_string.size() >= sizeof(address.sun_path)) {
            ::close(server_fd);
            return false;
        }
        std::strncpy(address.sun_path, path_string.c_str(), sizeof(address.sun_path) - 1);

        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            ::close(server_fd);
            return false;
        }

        ::chmod(path_string.c_str(), 0600);

        if (::listen(server_fd, 16) < 0) {
            ::close(server_fd);
            return false;
        }

        for (;;) {
            const int client_fd = ::accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ::close(server_fd);
                return false;
            }

            auto client = std::make_shared<Client>(client_fd);

            bool client_alive = true;
            if (snapshot_ && event_serializer_) {
                for (const AppEvent& event : snapshot_->snapshot()) {
                    const auto data = event_serializer_(event);
                    if (!data || !write_msg(client->fd, *data)) {
                        client_alive = false;
                        break;
                    }
                }
            }

            if (!client_alive) {
                continue;
            }

            {
                std::lock_guard clients_lock{clients_mutex_};
                clients_.push_back(client);
            }

            std::thread([this, client]() {
                while (true) {
                    const auto data = read_msg(client->fd);
                    if (!data) {
                        break;
                    }

                    if (!command_deserializer_) {
                        continue;
                    }

                    const auto command = command_deserializer_(*data);
                    if (command && command_handler_) {
                        command_handler_(*command);
                    }
                }

                {
                    std::lock_guard clients_lock{clients_mutex_};
                    clients_.erase(
                        std::remove(clients_.begin(), clients_.end(), client),
                        clients_.end()
                    );
                }

            }).detach();
        }
    }

    IpcClient::IpcClient(
        CommandSerializer command_serializer,
        EventDeserializer event_deserializer
    )
        : command_serializer_(std::move(command_serializer)),
        event_deserializer_(std::move(event_deserializer)) {}

    IpcClient::~IpcClient() {
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
        }
    }

    bool IpcClient::connect() {
        if (socket_fd_ >= 0) {
            return true;
        }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const std::filesystem::path path = socket_path();
    if (path.empty()) {
        ::close(fd);
        return false;
    }

    const std::string path_string = path.string();
        if (path_string.size() >= sizeof(address.sun_path)) {
            ::close(fd);
            return false;
        }
        std::strncpy(address.sun_path, path_string.c_str(), sizeof(address.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            ::close(fd);
            return false;
        }

        socket_fd_ = fd;
        return true;
    }

    bool IpcClient::send_command(const DeviceCommandEnvelope& command) {
        if (socket_fd_ < 0 || !command_serializer_) {
            return false;
        }

        const auto data = command_serializer_(command);
        return data.has_value() && write_msg(socket_fd_, *data);
    }

    std::optional<AppEvent> IpcClient::read_event() {
        if (socket_fd_ < 0 || !event_deserializer_) {
            return std::nullopt;
        }

        const auto data = read_msg(socket_fd_);
        if (!data) {
            return std::nullopt;
        }

        return event_deserializer_(*data);
    }

    std::optional<AppEvent> IpcClient::read_event_for(std::chrono::milliseconds timeout) {
        if (socket_fd_ < 0 || !event_deserializer_) {
            return std::nullopt;
        }

        pollfd descriptor {
            .fd = socket_fd_,
            .events = POLLIN,
            .revents = 0,
        };

        const int result = ::poll(
            &descriptor,
            1,
            static_cast<int>(timeout.count())
        );
        if (result <= 0 || (descriptor.revents & POLLIN) == 0) {
            return std::nullopt;
        }

        return read_event();
    }

} // namespace ipc
