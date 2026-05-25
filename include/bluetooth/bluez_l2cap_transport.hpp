#pragma once

#include "bluetooth/l2cap_transport.hpp"

/**
 * @brief BlueZ/Linux implementation of the L2CAP transport.
 */
class BluezL2capTransport final : public L2capTransport {
public:
    ~BluezL2capTransport() override;

    bool connect(std::string_view mac_address, uint16_t psm) override;
    bool send(const std::vector<uint8_t>& packet) override;
    std::optional<std::vector<uint8_t>> receive(std::size_t max_bytes) override;
    void close() override;

private:
    int socket_fd_ = -1;
};
