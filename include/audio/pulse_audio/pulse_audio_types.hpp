#pragma once

#include <pulse/pulseaudio.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct PaProplistDeleter {
    void operator()(pa_proplist* value) const noexcept
    {
        if (value != nullptr) {
            pa_proplist_free(value);
        }
    }
};

using ProplistPtr = std::unique_ptr<pa_proplist, PaProplistDeleter>;

struct PaMainloopDeleter {
    void operator()(pa_mainloop* value) const noexcept
    {
        if (value != nullptr) {
            pa_mainloop_free(value);
        }
    }
};

using MainloopPtr = std::unique_ptr<pa_mainloop, PaMainloopDeleter>;

struct PaContextDeleter {
    void operator()(pa_context* value) const noexcept
    {
        if (value != nullptr) {
            pa_context_unref(value);
        }
    }
};

using ContextPtr = std::unique_ptr<pa_context, PaContextDeleter>;

struct PaOperationDeleter {
    void operator()(pa_operation* value) const noexcept
    {
        if (value != nullptr) {
            pa_operation_unref(value);
        }
    }
};

using OperationPtr = std::unique_ptr<pa_operation, PaOperationDeleter>;

struct OwnedCardProfileInfo {
    std::optional<std::string> name;
};

struct OwnedCardInfo {
    uint32_t index = 0;
    ProplistPtr proplist;
    std::vector<OwnedCardProfileInfo> profiles;
};

struct OwnedSinkInfo {
    uint32_t index = 0;
    std::optional<std::string> name;
    ProplistPtr proplist;
    pa_cvolume volume {};
};
