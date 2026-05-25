#include "tui/tui_render.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace tui {

namespace {

// ──────────────────────── Glyphs ─────────────────────────

// Rounded box drawing.
constexpr const char* TL = "\xE2\x95\xAD"; // ╭
constexpr const char* TR = "\xE2\x95\xAE"; // ╮
constexpr const char* BL = "\xE2\x95\xB0"; // ╰
constexpr const char* BR = "\xE2\x95\xAF"; // ╯
constexpr const char* HZ = "\xE2\x94\x80"; // ─
constexpr const char* VR = "\xE2\x94\x82"; // │

constexpr const char* BAR_FULL = "\xE2\x96\x88";  // █
constexpr const char* BAR_EMPTY = "\xE2\x96\x91"; // ░
constexpr const char* RADIO_ON = "\xE2\x97\x89";  // ◉
constexpr const char* RADIO_OFF = "\xE2\x97\x8B"; // ○
constexpr const char* DOT_FILLED = "\xE2\x97\x8F";   // ●
constexpr const char* DOT_OUTLINE = "\xE2\x97\x8C";  // ◌
constexpr const char* CURSOR = "\xE2\x9D\xAF";    // ❯

// ──────────────────────── Palette ────────────────────────
//
// Goal: the chrome (borders, titles, labels, values) is white. Color is
// reserved for *state* indicators only — battery level, connection, ear
// detection, runtime warnings.
constexpr const char* RESET = "\x1b[0m";
constexpr const char* BOLD = "\x1b[1m";
constexpr const char* WHITE = "\x1b[97m";          // bright white (focused chrome)
constexpr const char* MUTED = "\x1b[38;5;245m";    // light gray (inactive labels)
constexpr const char* MUTED_DARK = "\x1b[38;5;240m"; // gray (inactive borders)

// Reserved for *state* — never used as decoration.
constexpr const char* GREEN = "\x1b[38;5;83m";
constexpr const char* AMBER = "\x1b[38;5;221m";
constexpr const char* RED = "\x1b[38;5;203m";

// ───────────────────── Sizing ─────────────────────────
constexpr uint16_t MIN_COLS = 60;
constexpr uint16_t MIN_ROWS = 24;

// ──────────────────────── Logo ────────────────────────
//
// 5-row "ANSI Shadow" style. Each line is 72 visible cells wide, so it fits
// comfortably inside an 80-col terminal once you subtract panel borders.
constexpr std::array<std::string_view, 5> LOGO_BIG = {
    " \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88    \xE2\x96\x88\xE2\x96\x88     \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88   \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88 ",
    "\xE2\x96\x88\xE2\x96\x88    \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88   \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88      \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88   \xE2\x96\x88\xE2\x96\x88     \xE2\x96\x88\xE2\x96\x88   \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88    \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88   \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88      ",
    "\xE2\x96\x88\xE2\x96\x88    \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88   \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88     \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88    \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88   \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88 ",
    "\xE2\x96\x88\xE2\x96\x88    \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88      \xE2\x96\x88\xE2\x96\x88      \xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88     \xE2\x96\x88\xE2\x96\x88      \xE2\x96\x88\xE2\x96\x88    \xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88   \xE2\x96\x88\xE2\x96\x88      \xE2\x96\x88\xE2\x96\x88 ",
    " \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88      \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x88   \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88     \xE2\x96\x88\xE2\x96\x88       \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88  \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88 ",
};
constexpr std::size_t LOGO_BIG_WIDTH = 72;

// 2-row half-block fallback for narrow terminals (< 78 cols).
constexpr std::array<std::string_view, 2> LOGO_SMALL = {
    "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x80\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x80\xE2\x96\x80 \xE2\x96\x88\xE2\x96\x84 \xE2\x96\x88   \xE2\x96\x88\xE2\x96\x80\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x80\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x80\xE2\x96\x84 \xE2\x96\x88\xE2\x96\x80",
    "\xE2\x96\x88\xE2\x96\x84\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x80\xE2\x96\x80 \xE2\x96\x88\xE2\x96\x84\xE2\x96\x84 \xE2\x96\x88 \xE2\x96\x80\xE2\x96\x88   \xE2\x96\x88\xE2\x96\x80\xE2\x96\x80 \xE2\x96\x88\xE2\x96\x84\xE2\x96\x88 \xE2\x96\x88\xE2\x96\x84\xE2\x96\x80 \xE2\x96\x84\xE2\x96\x88",
};
constexpr std::size_t LOGO_SMALL_WIDTH = 33;

// ─────────────────── ANSI-aware string helpers ───────────────────

/**
 * @brief CSI escape parser state used by visible_width / ellipsize.
 *  0 → outside an escape sequence
 *  1 → just saw ESC, expecting the CSI marker `[`
 *  2 → reading CSI parameters until a final byte in 0x40..0x7E
 */
int advance_escape_state(int state, unsigned char byte) {
    switch (state) {
    case 1:
        return (byte == '[') ? 2 : 0;
    case 2:
        return (byte >= 0x40 && byte <= 0x7E) ? 0 : 2;
    default:
        return (byte == 0x1B) ? 1 : 0;
    }
}

std::size_t visible_width(const std::string& text) {
    std::size_t width = 0;
    int escape_state = 0;
    for (std::size_t i = 0; i < text.size(); ) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (escape_state != 0 || byte == 0x1B) {
            escape_state = advance_escape_state(escape_state, byte);
            ++i;
            continue;
        }
        if ((byte & 0x80) == 0) {
            ++i; ++width;
        } else if ((byte & 0xE0) == 0xC0) {
            i += 2; ++width;
        } else if ((byte & 0xF0) == 0xE0) {
            i += 3; ++width;
        } else if ((byte & 0xF8) == 0xF0) {
            i += 4; ++width;
        } else {
            ++i;
        }
    }
    return width;
}

