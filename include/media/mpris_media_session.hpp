#pragma once

#include "media/media_session_controller.hpp"

#include <memory>
#include <mutex>

/**
 * @brief MPRIS-based MediaSessionController that talks to org.mpris.MediaPlayer2.* on the session bus.
 * @note Construction may fail to acquire a session bus connection; in that case all calls are no-ops.
 *       Calls are serialized with an internal mutex so the controller is safe to share across threads.
 */
class MprisMediaSessionController final : public MediaSessionController {
public:
    MprisMediaSessionController();
    ~MprisMediaSessionController() override;

    MprisMediaSessionController(const MprisMediaSessionController&) = delete;
    MprisMediaSessionController& operator=(const MprisMediaSessionController&) = delete;

    bool any_player_playing() override;
    std::vector<std::string> pause_playing_players() override;
    void resume_players(const std::vector<std::string>& services) override;
    void play_pause_first() override;
    void next() override;
    void previous() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;
};
