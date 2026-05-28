#include "server/headless_state.hpp"

#include "devices/apple_models.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <vector>

void HeadlessState::handle_event(const AppEvent& event) {
    if (const auto* connected = std::get_if<DeviceConnectedEvent>(&event.payload)) {
        auto& state = devices_[connected->mac];
        state.mac = connected->mac;
        state.name = connected->name;
        state.product_id = connected->product_id;
        selected_mac_ = connected->mac;
        return;
    }

    if (const auto* disconnected = std::get_if<DeviceDisconnectedEvent>(&event.payload)) {
        devices_.erase(disconnected->mac);
        if (selected_mac_ == disconnected->mac) {
            selected_mac_.reset();
            if (!devices_.empty()) {
                selected_mac_ = devices_.begin()->first;
            }
        }
        return;
    }

    const auto* aacp = std::get_if<AacpAppEvent>(&event.payload);
    if (aacp == nullptr) {
        return;
    }

    auto& state = devices_[aacp->mac];
    state.mac = aacp->mac;
    selected_mac_ = aacp->mac;

    if (const auto* control = std::get_if<ControlCommandStatus>(&aacp->event)) {
        if (control->identifier == ControlCommandIdentifiers::ListeningMode
            && !control->value.empty()) {
            state.noise_mode = control->value.front();
        }
        return;
    }

    const auto* batteries = std::get_if<std::vector<BatteryInfo>>(&aacp->event);
    if (batteries == nullptr) {
        return;
    }

    for (const BatteryInfo& battery : *batteries) {
        BatteryValue value {
            .level = battery.level,
            .status = battery.status,
        };

        switch (battery.component) {
            case BatteryComponent::LeftBud:
                state.left = value;
                break;
            case BatteryComponent::RightBud:
                state.right = value;
                break;
            case BatteryComponent::Case:
                if (battery.status != BatteryStatus::Disconnected) {
                    state.case_battery = value;
                }
                break;
            case BatteryComponent::Headphone:
                state.headphone = value;
                break;
        }
    }
}

std::string HeadlessState::waybar_json() const {
    if (!selected_mac_ || !devices_.contains(*selected_mac_)) {
        return nlohmann::json{
            {"text", ""},
            {"tooltip", "No AirPods"},
            {"class", "disconnected"},
            {"percentage", 0}
        }.dump();
    }

    const auto& device = devices_.at(*selected_mac_);
    const std::string name = display_name(device);
    std::string tooltip = name;
    int percentage = 0;
    std::string text = name;

    auto batteries = nlohmann::json::array();
    auto push_battery = [&](const char* component, const BatteryValue& value) {
        batteries.push_back({
            {"component", component},
            {"level", value.level},
            {"charging", value.status == BatteryStatus::Charging},
        });
    };

    std::vector<uint8_t> levels;
    if (device.left) {
        levels.push_back(device.left->level);
        tooltip += "\nL: " + std::to_string(device.left->level) + "%";
        push_battery("left", *device.left);
    }
    if (device.right) {
        levels.push_back(device.right->level);
        tooltip += "\nR: " + std::to_string(device.right->level) + "%";
        push_battery("right", *device.right);
    }
    if (device.case_battery) {
        tooltip += "\nC: " + std::to_string(device.case_battery->level) + "%";
        push_battery("case", *device.case_battery);
    }
    if (device.headphone) {
        levels.push_back(device.headphone->level);
        tooltip += "\n" + std::to_string(device.headphone->level) + "%";
        push_battery("headphone", *device.headphone);
    }

    if (!levels.empty()) {
        percentage = *std::min_element(levels.begin(), levels.end());
        text = std::to_string(percentage) + "%";
    }

    const char* noise = "";
    if (device.noise_mode) {
        switch (*device.noise_mode) {
            case 0x01: noise = "off"; break;
            case 0x02: noise = "anc"; break;
            case 0x03: noise = "transparency"; break;
            case 0x04: noise = "adaptive"; break;
            default: noise = ""; break;
        }
    }

    AppleModels models;
    const auto info = models.model_info(device.product_id);

    return nlohmann::json{
        {"text", text},
        {"tooltip", tooltip},
        {"class", "connected"},
        {"percentage", percentage},
        {"name", name},
        {"noise", noise},
        {"caps", {
            {"anc", info.has_noise_cancellation},
            {"adaptive", info.has_adaptive_transparency},
        }},
        {"batteries", batteries}
    }.dump();
}

bool HeadlessState::has_airpods() const {
    return selected_mac_.has_value() && devices_.contains(*selected_mac_);
}

bool HeadlessState::has_battery() const {
    if (!selected_mac_ || !devices_.contains(*selected_mac_)) {
        return false;
    }

    const auto& device = devices_.at(*selected_mac_);
    return device.left.has_value()
        || device.right.has_value()
        || device.headphone.has_value();
}

std::string HeadlessState::display_name(const DeviceState& device) {
    if (device.product_id != 0) {
        AppleModels models;
        return std::string{models.model_info(device.product_id).model_name};
    }

    if (!device.name.empty()) {
        return device.name;
    }

    return "AirPods";
}