std::string repeat(const std::string& unit, std::size_t count) {
    std::string out;
    out.reserve(unit.size() * count);
    for (std::size_t i = 0; i < count; ++i) out += unit;
    return out;
}

std::string pad_right(const std::string& text, std::size_t target_width) {
    const auto width = visible_width(text);
    if (width >= target_width) return text;
    return text + std::string(target_width - width, ' ');
}

std::string pad_left(const std::string& text, std::size_t target_width) {
    const auto width = visible_width(text);
    if (width >= target_width) return text;
    return std::string(target_width - width, ' ') + text;
}

std::string ellipsize(const std::string& text, std::size_t max_visible) {
    if (max_visible == 0) return {};
    const auto width = visible_width(text);
    if (width <= max_visible) return text;
    if (max_visible <= 1) return ".";

    std::string truncated;
    std::size_t copied = 0;
    int escape_state = 0;
    for (std::size_t i = 0; i < text.size() && copied + 1 < max_visible; ) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (escape_state != 0 || byte == 0x1B) {
            escape_state = advance_escape_state(escape_state, byte);
            truncated.push_back(text[i++]);
            continue;
        }
        std::size_t bytes = 1;
        if ((byte & 0xF8) == 0xF0) bytes = 4;
        else if ((byte & 0xF0) == 0xE0) bytes = 3;
        else if ((byte & 0xE0) == 0xC0) bytes = 2;
        if (i + bytes > text.size()) break;
        truncated.append(text, i, bytes);
        i += bytes;
        ++copied;
    }
    truncated += "\xE2\x80\xA6"; // …
    return truncated;
}

std::string colored(std::string_view code, std::string_view text) {
    std::string out;
    out.reserve(code.size() + text.size() + 4);
    out.append(code);
    out.append(text);
    out.append(RESET);
    return out;
}

// ───────────────────── Box drawing ─────────────────────

const char* border_color_for(bool focused) {
    return focused ? WHITE : MUTED_DARK;
}

std::string border_char(const char* glyph, bool focused) {
    return colored(border_color_for(focused), std::string_view{glyph});
}

std::string box_top(std::size_t inner_width, std::string_view title, bool focused) {
    const auto color = border_color_for(focused);
    if (title.empty()) {
        std::string out;
        out += colored(color, std::string_view{TL});
        out += colored(color, repeat(HZ, inner_width));
        out += colored(color, std::string_view{TR});
        return out;
    }

    // ╭─ TITLE ───╮
    const std::string padded_title = std::string{" "} + std::string{title} + " ";
    const auto padded_width = visible_width(padded_title);
    if (padded_width + 3 > inner_width) {
        return box_top(inner_width, "", focused);
    }
    const std::size_t right_dashes = inner_width - 1 - padded_width;
    std::string out;
    out += colored(color, std::string_view{TL});
    out += colored(color, std::string_view{HZ});
    // Title is always white-bright so it's readable regardless of focus.
    if (focused) {
        out += colored(std::string{BOLD} + std::string{WHITE}, padded_title);
    } else {
        out += colored(MUTED, padded_title);
    }
    out += colored(color, repeat(HZ, right_dashes));
    out += colored(color, std::string_view{TR});
    return out;
}

