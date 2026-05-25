#pragma once

#include <string>

/**
 * @brief Stores Bluetooth LE keys associated with AirPods.
 */
struct AirPodsLEKeys {
    std::string irk;
    std::string enc_key;
};

/**
 * @brief Stores persistent identity and firmware information reported by AirPods.
 */
struct AirPodsInformation {
    std::string name;
    std::string model_number;
    std::string manufacturer;
    std::string serial_number;
    std::string version1;
    std::string version2;
    std::string hardware_revision;
    std::string updater_identifier;
    std::string left_serial_number;
    std::string right_serial_number;
    std::string version3;
    AirPodsLEKeys le_keys;
};
