#pragma once

#include <iostream>
#include <mutex>
#include <ostream>

namespace utils {

void set_debug_logging(bool enabled);
bool debug_logging_enabled();

namespace detail {
    std::mutex& log_mutex();
}

} // namespace utils

/**
 * @brief Emit an unconditional log line on stderr; thread-safe.
 */
#define OPENPODS_LOG(...)                                                       \
    do {                                                                        \
        std::lock_guard<std::mutex> _lock(::utils::detail::log_mutex());        \
        std::cerr << __VA_ARGS__ << '\n';                                       \
    } while (0)

/**
 * @brief Emit a debug log line on stderr only when --debug is set.
 */
#define OPENPODS_DEBUG(...)                                                     \
    do {                                                                        \
        if (::utils::debug_logging_enabled()) {                                 \
            std::lock_guard<std::mutex> _lock(::utils::detail::log_mutex());    \
            std::cerr << "[debug] " << __VA_ARGS__ << '\n';                     \
        }                                                                       \
    } while (0)
