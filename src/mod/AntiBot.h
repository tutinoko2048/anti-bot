#pragma once

#include "ll/api/mod/NativeMod.h"

#include <string>

namespace anti_bot {

struct Config {
    int  version            = 1;
    bool kick               = true; // false: only log what would be kicked
    bool kickOnEarlyChat    = true; // chat/command before SetLocalPlayerAsInitialized
    bool kickOnSpoofedOrigin = true; // command whose origin type is not Player ("External")
    bool kickOnInitTimeout  = true; // logged in but never finished joining
    int  initTimeoutSeconds = 90;
    bool rejectKickedOnRejoin = true; // remember kicked XUID/name for this session
    std::string kickMessage = "Disconnected";
};

class AntiBot {
public:
    static AntiBot& getInstance();

    AntiBot() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }
    [[nodiscard]] Config const&       config() const { return mConfig; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod& mSelf;
    Config              mConfig;
};

} // namespace anti_bot
