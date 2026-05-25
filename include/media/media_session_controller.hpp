// include/media/media_session_controller.h
#pragma once

#include <string>
#include <string_view>
#include <vector>

class MediaSessionController {
public:
    virtual ~MediaSessionController() = default;

    virtual bool any_player_playing() = 0;
    virtual std::vector<std::string> pause_playing_players() = 0;
    virtual void resume_players(const std::vector<std::string>& services) = 0;
    virtual void play_pause_first() = 0;
    virtual void next() = 0;
    virtual void previous() = 0;
};