std::string box_bottom(std::size_t inner_width, bool focused) {
    const auto color = border_color_for(focused);
    std::string out;
    out += colored(color, std::string_view{BL});
    out += colored(color, repeat(HZ, inner_width));
    out += colored(color, std::string_view{BR});
    return out;
}

std::string box_row(const std::string& content, std::size_t inner_width, bool focused) {
    const auto trimmed = ellipsize(content, inner_width);
    return border_char(VR, focused)
        + pad_right(trimmed, inner_width)
        + border_char(VR, focused);
}

std::string box_blank_row(std::size_t inner_width, bool focused) {
    return border_char(VR, focused)
        + std::string(inner_width, ' ')
        + border_char(VR, focused);
}

// ─────────────────── State indicator helpers ───────────────────

std::string_view battery_color(uint8_t pct) {
    if (pct < 20) return RED;
    if (pct < 50) return AMBER;
    return GREEN;
}

std::string battery_bar(std::optional<uint8_t> pct, std::size_t cells) {
    std::string out;
    if (cells == 0) return out;
    if (!pct) {
        out += colored(MUTED_DARK, repeat(BAR_EMPTY, cells));
        return out;
    }
    const auto p = std::min<uint8_t>(*pct, 100);
    const auto filled = static_cast<std::size_t>(
        (static_cast<std::size_t>(p) * cells + 50) / 100
    );
    out += colored(battery_color(p), repeat(BAR_FULL, std::min(filled, cells)));
    out += colored(MUTED_DARK, repeat(BAR_EMPTY, cells > filled ? cells - filled : 0));
    return out;
}

/**
 * @brief Returns "● ready" / "● charging" / "◌ closed" / "◌ disc" depending
 *        on the battery + component combo. The dot is the only colored thing.
 */
std::string battery_status_label(
    std::optional<BatteryStatus> status,
    std::optional<uint8_t> level,
    bool is_case
) {
    if (!status || !level) {
        return colored(MUTED_DARK, std::string_view{DOT_OUTLINE})
             + std::string{" "} + colored(MUTED, "—");
    }
    switch (*status) {
    case BatteryStatus::Charging:
        return colored(AMBER, std::string_view{DOT_FILLED})
             + std::string{" charging"};
    case BatteryStatus::InUse:
        return colored(GREEN, std::string_view{DOT_FILLED})
             + std::string{" active"};
    case BatteryStatus::NotCharging:
        if (*level == 0) {
            return colored(RED, std::string_view{DOT_FILLED})
                 + std::string{" empty"};
        }
        if (is_case) {
            return colored(GREEN, std::string_view{DOT_OUTLINE})
                 + std::string{" closed"};
        }
        return colored(GREEN, std::string_view{DOT_FILLED})
             + std::string{" ready"};
    case BatteryStatus::Disconnected:
        return colored(MUTED_DARK, std::string_view{DOT_OUTLINE})
             + std::string{" disconnected"};
    }
    return "";
}

std::string ear_value(std::optional<EarDetectionStatus> e) {
    if (!e) return colored(MUTED, "—");
    switch (*e) {
    case EarDetectionStatus::InEar:
        return colored(GREEN, "in");
    case EarDetectionStatus::OutOfEar:
        return colored(MUTED, "out");
    case EarDetectionStatus::InCase:
        return colored(MUTED, "case");
    case EarDetectionStatus::Disconnected:
        return colored(RED, "disc");
    }
    return colored(MUTED, "—");
}

const char* mode_label(ListeningMode mode) {
    switch (mode) {
    case ListeningMode::Off: return "Off";
    case ListeningMode::NoiseCancellation: return "Noise Cancellation";
    case ListeningMode::Transparency: return "Transparency";
    case ListeningMode::Adaptive: return "Adaptive Audio";
    }
    return "Unknown";
}

// ───────────────────── Generic row builders ─────────────────────

/**
 * @brief Builds a "❯ label …………… value" line, padded to `available` cells.
 *        Layout: [2sp][❯ or sp sp][label][middle pad][value][2sp].
 *
 * @param value_text          Pre-coloured value (may contain ANSI).
 * @param value_visible_width Visible width of `value_text` (caller-computed
 *                            because we can't strip ANSI cheaply here).
 * @param available           Inner width of the panel (interior of the box).
 */
