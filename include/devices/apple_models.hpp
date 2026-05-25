#pragma once

#include <optional>
#include <string_view>
#include <cstdint>
#include <string>
#include <utility>


// Apple Inc. Bluetooth Company Identifier
constexpr uint16_t APPLE_COMPANY_ID {0x004c};

// Check Spatial Audio compatibility for Apple models
struct AppleModelInfo {
    std::string_view model_name;
    bool has_noise_cancellation;
    bool has_adaptive_transparency;
    bool has_stem_tap_gestures;
    bool has_conversation_awareness;
};

class AppleModels {
    public:
        /**
         * @brief Returns model information based on the Apple product ID.
         * @return An AppleModelInfo struct containing the model name and feature flags.
         * @note If the product ID is not recognized, a default "Apple Headphones" model with basic features is returned.
        */
        AppleModelInfo model_info(uint16_t product_id) noexcept;

        /**
         * @brief Determines if the Apple product ID requires extended AAP initialization.
         * @return True if extended AAP initialization is required; otherwise false.
         * @note Extended AAP initialization is necessary for certain models with advanced features.
        */
        bool needs_aap_init_ext(uint16_t product_id) noexcept;

        /**
         * @brief Parses a modalias string into Apple company and product IDs.
         * @return A pair containing the company ID and product ID, or std::nullopt if parsing fails.
         * @note The modalias must match the expected format for Apple devices.
        */
        std::optional<std::pair<uint16_t, uint16_t>> parse_modalias(const std::string& modalias);
};
