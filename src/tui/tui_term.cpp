#include "tui/tui_term.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace tui {

std::atomic_bool g_resize_pending {true}; // force initial layout query
std::atomic_bool g_shutdown_pending {false};

namespace {

termios g_saved_termios {};
bool g_termios_saved = false;
bool g_alt_screen_active = false;

void on_winch(int) {
    g_resize_pending.store(true, std::memory_order_relaxed);
}

void on_terminate(int) {
    g_shutdown_pending.store(true, std::memory_order_relaxed);
}

void enter_alt_screen() {
    // Switch to the alternate screen buffer and hide the cursor. Matches what
    // ratatui's crossterm backend does on init: the user's previous shell view
    // is preserved and restored on exit.
    constexpr std::string_view enter = "\x1b[?1049h\x1b[?25l\x1b[H\x1b[2J";
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(enter.size())) {
        const ssize_t written = ::write(
            STDOUT_FILENO,
            enter.data() + total,
            enter.size() - total
        );
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        total += written;
    }
    g_alt_screen_active = true;
}

void leave_alt_screen() {
    if (!g_alt_screen_active) {
        return;
    }
    constexpr std::string_view leave = "\x1b[?25h\x1b[?1049l";
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(leave.size())) {
        const ssize_t written = ::write(
            STDOUT_FILENO,
            leave.data() + total,
            leave.size() - total
        );
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        total += written;
    }
    g_alt_screen_active = false;
}

void install_signal_handlers() {
    struct sigaction winch_action {};
    winch_action.sa_handler = on_winch;
    sigemptyset(&winch_action.sa_mask);
    winch_action.sa_flags = SA_RESTART;
    ::sigaction(SIGWINCH, &winch_action, nullptr);

    struct sigaction term_action {};
    term_action.sa_handler = on_terminate;
    sigemptyset(&term_action.sa_mask);
    term_action.sa_flags = 0;
    ::sigaction(SIGINT, &term_action, nullptr);
    ::sigaction(SIGTERM, &term_action, nullptr);
    ::sigaction(SIGHUP, &term_action, nullptr);
}

} // namespace

TerminalSession::TerminalSession() {
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        return;
    }

    if (::tcgetattr(STDIN_FILENO, &g_saved_termios) != 0) {
        return;
    }
    g_termios_saved = true;

    termios raw = g_saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        g_termios_saved = false;
        return;
    }

    original_flags_ = ::fcntl(STDIN_FILENO, F_GETFL, 0);
    ::fcntl(STDIN_FILENO, F_SETFL, original_flags_ | O_NONBLOCK);

    enter_alt_screen();
    install_signal_handlers();
    ready_ = true;
}

TerminalSession::~TerminalSession() {
    if (restored_) {
        return;
    }
    leave_alt_screen();
    if (g_termios_saved) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
    }
    if (original_flags_ != 0) {
        ::fcntl(STDIN_FILENO, F_SETFL, original_flags_);
    }
    restored_ = true;
}

bool TerminalSession::consume_resize_event() {
    return g_resize_pending.exchange(false, std::memory_order_relaxed);
}

std::optional<std::string> TerminalSession::read_key(int timeout_ms) {
    pollfd fd {};
    fd.fd = STDIN_FILENO;
    fd.events = POLLIN;

    const int rc = ::poll(&fd, 1, timeout_ms);
    if (rc <= 0) {
        return std::nullopt;
    }

    std::array<char, 32> buffer {};
    const ssize_t read = ::read(STDIN_FILENO, buffer.data(), buffer.size());
    if (read <= 0) {
        return std::nullopt;
    }

    return std::string{buffer.data(), static_cast<size_t>(read)};
}

TerminalSize TerminalSession::size() {
    winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
        return TerminalSize{0, 0};
    }
    return TerminalSize{ws.ws_col, ws.ws_row};
}

void TerminalSession::write_raw(const std::string& data) {
    size_t total = 0;
    while (total < data.size()) {
        const ssize_t written = ::write(
            STDOUT_FILENO,
            data.data() + total,
            data.size() - total
        );
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        total += static_cast<size_t>(written);
    }
}

} // namespace tui
