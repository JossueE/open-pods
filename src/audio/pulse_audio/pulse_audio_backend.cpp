#include "audio/pulse_audio/pulse_audio_backend.hpp"
#include "audio/pulse_audio/pulse_audio_types.hpp"

#include <mutex>

// NOTE: All PulseAudio mainloop iterations must come from one thread at a time.
// We serialize every public method with `mutex_` so multiple MediaController
// instances (one per AirPods device) plus the playback listener thread can share
// the same backend without corrupting the synchronous mainloop state.

namespace {

std::string to_upper(std::string_view value)
{
    std::string result;
    result.reserve(value.size());

    for (const unsigned char ch : value) {
        result.push_back(static_cast<char>(std::toupper(ch)));
    }

    return result;
}

void success_callback(pa_context*, int success, void* userdata)
{
    auto* result = static_cast<bool*>(userdata);
    *result = success != 0;
}

} // namespace

struct PulseAudioBackend::Impl {
    MainloopPtr mainloop;
    ContextPtr context;
    bool ready = false;

    void wait_for_operation(pa_operation* operation)
    {
        OperationPtr op(operation);

        if (op == nullptr) {
            return;
        }

        while (pa_operation_get_state(op.get()) == PA_OPERATION_RUNNING) {
            pa_mainloop_iterate(mainloop.get(), 1, nullptr);
        }
    }

    std::vector<OwnedCardInfo> card_info_list();
    std::vector<OwnedSinkInfo> sink_info_list();
    std::vector<uint32_t> sink_input_indices();
};

std::vector<OwnedCardInfo> PulseAudioBackend::Impl::card_info_list() {
    if (!ready || context == nullptr || mainloop == nullptr) {
        return {};
    }

    std::vector<OwnedCardInfo> cards;

    auto* operation = pa_context_get_card_info_list(
        context.get(),
        [](pa_context*, const pa_card_info* info, int eol, void* userdata) {
            if (eol != 0 || info == nullptr) {
                return;
            }

            auto* cards = static_cast<std::vector<OwnedCardInfo>*>(userdata);
            OwnedCardInfo card;
            card.index = info->index;

            if (info->proplist != nullptr) {
                card.proplist.reset(pa_proplist_copy(info->proplist));
            }

            if (info->profiles == nullptr) {
                cards->push_back(std::move(card));
                return;
            }

            for (uint32_t i = 0; i < info->n_profiles; ++i) {
                const auto& profile = info->profiles[i];
                card.profiles.push_back(OwnedCardProfileInfo {
                    profile.name != nullptr
                        ? std::optional<std::string>(profile.name)
                        : std::nullopt
                });
            }

            cards->push_back(std::move(card));
        },
        &cards
    );

    wait_for_operation(operation);
    return cards;
}

std::vector<OwnedSinkInfo> PulseAudioBackend::Impl::sink_info_list() {
    if (!ready || context == nullptr || mainloop == nullptr) {
        return {};
    }

    std::vector<OwnedSinkInfo> sinks;

    auto* operation = pa_context_get_sink_info_list(
        context.get(),
        [](pa_context*, const pa_sink_info* info, int eol, void* userdata) {
            if (eol != 0 || info == nullptr) {
                return;
            }

            auto* sinks = static_cast<std::vector<OwnedSinkInfo>*>(userdata);
            OwnedSinkInfo sink;
            sink.index = info->index;
            sink.name = info->name != nullptr
                ? std::optional<std::string>(info->name)
                : std::nullopt;
            sink.volume = info->volume;

            if (info->proplist != nullptr) {
                sink.proplist.reset(pa_proplist_copy(info->proplist));
            }

            sinks->push_back(std::move(sink));
        },
        &sinks
    );

    wait_for_operation(operation);
    return sinks;
}

std::vector<uint32_t> PulseAudioBackend::Impl::sink_input_indices()
{
    if (!ready || context == nullptr || mainloop == nullptr) {
        return {};
    }

    std::vector<uint32_t> indices;

    auto* operation = pa_context_get_sink_input_info_list(
        context.get(),
        [](pa_context*, const pa_sink_input_info* info, int eol, void* userdata) {
            if (eol != 0 || info == nullptr) {
                return;
            }

            auto* indices = static_cast<std::vector<uint32_t>*>(userdata);
            indices->push_back(info->index);
        },
        &indices
    );

    wait_for_operation(operation);
    return indices;
}

