#include "utils/devices.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

namespace utils {

namespace {

using nlohmann::json;

json read_devices_file(const std::filesystem::path& path)
{
    std::ifstream input{path};
    if (!input) {
        return json::object();
    }

    try {
        json document = json::parse(input, /*cb*/ nullptr, /*allow_exceptions*/ true);
        if (!document.is_object()) {
            return json::object();
        }
        return document;
    } catch (const std::exception&) {
        return json::object();
    }
}

bool write_devices_file(const std::filesystem::path& path, const json& document)
{
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    std::ofstream output{path, std::ios::trunc};
    if (!output) {
        return false;
    }

    output << document.dump(2);
    return output.good();
}

json information_to_json(const AirPodsInformation& info)
{
    return json{
        {"name", info.name},
        {"model_number", info.model_number},
        {"manufacturer", info.manufacturer},
        {"serial_number", info.serial_number},
        {"version1", info.version1},
        {"version2", info.version2},
        {"hardware_revision", info.hardware_revision},
        {"updater_identifier", info.updater_identifier},
        {"left_serial_number", info.left_serial_number},
        {"right_serial_number", info.right_serial_number},
        {"version3", info.version3},
        {"le_keys", json{
            {"irk", info.le_keys.irk},
            {"enc_key", info.le_keys.enc_key}
        }}
    };
}

} // namespace

std::filesystem::path get_devices_path() {
    if (const char* xdg_data_home = std::getenv("XDG_DATA_HOME")) {
        if (xdg_data_home[0] != '\0') {
            return std::filesystem::path{xdg_data_home} / "airpods-tui" / "devices.json";
        }
    }

    if (const char* home = std::getenv("HOME")) {
        if (home[0] != '\0') {
            return std::filesystem::path{home}
                / ".local"
                / "share"
                / "airpods-tui"
                / "devices.json";
        }
    }

    return std::filesystem::path{".local"} / "share" / "airpods-tui" / "devices.json";
}

bool persist_device_information(const std::string& mac, const AirPodsInformation& info)
{
    const auto path = get_devices_path();
    json document = read_devices_file(path);

    json& entry = document[mac];
    if (!entry.is_object()) {
        entry = json::object();
    }
    entry["name"] = info.name;
    entry["type_"] = "AirPods";
    entry["information"] = json{
        {"kind", "AirPods"},
        {"data", information_to_json(info)}
    };

    return write_devices_file(path, document);
}

bool persist_device_name(const std::string& mac, const std::string& name)
{
    const auto path = get_devices_path();
    json document = read_devices_file(path);

    json& entry = document[mac];
    if (!entry.is_object()) {
        entry = json::object();
        entry["type_"] = "AirPods";
    }
    entry["name"] = name;
    return write_devices_file(path, document);
}

} // namespace utils