std::string label_value_row(
    bool selected,
    bool focused,
    bool unsupported,
    std::string_view label,
    const std::string& value_text,
    std::size_t value_visible_width,
    std::size_t available
) {
    const std::string left_pad = "  ";
    const std::string cursor_part = selected
        ? colored(std::string{BOLD} + WHITE, std::string_view{CURSOR}) + std::string{" "}
        : std::string{"  "};

    std::string label_str;
    if (unsupported) {
        label_str = colored(MUTED_DARK, label);
    } else if (selected && focused) {
        label_str = colored(std::string{BOLD} + WHITE, label);
    } else if (focused) {
        label_str = colored(WHITE, label);
    } else {
        label_str = colored(MUTED, label);
    }

    const std::size_t taken = visible_width(left_pad)
        + 2  // cursor or 2 spaces
        + visible_width(label_str)
        + value_visible_width
        + 2; // right pad

    const std::size_t middle = (taken < available) ? available - taken : 0;
    std::string row;
    row += left_pad;
    row += cursor_part;
    row += label_str;
    row += std::string(middle, ' ');
    row += pad_left(value_text, value_visible_width);
    row += "  ";
    return row;
}

// ───────────────────── HEADER ─────────────────────

std::vector<std::string> render_header(
    const TuiState& state,
    std::size_t inner_width
) {
    std::vector<std::string> rows;
    rows.push_back(box_top(inner_width, "", false));
    rows.push_back(box_blank_row(inner_width, false));

    // Choose the largest logo that fits.
    if (inner_width >= LOGO_BIG_WIDTH + 4) {
        const std::size_t left = (inner_width - LOGO_BIG_WIDTH) / 2;
        const std::size_t right = inner_width - LOGO_BIG_WIDTH - left;
        for (const auto& line : LOGO_BIG) {
            std::string row;
            row.append(left, ' ');
            row += colored(WHITE, line);
            row.append(right, ' ');
            rows.push_back(border_char(VR, false) + row + border_char(VR, false));
        }
    } else if (inner_width >= LOGO_SMALL_WIDTH + 4) {
        const std::size_t left = (inner_width - LOGO_SMALL_WIDTH) / 2;
        const std::size_t right = inner_width - LOGO_SMALL_WIDTH - left;
        for (const auto& line : LOGO_SMALL) {
            std::string row;
            row.append(left, ' ');
            row += colored(WHITE, line);
            row.append(right, ' ');
            rows.push_back(border_char(VR, false) + row + border_char(VR, false));
        }
    } else {
        // No logo, just the brand text.
        const std::string brand = colored(std::string{BOLD} + WHITE, "OPEN-PODS");
        const std::size_t bw = visible_width(brand);
        const std::size_t left = (inner_width > bw) ? (inner_width - bw) / 2 : 0;
        std::string row;
        row.append(left, ' ');
        row += brand;
        rows.push_back(box_row(row, inner_width, false));
    }

    rows.push_back(box_blank_row(inner_width, false));

    // Status line: DEVICE name | STATUS dot | WEAR L:in R:in | DAEMON OK
    const auto& dev = state.device();
    std::string device_name = dev.name.empty()
        ? std::string{"<no device>"}
        : (dev.product_id != 0
            ? std::string{dev.model_info.model_name}
            : dev.name);
    if (device_name.empty()) device_name = "AirPods";

    std::string status_dot = dev.connected
        ? colored(GREEN, std::string_view{DOT_FILLED}) + std::string{" connected"}
        : colored(RED, std::string_view{DOT_FILLED}) + std::string{" disconnected"};

    std::string wear =
        colored(MUTED, "L:") + ear_value(dev.ear_left)
        + std::string{" "} + colored(MUTED, "R:") + ear_value(dev.ear_right);

    std::string daemon_status = state.audio_unavailable()
        ? colored(AMBER, "PA degraded")
        : (dev.connected ? colored(GREEN, "OK") : colored(MUTED, "idle"));

    std::string line;
    line += "  ";
    line += colored(MUTED, "DEVICE ");
    line += colored(WHITE, device_name);
    line += "    ";
    line += colored(MUTED, "STATUS ");
    line += status_dot;
    line += "    ";
    line += colored(MUTED, "WEAR ");
    line += wear;
    line += "    ";
    line += colored(MUTED, "DAEMON ");
    line += daemon_status;

    rows.push_back(box_row(line, inner_width, false));
    rows.push_back(box_blank_row(inner_width, false));
    rows.push_back(box_bottom(inner_width, false));
    return rows;
}

// ───────────────────── BATTERY ─────────────────────

