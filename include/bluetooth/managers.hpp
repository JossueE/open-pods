#pragma once

#include <memory>
#include <utility>

class AACPManager;

/**
 * @brief Holds runtime managers associated with one Bluetooth device.
 */
class DeviceManagers {
public:
    static DeviceManagers placeholder()
    {
        return {};
    }

    static DeviceManagers with_aacp(std::shared_ptr<AACPManager> aacp)
    {
        DeviceManagers managers;
        managers.set_aacp(std::move(aacp));
        return managers;
    }

    void set_aacp(std::shared_ptr<AACPManager> manager)
    {
        aacp_ = std::move(manager);
    }

    std::shared_ptr<AACPManager> get_aacp() const
    {
        return aacp_;
    }

private:
    std::shared_ptr<AACPManager> aacp_;
};