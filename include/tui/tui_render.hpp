#pragma once

#include <cstdint>
#include <string>

#include "tui/tui_state.hpp"
#include "tui/tui_term.hpp"

namespace tui {

/**
 * @brief Builds the full ANSI-encoded frame for the given state and terminal
 *        size. Returns a single string ready to be written to stdout.
 *
 * Layout philosophy:
 *  - The grid is column-aware: when the terminal is wider than 100 columns the
 *    POD MATRIX and AUDIO CORE panels sit side-by-side; otherwise they stack.
 *  - Every panel computes its inner width from `cols`, so resizing never
 *    breaks borders or smears text.
 *  - Panels whose feature set is unsupported by the connected model are
 *    dropped entirely (e.g. AUDIO CORE for AirPods 1st gen).
 *  - When the terminal is too small (< 60 cols or < 16 rows) a banner shows
 *    instead of a broken layout.
 */
[[nodiscard]] std::string render_frame(
    const TuiState& state,
    TerminalSize size
);

} // namespace tui