PulseAudioBackend::PulseAudioBackend() : impl_(std::make_unique<Impl>()) {
    impl_->mainloop.reset(pa_mainloop_new());

    if (impl_->mainloop == nullptr) {
        return;
    }

    auto* api = pa_mainloop_get_api(impl_->mainloop.get());
    impl_->context.reset(pa_context_new(api, "open-pods"));

    if (impl_->context == nullptr) {
        impl_->mainloop.reset();
        return;
    }

    if (pa_context_connect(impl_->context.get(), nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
        impl_->context.reset();
        impl_->mainloop.reset();
        return;
    }

    while (true) {
        pa_mainloop_iterate(impl_->mainloop.get(), 1, nullptr);

        const auto state = pa_context_get_state(impl_->context.get());

        if (state == PA_CONTEXT_READY) {
            impl_->ready = true;
            break;
        }

        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            impl_->context.reset();
            impl_->mainloop.reset();
            return;
        }
    }
}

PulseAudioBackend::~PulseAudioBackend() = default;

std::optional<uint32_t> PulseAudioBackend::find_card_by_mac(std::string_view mac) {
    std::lock_guard lock{mutex_};
    const auto cards = impl_->card_info_list();

    for (const auto& card : cards) {
        if (card.proplist == nullptr) {
            continue;
        }

        const char* device_string = pa_proplist_gets(card.proplist.get(), "device.string");
        if (device_string == nullptr) {
            continue;
        }

        if (std::string_view(device_string).find(mac) != std::string_view::npos) {
            return card.index;
        }
    }

    return std::nullopt;
}

bool PulseAudioBackend::is_a2dp_available(uint32_t card_index) {
    std::lock_guard lock{mutex_};
    const auto cards = impl_->card_info_list();

    for (const auto& card : cards) {
        if (card.index != card_index) {
            continue;
        }

        for (const auto& profile : card.profiles) {
            if (profile.name.has_value()
                && profile.name->starts_with("a2dp-sink")) {
                return true;
            }
        }

        return false;
    }

    return false;
}

bool PulseAudioBackend::is_profile_available(uint32_t card_index, std::string_view profile) {
    std::lock_guard lock{mutex_};
    const auto cards = impl_->card_info_list();

    for (const auto& card : cards) {
        if (card.index != card_index) {
            continue;
        }

        for (const auto& card_profile : card.profiles) {
            if (card_profile.name.has_value() && *card_profile.name == profile) {
                return true;
            }
        }

        return false;
    }

    return false;
}

std::optional<std::string> PulseAudioBackend::best_available_a2dp_profile(uint32_t card_index) {
    std::lock_guard lock{mutex_};

    // Prefer AAC for AirPods when available, then fall back to SBC variants.
    static constexpr std::string_view preferred_profiles[] = {
        "a2dp-sink-aac",
        "a2dp-sink-sbc_xq",
        "a2dp-sink-sbc",
        "a2dp-sink",
    };

    const auto cards = impl_->card_info_list();
    for (const auto profile : preferred_profiles) {
        for (const auto& card : cards) {
            if (card.index != card_index) {
                continue;
            }

            for (const auto& card_profile : card.profiles) {
                if (card_profile.name.has_value() && *card_profile.name == profile) {
                    return std::string(profile);
                }
            }
            break;
        }
    }

    return std::nullopt;
}

bool PulseAudioBackend::set_card_profile(uint32_t card_index, std::string_view profile) {
    std::lock_guard lock{mutex_};

    if (!impl_->ready || impl_->context == nullptr) {
        return false;
    }

    const std::string profile_name(profile);
    bool success = false;

    auto* operation = pa_context_set_card_profile_by_index(
        impl_->context.get(),
        card_index,
        profile_name.c_str(),
        success_callback,
        &success
    );

    if (operation == nullptr) {
        return false;
    }

    impl_->wait_for_operation(operation);
    return success;
}

std::optional<std::string> PulseAudioBackend::find_sink_by_mac(std::string_view mac) {
    std::lock_guard lock{mutex_};
    const auto sinks = impl_->sink_info_list();
    const auto wanted_mac = to_upper(mac);

    for (const auto& sink : sinks) {
        if (!sink.name.has_value() || sink.proplist == nullptr) {
            continue;
        }

        const char* device_string = pa_proplist_gets(sink.proplist.get(), "device.string");
        if (device_string != nullptr
            && to_upper(device_string).find(wanted_mac) != std::string::npos) {
            return *sink.name;
        }

        const char* bluez_path = pa_proplist_gets(sink.proplist.get(), "bluez.path");
        if (bluez_path == nullptr) {
            continue;
        }

        std::string path(bluez_path);
        const auto last_separator = path.find_last_of('/');
        std::string mac_from_path = last_separator == std::string::npos
            ? path
            : path.substr(last_separator + 1);

        if (mac_from_path.starts_with("dev_")) {
            mac_from_path.erase(0, 4);
        }

        for (auto& ch : mac_from_path) {
            if (ch == '_') {
                ch = ':';
            }
        }

        if (to_upper(mac_from_path) == wanted_mac) {
            return *sink.name;
        }
    }

    return std::nullopt;
}