std::vector<std::string> render_battery(
    const TuiState& state,
    std::size_t inner_width,
    bool focused
) {
    const auto& dev = state.device();
    const std::size_t selected_row = state.selected_row(Section::Battery);
    std::vector<std::string> rows;

    rows.push_back(box_top(inner_width, "BATTERY", focused));
    rows.push_back(box_blank_row(inner_width, focused));

    struct Slot {
        const char* label;
        std::optional<uint8_t> level;
        std::optional<BatteryStatus> status;
        bool is_case;
    };

    std::vector<Slot> slots;
    if (dev.headphone_battery) {
        slots.push_back({"Headset", dev.headphone_battery, dev.headphone_status, false});
        slots.push_back({"Left",  dev.left_battery,  dev.left_status,  false});
        slots.push_back({"Right", dev.right_battery, dev.right_status, false});
    } else {
        slots.push_back({"Left",  dev.left_battery,  dev.left_status,  false});
        slots.push_back({"Right", dev.right_battery, dev.right_status, false});
        slots.push_back({"Case",  dev.case_battery,  dev.case_status,  true});
    }

    // Battery rows have extra layout: [label 8] [bar 22] [pct 4] [status]
    constexpr std::size_t LABEL_W = 8;
    constexpr std::size_t BAR_W = 22;
    constexpr std::size_t PCT_W = 4;
    constexpr std::size_t STATUS_W = 16;
    const std::size_t row_min = 2 + 2 + LABEL_W + BAR_W + 2 + PCT_W + 3 + STATUS_W + 2;

    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto& s = slots[i];
        const bool selected = focused && (i == selected_row);

        std::string content;
        content += "  "; // left pad
        if (selected) {
            content += colored(std::string{BOLD} + WHITE, std::string_view{CURSOR}) + std::string{" "};
        } else {
            content += "  ";
        }

        // Label
        std::string label_str;
        if (selected && focused) {
            label_str = colored(std::string{BOLD} + WHITE, s.label);
        } else if (focused) {
            label_str = colored(WHITE, s.label);
        } else {
            label_str = colored(MUTED, s.label);
        }
        content += pad_right(label_str, LABEL_W);

        // Bar
        content += battery_bar(s.level, BAR_W);
        content += "  ";

        // Percentage
        if (s.level) {
            std::ostringstream pct;
            pct << std::setw(3) << static_cast<int>(*s.level) << "%";
            content += colored(battery_color(*s.level), pct.str());
        } else {
            content += colored(MUTED_DARK, " --%");
        }
        content += "   ";

        // Status
        content += battery_status_label(s.status, s.level, s.is_case);

        if (inner_width >= row_min) {
            rows.push_back(box_row(content, inner_width, focused));
        } else {
            // Fallback: drop the status label first.
            std::string compact;
            compact += "  ";
            compact += selected
                ? colored(std::string{BOLD} + WHITE, std::string_view{CURSOR}) + std::string{" "}
                : std::string{"  "};
            compact += pad_right(label_str, LABEL_W);
            compact += battery_bar(s.level, std::min(BAR_W, inner_width / 2));
            compact += "  ";
            if (s.level) {
                std::ostringstream pct;
                pct << std::setw(3) << static_cast<int>(*s.level) << "%";
                compact += colored(battery_color(*s.level), pct.str());
            } else {
                compact += colored(MUTED_DARK, " --%");
            }
            rows.push_back(box_row(compact, inner_width, focused));
        }
    }

    rows.push_back(box_blank_row(inner_width, focused));
    rows.push_back(box_bottom(inner_width, focused));
    return rows;
}

// ───────────────────── NOISE CONTROL ─────────────────────

