#pragma once

#include <ostream>
#include <string>
#include <variant>
#include <optional>
#include <cstdint>

#include "devices/airpods.hpp" 

enum class DeviceType {
    AirPods,
};

/**
 * @brief Writes the device type name to an output stream.
 * @return The output stream after writing the device type.
 * @note Unknown DeviceType values are written as "Unknown".
*/
inline std::ostream& operator<<(std::ostream& os, DeviceType mode) {
    switch (mode) {
        case DeviceType::AirPods: return os << "AirPods";
        default: return os << "Unknown";
    }
}

struct DeviceInformation{
    std::variant<AirPodsInformation> data;
};

struct DeviceData{
    std::string name;
    DeviceType type;
    std::optional<DeviceInformation> information;
};

enum class AirPodsNoiseControlMode : uint8_t {
    Off = 0x01,
    NoiseCancellation = 0x02,
    Transparency = 0x03,
    Adaptive = 0x04
};

/**
 * @brief Converts a raw byte into an AirPods noise control mode.
 * @return The matching AirPodsNoiseControlMode value.
 * @note Unknown byte values are mapped to AirPodsNoiseControlMode::Off.
*/
inline AirPodsNoiseControlMode noise_mode_from_byte(uint8_t value) {
    switch (value) {
        case 0x01: return AirPodsNoiseControlMode::Off;
        case 0x02: return AirPodsNoiseControlMode::NoiseCancellation;
        case 0x03: return AirPodsNoiseControlMode::Transparency;
        case 0x04: return AirPodsNoiseControlMode::Adaptive;
        default:   return AirPodsNoiseControlMode::Off;
    }
}

// DEVELOPER NOTE: uint8_t byte = static_cast<uint8_t>(AirPodsNoiseControlMode::Adaptive);

/**
 * @brief Writes the AirPods noise control mode name to an output stream.
 * @return The output stream after writing the noise control mode.
 * @note Unknown AirPodsNoiseControlMode values are written as "Unknown".
*/
inline std::ostream& operator<<(std::ostream& os, AirPodsNoiseControlMode mode) {
    switch (mode) {
        case AirPodsNoiseControlMode::Off: return os << "Off";
        case AirPodsNoiseControlMode::NoiseCancellation: return os << "Noise Cancellation";
        case AirPodsNoiseControlMode::Transparency: return os << "Transparency";
        case AirPodsNoiseControlMode::Adaptive: return os << "Adaptive";
        default: return os << "Unknown";
    }
}


