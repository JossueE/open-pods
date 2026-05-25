#include "utils/runtime.hpp"

#include <cstdlib>
#include <fstream>

namespace utils {

std::optional<std::filesystem::path> runtime_dir() {
    const char* value = std::getenv("XDG_RUNTIME_DIR");
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }

    return std::filesystem::path{value};
}

bool write_battery_env(
    std::optional<uint8_t> left,
    std::optional<uint8_t> right,
    std::optional<uint8_t> case_battery,
    std::optional<uint8_t> headphone
) {
    const auto dir = runtime_dir();
    if (!dir) {
        return false;
    }

    std::ofstream file{*dir / "airpods-battery.env", std::ios::trunc};
    if (!file) {
        return false;
    }

    if (left) {
        file << "LEFT=" << static_cast<int>(*left) << '\n';
    }
    if (right) {
        file << "RIGHT=" << static_cast<int>(*right) << '\n';
    }
    if (case_battery) {
        file << "CASE=" << static_cast<int>(*case_battery) << '\n';
    }
    if (headphone) {
        file << "HEADPHONE=" << static_cast<int>(*headphone) << '\n';
    }

    return file.good();
}

} // namespace utils