std::vector<std::string> render_noise(
    const TuiState& state,
    std::size_t inner_width,
    bool focused
) {
    const auto& dev = state.device();
    const std::size_t selected_row = state.selected_row(Section::NoiseControl);
    std::vector<std::string> rows;

    rows.push_back(box_top(inner_width, "NOISE CONTROL", focused));
    rows.push_back(box_blank_row(inner_width, focused));

    // Listening modes (radio).
    constexpr std::array<ListeningMode, 4> MODES = {
        ListeningMode::Transparency,
        ListeningMode::NoiseCancellation,
        ListeningMode::Adaptive,
        ListeningMode::Off,
    };

    auto mode_supported = [&](ListeningMode mode) {
        switch (mode) {
        case ListeningMode::Off:
            return true;
        case ListeningMode::NoiseCancellation:
        case ListeningMode::Transparency:
            return dev.model_info.has_noise_cancellation;
        case ListeningMode::Adaptive:
            return dev.model_info.has_adaptive_transparency;
        }
        return false;
    };

    constexpr std::size_t STATUS_W = 8;
    for (std::size_t i = 0; i < MODES.size(); ++i) {
        const auto mode = MODES[i];
        const bool active = dev.listening_mode.has_value()
            && *dev.listening_mode == mode;
        const bool supported = mode_supported(mode);
        const bool selected = focused && (i == selected_row);

        std::string content;
        content += "  ";
        content += selected
            ? colored(std::string{BOLD} + WHITE, std::string_view{CURSOR}) + std::string{" "}
            : std::string{"  "};

        // Radio glyph
        if (active) {
            content += colored(GREEN, std::string_view{RADIO_ON});
        } else if (supported) {
            content += colored(MUTED, std::string_view{RADIO_OFF});
        } else {
            content += colored(MUTED_DARK, std::string_view{RADIO_OFF});
        }
        content += "  ";

        // Label
        const char* label = mode_label(mode);
        if (!supported) {
            content += colored(MUTED_DARK, label);
        } else if (selected && focused) {
            content += colored(std::string{BOLD} + WHITE, label);
        } else if (focused) {
            content += colored(WHITE, label);
        } else {
            content += colored(MUTED, label);
        }

        // Right-aligned status: active / standby / —
        std::string status;
        if (!supported) {
            status = colored(MUTED_DARK, "—");
        } else if (active) {
            status = colored(GREEN, "active");
        } else {
            status = colored(MUTED, "standby");
        }

        const std::size_t taken = visible_width(content) + STATUS_W + 2;
        const std::size_t middle = (taken < inner_width) ? inner_width - taken : 0;
        content += std::string(middle, ' ');
        content += pad_left(status, STATUS_W);
        content += "  ";
        rows.push_back(box_row(content, inner_width, focused));
    }

    rows.push_back(box_blank_row(inner_width, focused));

    // Conversation Awareness (toggle, row index 4).
    {
        const std::size_t i = MODES.size();
        const bool selected = focused && (i == selected_row);
        const bool supported = dev.model_info.has_conversation_awareness;
        const auto toggle = dev.conversation_detect_enabled;

        std::string value;
        if (!supported) {
            value = colored(MUTED_DARK, "—");
        } else if (toggle && *toggle) {
            value = colored(GREEN, "on");
        } else if (toggle && !*toggle) {
            value = colored(MUTED, "off");
        } else {
            value = colored(MUTED, "—");
        }
        const std::size_t value_visible = visible_width(value);

        rows.push_back(box_row(
            label_value_row(
                selected, focused, !supported,
                "Conversation Awareness",
                value, value_visible, inner_width
            ),
            inner_width, focused
        ));

        // Runtime hint when AACP fires "ConversationalAwareness" status.
        if (supported && dev.conversation_awareness && *dev.conversation_awareness != 0) {
            std::string hint;
            hint += "    ";
            hint += colored(AMBER, std::string_view{DOT_FILLED});
            hint += " ";
            hint += colored(AMBER, "active — auto-lowering volume");
            rows.push_back(box_row(hint, inner_width, focused));
        }
    }

    rows.push_back(box_blank_row(inner_width, focused));
    rows.push_back(box_bottom(inner_width, focused));
    return rows;
}

// ───────────────────── SETTINGS ─────────────────────

const char* press_speed_label(uint8_t value) {
    // The Apple W1/H2 chips encode press intervals as ordinal indices.
    // 0 ≈ default, 1 ≈ slow, 2 ≈ slowest. Anything else → fall back to byte.
    switch (value) {
    case 0: return "default";
    case 1: return "slow";
    case 2: return "slowest";
    default: return nullptr;
    }
}

const char* mic_mode_label(uint8_t value) {
    switch (value) {
    case 0x00: return "automatic";
    case 0x01: return "always left";
    case 0x02: return "always right";
    default: return nullptr;
    }
}

std::string toggle_value(std::optional<bool> v, bool focused) {
    if (!v) return colored(MUTED, "—");
    if (*v) {
        return focused
            ? colored(std::string{BOLD} + WHITE, "on")
            : colored(WHITE, "on");
    }
    return colored(MUTED, "off");
}

std::string enum_value(std::optional<uint8_t> v, const char* (*lookup)(uint8_t), bool focused) {
    if (!v) return colored(MUTED, "—");
    if (const auto* lbl = lookup(*v)) {
        return focused ? colored(WHITE, lbl) : colored(MUTED, lbl);
    }
    std::ostringstream out;
    out << "0x" << std::hex << static_cast<int>(*v);
    return colored(MUTED, out.str());
}

