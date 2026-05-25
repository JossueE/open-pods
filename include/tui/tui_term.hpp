#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace tui {

struct TerminalSize {
    uint16_t cols = 0;
    uint16_t rows = 0;
};

/**
 * @brief Owns the terminal in raw / alt-screen mode for the lifetime of the
 *        TUI loop. Restoring the original termios is RAII.
 */
class TerminalSession {
public:
    TerminalSession();
    ~TerminalSession();

    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    [[nodiscard]] bool ready() const { return ready_; }

    /**
     * @brief Returns and clears the resize-pending flag set by SIGWINCH.
     */
    [[nodiscard]] bool consume_resize_event();

    /**
     * @brief Reads a key (or escape sequence) from stdin with a timeout.
     *        Returns std::nullopt on timeout, EOF, or signal interruption.
     */
    [[nodiscard]] std::optional<std::string> read_key(int timeout_ms);

    /**
     * @brief Queries the current terminal size via TIOCGWINSZ.
     */
    [[nodiscard]] static TerminalSize size();

    /**
     * @brief Writes raw bytes to stdout, retrying on partial writes / EINTR.
     */
    static void write_raw(const std::string& data);

private:
    bool ready_ = false;
    int original_flags_ = 0;
    bool restored_ = false;
};

/**
 * @brief Set when the process receives SIGWINCH. The TUI loop polls and clears
 *        it via TerminalSession::consume_resize_event().
 */
extern std::atomic_bool g_resize_pending;

/**
 * @brief Set when the process receives SIGINT/SIGTERM/SIGHUP, signalling the
 *        TUI loop to shut down cleanly.
 */
extern std::atomic_bool g_shutdown_pending;

} // namespace tui
