#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bluetooth/aacp.hpp"
#include "bluetooth/l2cap_transport.hpp"

using ControlCommandSubscriber = std::function<void(const std::vector<uint8_t>&)>;
using AACPEventHandler = std::function<void(const AACPEvent&)>;

class AACPManager {
    public:

        explicit AACPManager(std::shared_ptr<L2capTransport> transport);
        ~AACPManager();

        bool connect(std::string_view mac_address);

        bool send_packet(const std::vector<uint8_t>& packet);

        bool send_data_packet(const std::vector<uint8_t>& data);

        bool set_event_channel(AACPEventHandler handler);

        bool subscribe_to_control_command(ControlCommandIdentifiers identifier, ControlCommandSubscriber subscriber);

        bool receive_packet(const std::vector<uint8_t>& packet);

        bool wait_for_opcode(uint8_t expected_opcode, std::chrono::milliseconds timeout);

        bool wait_for_any_opcode(std::chrono::milliseconds timeout);

        bool send_notification_request();

        bool send_set_feature_flags_packet();

        bool send_init_ext();

        bool send_handshake();

        bool send_proximity_keys_request(const std::vector<ProximityKeyType>& key_types);

        bool send_rename_packet(std::string_view new_name);

        bool send_control_command(ControlCommandIdentifiers identifier, const std::vector<uint8_t>& data);

        bool send_ssl_request();

        std::pair<std::optional<EarDetectionStatus>, std::optional<EarDetectionStatus>> ear_detection_state();

        std::optional<AudioSource> last_audio_source();

    private:
        struct State {
            // The opcode mutex/condvar pair drives wait_for_opcode/wait_for_any_opcode
            // for strict init sequencing. The state mutex protects all other shared
            // fields read/written across threads (receive loop, init thread, public
            // accessors, MediaController playback listener).
            std::mutex opcode_mutex;
            std::condition_variable opcode_cv;
            std::vector<uint8_t> opcode_history;

            std::mutex state_mutex;

            std::vector<ControlCommandStatus> control_command_status_list;

            std::unordered_map<
                ControlCommandIdentifiers,
                std::vector<ControlCommandSubscriber>
            > control_command_subscribers;

            bool owns = false;

            std::vector<ConnectedDevice> old_connected_devices;
            std::vector<ConnectedDevice> connected_devices;

            std::optional<AudioSource> audio_source;
            std::vector<BatteryInfo> battery_info;

            uint8_t conversational_awareness_status = 0;

            std::optional<EarDetectionStatus> ear_detection_left;
            std::optional<EarDetectionStatus> ear_detection_right;
            std::optional<BatteryComponent> primary_pod;

            std::optional<std::string> airpods_mac;

            std::optional<AirPodsInformation> information;

            AACPEventHandler event_handler;
        };

        State state_;
        std::shared_ptr<L2capTransport> transport_;
        std::atomic_bool receive_running_ {false};
        std::thread receive_thread_;
        // Serializes L2CAP writes so concurrent IPC commands and init-time
        // control packets cannot interleave bytes on the wire. SOCK_SEQPACKET
        // preserves message boundaries on the kernel side; this mutex preserves
        // them on our send path.
        std::mutex send_mutex_;

        void record_opcode(uint8_t opcode);
        void start_receive_loop();
        void stop_receive_loop();
};