std::vector<std::string> render_settings(
    const TuiState& state,
    std::size_t inner_width,
    bool focused
) {
    const auto& dev = state.device();
    const std::size_t selected_row = state.selected_row(Section::Settings);
    std::vector<std::string> rows;

    rows.push_back(box_top(inner_width, "SETTINGS", focused));
    rows.push_back(box_blank_row(inner_width, focused));

    auto push_simple_row = [&](
        std::size_t i,
        const char* label,
        const std::string& value,
        bool unsupported
    ) {
        const bool selected = focused && (i == selected_row);
        const auto value_visible = visible_width(value);
        rows.push_back(box_row(
            label_value_row(
                selected, focused, unsupported,
                label, value, value_visible, inner_width
            ),
            inner_width, focused
        ));
    };

    // 0. NC with One AirPod
    push_simple_row(0, "NC with One AirPod",
        toggle_value(dev.nc_one_bud, focused),
        !dev.model_info.has_noise_cancellation
    );

    // 1. Personalized Volume
    push_simple_row(1, "Personalized Volume",
        toggle_value(dev.personalized_volume, focused),
        false
    );

    // 2. Volume Swipe
    push_simple_row(2, "Volume Swipe",
        toggle_value(dev.volume_swipe, focused),
        !dev.model_info.has_stem_tap_gestures
    );

    // 3. Press Speed
    push_simple_row(3, "Press Speed",
        enum_value(dev.press_speed, press_speed_label, focused),
        !dev.model_info.has_stem_tap_gestures
    );

    // 4. Press & Hold
    push_simple_row(4, "Press & Hold",
        enum_value(dev.press_hold, press_speed_label, focused),
        !dev.model_info.has_stem_tap_gestures
    );

    // 5. Tone Volume — bar + percentage.
    {
        const std::size_t i = 5;
        const bool selected = focused && (i == selected_row);
        const auto v = dev.tone_volume;
        // The chip reports 0..3 (mute/low/med/high); render that as a 4-step bar.
        std::optional<uint8_t> pct;
        if (v) pct = static_cast<uint8_t>(std::min<int>(*v, 3) * 100 / 3);

        constexpr std::size_t BAR_W = 18;
        std::string value;
        if (!v) {
            value = colored(MUTED_DARK, repeat(BAR_EMPTY, BAR_W)) + std::string{" "} + colored(MUTED, "—");
        } else {
            value = battery_bar(pct, BAR_W);
            std::ostringstream pct_str;
            pct_str << " " << std::setw(3) << static_cast<int>(*pct) << "%";
            value += colored(WHITE, pct_str.str());
        }
        const auto vw = visible_width(value);
        rows.push_back(box_row(
            label_value_row(
                selected, focused, false,
                "Tone Volume",
                value, vw, inner_width
            ),
            inner_width, focused
        ));
    }

    // 6. Volume Swipe Length
    push_simple_row(6, "Volume Swipe Length",
        enum_value(dev.volume_swipe_length, press_speed_label, focused),
        !dev.model_info.has_stem_tap_gestures
    );

    // 7. Mic Mode
    push_simple_row(7, "Mic Mode",
        enum_value(dev.mic_mode, mic_mode_label, focused),
        false
    );

    // 8. Auto Connect
    push_simple_row(8, "Auto Connect",
        toggle_value(dev.auto_connect, focused),
        false
    );

    rows.push_back(box_blank_row(inner_width, focused));
    rows.push_back(box_bottom(inner_width, focused));
    return rows;
}

// ───────────────────── FOOTER / KEYS ─────────────────────

std::vector<std::string> render_footer(std::size_t inner_width) {
    std::vector<std::string> rows;
    rows.push_back(box_top(inner_width, "KEYS", false));

    auto pair = [](const char* key, const char* desc) {
        return colored(WHITE, std::string_view{key})
             + std::string{" "}
             + colored(MUTED, std::string_view{desc});
    };

    std::string line = "  "
        + pair("q", "quit") + "    "
        + pair("tab", "section") + "    "
        + pair("\xE2\x86\x91\xE2\x86\x93", "navigate") + "    "
        + pair("\xE2\x86\x90\xE2\x86\x92", "adjust") + "    "
        + pair("r", "rename") + "    "
        + pair("i", "info");
    rows.push_back(box_row(line, inner_width, false));
    rows.push_back(box_bottom(inner_width, false));
    return rows;
}

