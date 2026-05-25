#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct AppEvent;
struct DeviceCommand;

namespace ipc {

    inline constexpr std::size_t MAX_MESSAGE_BYTES {16 * 1024 * 1024};

    /**
     * @brief Path to the Unix socket used by the daemon IPC server.
     */
    std::filesystem::path socket_path();

    /**
     * @brief Writes one length-prefixed IPC message to a file descriptor.
     * @note Message format: 4-byte big-endian length followed by payload bytes.
     */
    bool write_msg(int fd, const std::vector<uint8_t>& data);

    /**
     * @brief Reads one length-prefixed IPC message from a file descriptor.
     * @return std::nullopt on EOF, invalid framing, or read error.
     */
    std::optional<std::vector<uint8_t>> read_msg(int fd);

    /**
     * @brief State snapshot maintained by the daemon for replaying to new clients.
     */
    class SnapshotStore {
        public:
            /**
             * @brief Updates the replay snapshot with one application event.
             * @note Keeps latest durable state and skips transient events.
             */
            void update(const AppEvent& event);

            std::vector<AppEvent> snapshot() const;

        private:
            mutable std::mutex mutex_;
            std::vector<AppEvent> events_;
    };

    using StateSnapshotPtr = std::shared_ptr<SnapshotStore>;
    using DeviceCommandEnvelope = std::pair<std::string, DeviceCommand>;
    using CommandHandler = std::function<void(const DeviceCommandEnvelope&)>;
    using EventSerializer = std::function<std::optional<std::vector<uint8_t>>(const AppEvent&)>;
    using EventDeserializer = std::function<std::optional<AppEvent>(const std::vector<uint8_t>&)>;
    using CommandSerializer = std::function<std::optional<std::vector<uint8_t>>(const DeviceCommandEnvelope&)>;
    using CommandDeserializer = std::function<std::optional<DeviceCommandEnvelope>(const std::vector<uint8_t>&)>;

    /**
     * @brief Unix-socket IPC server for daemon clients.
     */
    class IpcServer {
        public:
            IpcServer(
                StateSnapshotPtr snapshot,
                CommandHandler command_handler,
                EventSerializer event_serializer,
                CommandDeserializer command_deserializer
            );

            /**
             * @brief Broadcasts an event to all connected clients.
             */
            void broadcast(const AppEvent& event);

            /**
             * @brief Runs the blocking IPC accept loop.
             */
            bool run();

        private:
            struct Client;

            StateSnapshotPtr snapshot_;
            CommandHandler command_handler_;
            EventSerializer event_serializer_;
            CommandDeserializer command_deserializer_;
            std::mutex clients_mutex_;
            std::vector<std::shared_ptr<Client>> clients_;
    };

    /**
     * @brief Client connection to a running daemon.
     */
    class IpcClient {
        public:
            IpcClient(
                CommandSerializer command_serializer,
                EventDeserializer event_deserializer
            );
            ~IpcClient();

            bool connect();
            bool send_command(const DeviceCommandEnvelope& command);
            std::optional<AppEvent> read_event();
            std::optional<AppEvent> read_event_for(std::chrono::milliseconds timeout);

        private:
            int socket_fd_ = -1;
            CommandSerializer command_serializer_;
            EventDeserializer event_deserializer_;
    };

} // namespace ipc
