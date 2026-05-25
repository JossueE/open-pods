#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <memory>
#include <mutex>
#include <cctype>
#include <utility>

#include "audio/audio_backend.hpp"

// DEVELOPER NOTE: We use final because we don't want to allow further inheritance from this class. 
class PulseAudioBackend final : public AudioBackend { 
public:
    PulseAudioBackend();
    
    ~PulseAudioBackend() override;

    PulseAudioBackend(const PulseAudioBackend&) = delete;
    PulseAudioBackend& operator=(const PulseAudioBackend&) = delete;

    PulseAudioBackend(PulseAudioBackend&&) = delete;
    PulseAudioBackend& operator=(PulseAudioBackend&&) = delete;

    std::optional<uint32_t> find_card_by_mac(std::string_view mac) override;
    bool is_a2dp_available(uint32_t card_index) override;
    bool is_profile_available(uint32_t card_index, std::string_view profile) override;
    std::optional<std::string> best_available_a2dp_profile(uint32_t card_index) override;
    bool set_card_profile(uint32_t card_index, std::string_view profile) override;

    std::optional<std::string> find_sink_by_mac(std::string_view mac) override;
    bool set_default_sink(std::string_view sink_name) override;
    bool move_all_sink_inputs(std::string_view sink_name) override;
    std::optional<uint32_t> get_sink_volume(std::string_view sink_name) override;
    bool set_sink_volume(std::string_view sink_name, uint32_t percent) override;
    bool suspend_sink(std::string_view sink_name, bool suspend) override;
    bool has_active_sink_input(std::string_view sink_name) override;

private:

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;
};
