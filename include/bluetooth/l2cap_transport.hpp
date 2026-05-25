#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

/**
 * @brief Abstract transport for BR/EDR L2CAP packet I/O.
 */
class L2capTransport {
public:
    virtual ~L2capTransport() = default;

    virtual bool connect(std::string_view mac_address, uint16_t psm) = 0;
    virtual bool send(const std::vector<uint8_t>& packet) = 0;
    virtual std::optional<std::vector<uint8_t>> receive(std::size_t max_bytes) = 0;
    virtual void close() = 0;
};
