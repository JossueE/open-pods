#include "main.hpp"

#include "bluetooth/aacp_manager.hpp"
#include "bluetooth/bluez_discovery.hpp"
#include "bluetooth/bluez_l2cap_transport.hpp"
#include "bluetooth/discovery.hpp"
#include "config/app_config.hpp"
#include "devices/apple_models.hpp"
#include "server/app_event.hpp"
#include "server/daemon.hpp"
#include "server/headless_state.hpp"
#include "server/ipc.hpp"
#include "tui/tui_app.hpp"
#include "tui/tui_render.hpp"
#include "tui/tui_state.hpp"
#include "tui/tui_term.hpp"
#include "utils/logging.hpp"
#include "utils/runtime.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view VERSION = "dev";

bool has_apple_device_id(std::istream& input) {
    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }

        if (line[first] == '#') {
            continue;
        }

        if (line.find("bluetooth:004C:") != std::string::npos) {
            return true;
        }
    }

    return false;
}

uint16_t product_id_from_modalias(const std::string& modalias) {
    AppleModels models;
    const auto ids = models.parse_modalias(modalias);
    if (!ids || ids->first != APPLE_COMPANY_ID) {
        return 0;
    }
    return ids->second;
}

std::string model_label(const BluetoothDeviceInfo& device) {
    const auto product_id = product_id_from_modalias(device.modalias);
    if (product_id != 0) {
        AppleModels models;
        return std::string{models.model_info(product_id).model_name};
    }

    if (!device.name.empty()) {
        return device.name;
    }

    return "AirPods";
}

std::optional<std::vector<BatteryInfo>> read_battery_once(
    const BluetoothDeviceInfo& device
) {
    auto transport = std::make_shared<BluezL2capTransport>();
    AACPManager manager{transport};

    std::mutex mutex;
    std::condition_variable cv;
    std::optional<std::vector<BatteryInfo>> batteries;

    manager.set_event_channel([&](const AACPEvent& event) {
        if (const auto* info = std::get_if<std::vector<BatteryInfo>>(&event)) {
            {
                std::lock_guard lock{mutex};
                batteries = *info;
            }
            cv.notify_all();
        }
    });

    if (!manager.connect(device.address)) {
        return std::nullopt;
    }

    bool ok = true;
    ok = manager.send_handshake() && ok;
    (void)manager.wait_for_any_opcode(std::chrono::milliseconds{500});

    ok = manager.send_set_feature_flags_packet() && ok;
    (void)manager.wait_for_opcode(opcodes::SET_FEATURE_FLAGS, std::chrono::milliseconds{500});

    ok = manager.send_notification_request() && ok;
    (void)manager.wait_for_opcode(opcodes::REQUEST_NOTIFICATIONS, std::chrono::milliseconds{500});

    ok = manager.send_ssl_request() && ok;

    AppleModels models;
    const auto product_id = product_id_from_modalias(device.modalias);
    if (models.needs_aap_init_ext(product_id)) {
        (void)manager.wait_for_opcode(opcodes::SET_FEATURE_FLAGS, std::chrono::milliseconds{500});
        ok = manager.send_init_ext() && ok;
    }

    ok = manager.send_proximity_keys_request({
        ProximityKeyType::Irk,
        ProximityKeyType::EncKey,
    }) && ok;

    if (!ok) {
        return std::nullopt;
    }

    std::unique_lock lock{mutex};
    cv.wait_for(lock, std::chrono::seconds{3}, [&] {
        return batteries.has_value();
    });

    return batteries;
}

std::optional<int> run_waybar_from_ipc(bool watch) {
    ipc::IpcClient client{
        serialize_device_command_envelope,
        deserialize_app_event
    };

    if (!client.connect()) {
        return std::nullopt;
    }

    HeadlessState state;
    std::string last_json;

    auto print_if_changed = [&]() {
        const std::string json = state.waybar_json();
        if (json != last_json) {
            std::cout << json << '\n';
            last_json = json;
        }
    };

    if (watch) {
        print_if_changed();
        while (true) {
            auto event = client.read_event();
            if (!event) {
                return 1;
            }
            state.handle_event(*event);
            print_if_changed();
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()
        );

        auto event = client.read_event_for(std::min(remaining, std::chrono::milliseconds{500}));
        if (!event) {
            continue;
        }

        state.handle_event(*event);
        if (state.has_battery()) {
            break;
        }
    }

    std::cout << state.waybar_json() << '\n';
    return 0;
}

