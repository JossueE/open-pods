#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    virtual std::optional<uint32_t> find_card_by_mac(std::string_view mac) = 0;

    virtual bool is_a2dp_available(uint32_t card_index) = 0;

    virtual bool is_profile_available(uint32_t card_index, std::string_view profile) = 0;
    
    virtual std::optional<std::string> best_available_a2dp_profile(uint32_t card_index) = 0;

    virtual bool set_card_profile(uint32_t card_index, std::string_view profile) = 0;

    virtual std::optional<std::string> find_sink_by_mac(std::string_view mac) = 0;

    virtual bool set_default_sink(std::string_view sink_name) = 0;

    virtual bool move_all_sink_inputs(std::string_view sink_name) = 0;

    virtual std::optional<uint32_t> get_sink_volume(std::string_view sink_name) = 0;

    virtual bool set_sink_volume(std::string_view sink_name, uint32_t percent) = 0;

    virtual bool suspend_sink(std::string_view sink_name, bool suspend) = 0;
    
    virtual bool has_active_sink_input(std::string_view sink_name) = 0;
};