bool PulseAudioBackend::set_default_sink(std::string_view sink_name) {
    std::lock_guard lock{mutex_};

    if (!impl_->ready || impl_->context == nullptr) {
        return false;
    }

    const std::string sink(sink_name);
    bool success = false;

    auto* operation = pa_context_set_default_sink(
        impl_->context.get(),
        sink.c_str(),
        success_callback,
        &success
    );

    if (operation == nullptr) {
        return false;
    }

    impl_->wait_for_operation(operation);
    return success;
}

bool PulseAudioBackend::move_all_sink_inputs(std::string_view sink_name) {
    std::lock_guard lock{mutex_};

    if (!impl_->ready || impl_->context == nullptr) {
        return false;
    }

    const std::string sink(sink_name);
    bool all_succeeded = true;

    for (const auto index : impl_->sink_input_indices()) {
        bool success = false;
        auto* operation = pa_context_move_sink_input_by_name(
            impl_->context.get(),
            index,
            sink.c_str(),
            success_callback,
            &success
        );

        if (operation == nullptr) {
            all_succeeded = false;
            continue;
        }

        impl_->wait_for_operation(operation);
        all_succeeded = all_succeeded && success;
    }

    return all_succeeded;
}

std::optional<uint32_t> PulseAudioBackend::get_sink_volume(std::string_view sink_name) {
    std::lock_guard lock{mutex_};
    const auto sinks = impl_->sink_info_list();

    for (const auto& sink : sinks) {
        if (!sink.name.has_value() || *sink.name != sink_name) {
            continue;
        }

        if (sink.volume.channels == 0) {
            return std::nullopt;
        }

        uint64_t total = 0;
        for (uint8_t channel = 0; channel < sink.volume.channels; ++channel) {
            total += sink.volume.values[channel];
        }

        const auto average = total / sink.volume.channels;
        const auto percent = static_cast<uint32_t>(
            (average * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM
        );
        return percent;
    }

    return std::nullopt;
}

bool PulseAudioBackend::set_sink_volume(std::string_view sink_name, uint32_t percent) {
    std::lock_guard lock{mutex_};

    if (!impl_->ready || impl_->context == nullptr) {
        return false;
    }

    const auto sinks = impl_->sink_info_list();
    std::optional<uint8_t> channels;

    for (const auto& sink : sinks) {
        if (sink.name.has_value() && *sink.name == sink_name) {
            channels = sink.volume.channels;
            break;
        }
    }

    if (!channels.has_value() || *channels == 0) {
        return false;
    }

    const std::string sink(sink_name);
    const auto raw_volume = static_cast<pa_volume_t>(
        (static_cast<uint64_t>(percent) * PA_VOLUME_NORM + 50) / 100
    );

    pa_cvolume volume {};
    pa_cvolume_set(&volume, *channels, raw_volume);

    bool success = false;
    auto* operation = pa_context_set_sink_volume_by_name(
        impl_->context.get(),
        sink.c_str(),
        &volume,
        success_callback,
        &success
    );

    if (operation == nullptr) {
        return false;
    }

    impl_->wait_for_operation(operation);
    return success;
}

bool PulseAudioBackend::suspend_sink(std::string_view sink_name, bool suspend) {
    std::lock_guard lock{mutex_};

    if (!impl_->ready || impl_->context == nullptr) {
        return false;
    }

    const std::string sink(sink_name);
    bool success = false;

    auto* operation = pa_context_suspend_sink_by_name(
        impl_->context.get(),
        sink.c_str(),
        suspend ? 1 : 0,
        success_callback,
        &success
    );

    if (operation == nullptr) {
        return false;
    }

    impl_->wait_for_operation(operation);
    return success;
}

bool PulseAudioBackend::has_active_sink_input(std::string_view sink_name) {
    std::lock_guard lock{mutex_};

    if (!impl_->ready || impl_->context == nullptr) {
        return false;
    }

    const auto sinks = impl_->sink_info_list();
    std::optional<uint32_t> sink_index;

    for (const auto& sink : sinks) {
        if (sink.name.has_value() && *sink.name == sink_name) {
            sink_index = sink.index;
            break;
        }
    }

    if (!sink_index.has_value()) {
        return false;
    }

    const auto target_sink_index = *sink_index;
    std::pair<uint32_t, bool> payload { target_sink_index, false };

    auto* operation = pa_context_get_sink_input_info_list(
        impl_->context.get(),
        [](pa_context*, const pa_sink_input_info* info, int eol, void* userdata) {
            if (eol != 0 || info == nullptr) {
                return;
            }

            auto* payload = static_cast<std::pair<uint32_t, bool>*>(userdata);
            if (info->sink == payload->first && info->corked == 0) {
                payload->second = true;
            }
        },
        &payload
    );

    if (operation == nullptr) {
        return false;
    }

    impl_->wait_for_operation(operation);
    return payload.second;
}
