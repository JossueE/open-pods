#pragma once

namespace tui {

/**
 * @brief Connects to the open-pods daemon via IPC, opens an alt-screen TUI
 *        session and runs the input/event/render loop until the user quits or
 *        a SIGINT/SIGTERM is delivered.
 *
 * @return Process-style exit code.
 */
int run();

} // namespace tui
