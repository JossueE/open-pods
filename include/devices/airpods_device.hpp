#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "config/app_config.hpp"
#include "server/app_event.hpp"

class AACPManager;
class MediaController;

/**
 * @brief Represents one active AirPods runtime connection.
 * @note The constructor only stores dependencies; Bluetooth initialization will live in start().
 */
class AirPodsDevice {
public:
    using AppEventSink = std::function<void(const AppEvent&)>;

    AirPodsDevice(
        std::string mac_address,
        uint16_t product_id,
        AppConfig config,
        std::shared_ptr<AACPManager> aacp_manager,
        std::shared_ptr<MediaController> media_controller,
        AppEventSink app_event_sink = {}
    )
        : mac_address_(std::move(mac_address))
        , product_id_(product_id)
        , config_(std::move(config))
        , aacp_manager_(std::move(aacp_manager))
        , media_controller_(std::move(media_controller))
        , app_event_sink_(std::move(app_event_sink))
    {
    }

    ~AirPodsDevice();

    AirPodsDevice(const AirPodsDevice&) = delete;
    AirPodsDevice& operator=(const AirPodsDevice&) = delete;

    /**
     * @brief Starts the AirPods protocol initialization.
     * @return False until the AACP connection flow is implemented.
     */
    bool start();

    bool wait_for_opcode(AACPManager &aacp_manager, uint8_t opcode, std::chrono::milliseconds timeout);

    bool wait_for_any_opcode(AACPManager &aacp_manager, const std::vector<uint8_t> &opcodes, std::chrono::milliseconds timeout);

    std::shared_ptr<MediaController> media_controller() const { return media_controller_; }

private:
    std::string mac_address_;
    uint16_t product_id_ {0};
    AppConfig config_;
    std::shared_ptr<AACPManager> aacp_manager_;
    std::shared_ptr<MediaController> media_controller_;
    AppEventSink app_event_sink_;
};