std::string direct_waybar_json() {
    BluezDiscoveryBackend backend;
    const auto device = find_connected_airpods(backend);

    if (!device) {
        return nlohmann::json{
            {"text", ""},
            {"tooltip", "No AirPods"},
            {"class", "disconnected"},
            {"percentage", 0}
        }.dump();
    }

    const std::string label = model_label(*device);
    std::string tooltip = label + "\n" + device->address;
    int percentage = 0;
    std::string text = label;

    const auto batteries = read_battery_once(*device);
    if (batteries) {
        std::optional<uint8_t> left;
        std::optional<uint8_t> right;
        std::optional<uint8_t> case_battery;
        std::optional<uint8_t> headphone;

        for (const BatteryInfo& battery : *batteries) {
            switch (battery.component) {
                case BatteryComponent::LeftBud:
                    left = battery.level;
                    break;
                case BatteryComponent::RightBud:
                    right = battery.level;
                    break;
                case BatteryComponent::Case:
                    if (battery.status != BatteryStatus::Disconnected) {
                        case_battery = battery.level;
                    }
                    break;
                case BatteryComponent::Headphone:
                    headphone = battery.level;
                    break;
            }
        }

        std::vector<uint8_t> levels;
        if (left) levels.push_back(*left);
        if (right) levels.push_back(*right);
        if (headphone) levels.push_back(*headphone);

        if (!levels.empty()) {
            percentage = *std::min_element(levels.begin(), levels.end());
            text = std::to_string(percentage) + "%";
        }

        if (left) tooltip += "\nL: " + std::to_string(*left) + "%";
        if (right) tooltip += "\nR: " + std::to_string(*right) + "%";
        if (case_battery) tooltip += "\nC: " + std::to_string(*case_battery) + "%";
        if (headphone) tooltip += "\n" + std::to_string(*headphone) + "%";

        utils::write_battery_env(left, right, case_battery, headphone);
    } else {
        tooltip += "\nBattery unavailable";
    }

    return nlohmann::json{
        {"text", text},
        {"tooltip", tooltip},
        {"class", "connected"},
        {"percentage", percentage}
    }.dump();
}

int run_reclaim() {
    ipc::IpcClient client{
        serialize_device_command_envelope,
        deserialize_app_event
    };

    if (!client.connect()) {
        std::cerr << "Could not reach the open-pods daemon. Is it running?\n";
        return 1;
    }

    BluezDiscoveryBackend backend;
    const auto target = find_connected_airpods(backend);
    if (!target) {
        std::cerr << "No connected AirPods found.\n";
        return 1;
    }

    if (!client.send_command(std::make_pair(target->address, DeviceCommand::reclaim_audio()))) {
        std::cerr << "Failed to send reclaim command to the daemon.\n";
        return 1;
    }
    return 0;
}

} // namespace

MainArgs parse_args(const std::vector<std::string_view>& args) {
    MainArgs parsed;

    for (std::string_view arg : args) {
        if (arg == "--debug" || arg == "-d") {
            parsed.debug = true;
        } else if (arg == "--version" || arg == "-v") {
            parsed.version = true;
        } else if (arg == "--waybar") {
            parsed.waybar = true;
        } else if (arg == "--waybar-watch") {
            parsed.waybar_watch = true;
        } else if (arg == "--daemon") {
            parsed.daemon = true;
        } else if (arg == "--reclaim") {
            parsed.reclaim = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage("open-pods");
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            print_usage("open-pods");
            std::exit(2);
        }
    }

    return parsed;
}

void print_usage(std::string_view program_name) {
    std::cout
        << "Usage: " << program_name << " [options]\n\n"
        << "Options:\n"
        << "  -d, --debug          Enable debug logging\n"
        << "  -v, --version        Show version and exit\n"
        << "      --waybar         Print JSON status for waybar and exit\n"
        << "      --waybar-watch   Print JSON status for waybar on each change\n"
        << "      --daemon         Run headless daemon\n"
        << "      --reclaim        Force the AirPods back to this host (when an iPhone took the audio source)\n"
        << "  -h, --help           Show this help\n";
}

void check_bluetooth_config() {
    constexpr std::string_view path = "/etc/bluetooth/main.conf";

    std::ifstream file{std::string{path}};
    if (file && has_apple_device_id(file)) {
        return;
    }

    std::cerr
        << "WARNING: Apple DeviceID missing in " << path << ".\n"
        << "Add under [General]:\n"
        << "  DeviceID = bluetooth:004C:0000:0000\n"
        << "Then restart bluetooth and re-pair AirPods.\n";
}

