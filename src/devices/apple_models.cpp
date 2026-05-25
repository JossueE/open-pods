#include "devices/apple_models.hpp"

#include <charconv>
#include <system_error>

AppleModelInfo AppleModels::model_info(uint16_t product_id) noexcept {
    switch (product_id) {
        case 0x2002: return {"AirPods (1st gen)", false, false, false, false};
        case 0x200f: return {"AirPods (2nd gen)", false, false, false, false};
        case 0x2013: return {"AirPods (3rd gen)", false, false, true, false};
        case 0x2019: return {"AirPods (4th gen)", false, false, true, false};
        case 0x201b: return {"AirPods 4 ANC", true, true, true, true};
        case 0x200e: return {"AirPods Pro", true, false, true, false};
        case 0x2014: return {"AirPods Pro 2", true, true, true, true};
        case 0x2027: return {"AirPods Pro 3", true, true, true, true};
        case 0x2024: return {"AirPods Pro (USB-C)", true, true, true, true};
        case 0x200a: return {"AirPods Max", true, false, false, false};
        case 0x201f: return {"AirPods Max (2024)", true, false, false, false};
        case 0x202d: return {"AirPods Max 2", true, true, false, true};
        case 0x200b: return {"Powerbeats Pro", false, false, false, false};
        case 0x201d: return {"Powerbeats Pro 2", true, false, false, false};
        case 0x202f: return {"Powerbeats Fit", true, false, false, false};
        case 0x2006: return {"Beats Solo3", false, false, false, false};
        case 0x200c: return {"Beats Solo Pro", true, false, false, false};
        case 0x2009: return {"Beats Studio3", true, false, false, false};
        case 0x2005: return {"Beats X", false, false, false, false};
        case 0x2010: return {"Beats Flex", false, false, false, false};
        case 0x2003: return {"Powerbeats3", false, false, false, false};
        case 0x200d: return {"Powerbeats4", false, false, false, false};
        case 0x2012: return {"Beats Fit Pro", true, false, false, false};
        case 0x2011: return {"Beats Studio Buds", true, false, false, false};
        case 0x2016: return {"Beats Studio Buds+", true, false, false, false};
        case 0x2017: return {"Beats Studio Pro", true, false, false, false};
        case 0x2025: return {"Beats Solo 4", true, false, false, false};
        case 0x2026: return {"Beats Solo Buds", false, false, false, false};
        default: return {"Apple Headphones", true, false, false, false};
    }
}

bool AppleModels::needs_aap_init_ext(uint16_t product_id) noexcept {
    switch (product_id) {
        case 0x201b: // AirPods 4 ANC
        case 0x2014: // AirPods Pro 2
        case 0x2027: // AirPods Pro 3
        case 0x2024: // AirPods Pro (USB-C)
        case 0x202d: // AirPods Max 2
            return true;
        default:
            return false;
    }
}

std::optional<std::pair<uint16_t,uint16_t>> AppleModels::parse_modalias(const std::string& modalias) {
    auto parse_hex_u16 = [](std::string_view text) -> std::optional<uint16_t> {
        uint16_t value = 0;
        auto begin = text.data();
        auto end = text.data() + text.size();

        auto [ptr, ec] = std::from_chars(begin, end, value, 16);
        if (ec != std::errc{} || ptr != end) {
            return std::nullopt;
        }

        return value;
    };

    size_t vpos = modalias.find('v');
    if (vpos == std::string::npos || vpos + 5 > modalias.size()) {
        return std::nullopt;
    }

    size_t ppos = modalias.find('p');
    if (ppos == std::string::npos || ppos + 5 > modalias.size()) {
        return std::nullopt;
    }

    auto vendor = parse_hex_u16(std::string_view(modalias).substr(vpos + 1, 4));
    auto product = parse_hex_u16(std::string_view(modalias).substr(ppos + 1, 4));

    if (!vendor || !product) {
        return std::nullopt;
    }

    return std::make_pair(*vendor, *product);
}