// ───────────────────── Composer / fallbacks ─────────────────────

std::string move_cursor_home() { return "\x1b[H"; }
std::string clear_screen() { return "\x1b[2J"; }

std::string too_small_screen(uint16_t cols, uint16_t rows) {
    std::ostringstream out;
    out << clear_screen() << move_cursor_home();
    out << colored(std::string{BOLD} + WHITE, "open-pods") << "\r\n";
    out << colored(MUTED, "Terminal too small.") << "\r\n";
    std::ostringstream sz;
    sz << "current: " << cols << "x" << rows
       << "   minimum: " << MIN_COLS << "x" << MIN_ROWS;
    out << colored(MUTED, sz.str()) << "\r\n";
    return out.str();
}

std::vector<std::string> stack(
    std::vector<std::string> a,
    std::vector<std::string> b
) {
    a.reserve(a.size() + b.size());
    for (auto& line : b) a.push_back(std::move(line));
    return a;
}

std::vector<std::string> side_by_side(
    const std::vector<std::string>& left,
    std::size_t left_width,
    const std::vector<std::string>& right,
    std::size_t right_width,
    std::size_t gap
) {
    std::vector<std::string> rows;
    const auto row_count = std::max(left.size(), right.size());
    rows.reserve(row_count);

    for (std::size_t i = 0; i < row_count; ++i) {
        std::string row;
        if (i < left.size()) {
            row += pad_right(left[i], left_width);
        } else {
            row.append(left_width, ' ');
        }
        row.append(gap, ' ');
        if (i < right.size()) {
            row += pad_right(right[i], right_width);
        } else {
            row.append(right_width, ' ');
        }
        rows.push_back(std::move(row));
    }

    return rows;
}

void pad_box_to_height(
    std::vector<std::string>& box,
    std::size_t inner_width,
    bool focused,
    std::size_t target_rows
) {
    if (box.empty() || box.size() >= target_rows) return;

    auto bottom = std::move(box.back());
    box.pop_back();
    while (box.size() + 1 < target_rows) {
        box.push_back(box_blank_row(inner_width, focused));
    }
    box.push_back(std::move(bottom));
}

} // namespace

std::string render_frame(
    const TuiState& state,
    TerminalSize size
) {
    if (size.cols < MIN_COLS || size.rows < MIN_ROWS) {
        return too_small_screen(size.cols, size.rows);
    }

    const std::size_t total_outer = size.cols;
    const std::size_t total_inner = total_outer >= 2 ? total_outer - 2 : 0;
    const auto active = state.selected_section();
    constexpr std::size_t SIDE_BY_SIDE_MIN_COLS = 80;
    constexpr std::size_t SIDE_BY_SIDE_GAP = 2;

    auto header = render_header(state, total_inner);
    auto settings = render_settings(state, total_inner, active == Section::Settings);
    auto footer = render_footer(total_inner);

    std::vector<std::string> all = std::move(header);
    if (total_outer >= SIDE_BY_SIDE_MIN_COLS) {
        const auto panel_width = total_outer - SIDE_BY_SIDE_GAP;
        const auto battery_outer = panel_width / 2;
        const auto noise_outer = panel_width - battery_outer;
        auto battery = render_battery(state, battery_outer - 2, active == Section::Battery);
        auto noise = render_noise(state, noise_outer - 2, active == Section::NoiseControl);
        const auto panel_rows = std::max(battery.size(), noise.size());
        pad_box_to_height(battery, battery_outer - 2, active == Section::Battery, panel_rows);
        pad_box_to_height(noise, noise_outer - 2, active == Section::NoiseControl, panel_rows);
        all = stack(std::move(all), side_by_side(
            battery, battery_outer,
            noise, noise_outer,
            SIDE_BY_SIDE_GAP
        ));
    } else {
        auto battery = render_battery(state, total_inner, active == Section::Battery);
        auto noise = render_noise(state, total_inner, active == Section::NoiseControl);
        all = stack(std::move(all), std::move(battery));
        all = stack(std::move(all), std::move(noise));
    }
    all = stack(std::move(all), std::move(settings));
    all = stack(std::move(all), std::move(footer));

    std::ostringstream frame;
    frame << move_cursor_home();
    frame << "\x1b[J"; // clear from cursor down
    for (std::size_t i = 0; i < all.size() && i < size.rows; ++i) {
        frame << all[i] << "\r\n";
    }
    return frame.str();
}

} // namespace tui
