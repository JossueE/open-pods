#include "utils/logging.hpp"

#include <atomic>

namespace utils {

namespace {

std::atomic_bool g_debug_enabled {false};

} // namespace

void set_debug_logging(bool enabled)
{
    g_debug_enabled = enabled;
}

bool debug_logging_enabled()
{
    return g_debug_enabled.load(std::memory_order_relaxed);
}

namespace detail {

std::mutex& log_mutex()
{
    static std::mutex mutex;
    return mutex;
}

} // namespace detail

} // namespace utils