int run_waybar_mode(bool watch) {
    if (const auto ipc_result = run_waybar_from_ipc(watch)) {
        return *ipc_result;
    }

    std::string last_json;
    do {
        const std::string json = direct_waybar_json();
        if (json != last_json) {
            std::cout << json << '\n';
            last_json = json;
        }

        if (!watch) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::seconds{30});
    } while (true);

    return 0;
}

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    // Hidden visual smoke-test flag. Renders a synthetic frame and exits, so
    // the layout can be validated without an interactive TTY.
    for (auto arg : args) {
        if (arg == "--render-demo") {
            uint16_t product_id = 0x2014; // AirPods Pro 2 by default
            std::string model_name = "AirPods Pro 2";
            for (auto a : args) {
                if (a == "--airpods1") { product_id = 0x2002; model_name = "AirPods (1st gen)"; }
                if (a == "--airpods3") { product_id = 0x2013; model_name = "AirPods (3rd gen)"; }
                if (a == "--max")      { product_id = 0x200a; model_name = "AirPods Max"; }
            }
            tui::TuiState state;
            state.handle_event(AppEvent::device_connected(
                "AA:BB:CC:DD:EE:FF",
                model_name,
                product_id
            ));
            std::vector<BatteryInfo> batt {
                {BatteryComponent::LeftBud, BatteryStatus::NotCharging, 82},
                {BatteryComponent::RightBud, BatteryStatus::NotCharging, 74},
                {BatteryComponent::Case, BatteryStatus::Charging, 48},
            };
            state.handle_event(AppEvent::aacp_event("AA:BB:CC:DD:EE:FF", AACPEvent{batt}));
            auto push_cmd = [&](ControlCommandIdentifiers id, uint8_t v) {
                state.handle_event(AppEvent::aacp_event("AA:BB:CC:DD:EE:FF",
                    AACPEvent{ControlCommandStatus{id, {v}}}));
            };
            push_cmd(ControlCommandIdentifiers::ListeningMode, 0x03); // Transparency
            push_cmd(ControlCommandIdentifiers::ConversationDetectConfig, 0x00);
            push_cmd(ControlCommandIdentifiers::OneBudAncMode, 0x00);
            push_cmd(ControlCommandIdentifiers::AdaptiveVolumeConfig, 0x01);
            push_cmd(ControlCommandIdentifiers::VolumeSwipeMode, 0x01);
            push_cmd(ControlCommandIdentifiers::DoubleClickInterval, 0x00);
            push_cmd(ControlCommandIdentifiers::ClickHoldInterval, 0x00);
            push_cmd(ControlCommandIdentifiers::ChimeVolume, 0x02);
            push_cmd(ControlCommandIdentifiers::VolumeSwipeInterval, 0x00);
            push_cmd(ControlCommandIdentifiers::MicMode, 0x00);
            push_cmd(ControlCommandIdentifiers::AllowAutoConnect, 0x01);
            state.handle_event(AppEvent::aacp_event("AA:BB:CC:DD:EE:FF",
                AACPEvent{EarDetection{
                    EarDetectionStatus::OutOfEar, EarDetectionStatus::OutOfEar,
                    EarDetectionStatus::InEar, EarDetectionStatus::InEar
                }}));
            state.set_media_status(
                "org.mpris.MediaPlayer2.spotify",
                std::string{"Playing"},
                std::string{"After Dark"},
                std::string{"Mr. Kitty"}
            );
            state.set_volume(42);
            state.set_codec(std::string{"AAC"});
            state.set_local_address("11:22:33:44:55:66");
            state.log("ipc: connected to daemon");
            uint16_t cols = 120;
            uint16_t rows = 50;
            for (auto a : args) {
                if (a == "--narrow") { cols = 80; rows = 60; }
                if (a == "--tiny") { cols = 40; rows = 12; }
            }
            const tui::TerminalSize sz {cols, rows};
            std::cout << tui::render_frame(state, sz) << '\n';
            return 0;
        }
    }

    const MainArgs parsed = parse_args(args);

    if (parsed.version) {
        std::cout << "open-pods " << VERSION << '\n';
        return 0;
    }

    utils::set_debug_logging(parsed.debug);

    check_bluetooth_config();

    AppConfig config = AppConfig::load();

    if (parsed.reclaim) {
        return run_reclaim();
    }

    if (parsed.waybar || parsed.waybar_watch) {
        return run_waybar_mode(parsed.waybar_watch);
    }

    if (parsed.daemon) {
        Daemon daemon{std::move(config)};
        return daemon.run() ? 0 : 1;
    }

    return tui::run();
}
