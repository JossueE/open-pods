#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "server/app_event.hpp"

class HeadlessState {
public:
    void handle_event(const AppEvent& event);
    std::string waybar_json() const;
    bool has_airpods() const;
    bool has_battery() const;

private:
    struct BatteryValue {
        uint8_t level = 0;
        BatteryStatus status = BatteryStatus::Disconnected;
    };

    struct DeviceState {
        std::string mac;
        std::string name;
        uint16_t product_id = 0;
        std::optional<BatteryValue> left;
        std::optional<BatteryValue> right;
        std::optional<BatteryValue> case_battery;
        std::optional<BatteryValue> headphone;
        std::optional<uint8_t> noise_mode;
    };

    static std::string display_name(const DeviceState& device);

    std::optional<std::string> selected_mac_;
    std::unordered_map<std::string, DeviceState> devices_;
};
