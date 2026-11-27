// Anti Bot: drops and kicks clients that chat or run commands before finishing the join.
//
// No hooks of its own: everything goes through LeviLamina's maintained events, so a game
// update only needs a rebuild against the matching LeviLamina, not new hook signatures.
//   PlayerConnectEvent    - login (start of the join timeout)
//   PlayerJoinEvent       - SetLocalPlayerAsInitialized (join finished)
//   PlayerChatEvent       - TextPacket from a player (cancellable)
//   ExecutingCommandEvent - any command about to run (cancellable)
//   PlayerDisconnectEvent - cleanup
//   ServerLevelTickEvent  - timeouts and deferred kicks
// The only direct game call is ServerNetworkHandler::disconnectClientWithMessage.

#include "mod/AntiBot.h"

#include "ll/api/Config.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ExecuteCommandEvent.h"
#include "ll/api/event/player/PlayerChatEvent.h"
#include "ll/api/event/player/PlayerConnectEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include "mc/common/SubClientId.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/connection/DisconnectFailReason.h"
#include "mc/server/ServerPlayer.h"
#include "mc/server/commands/CommandContext.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOriginType.h"
#include "mc/world/actor/player/Player.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace anti_bot {

namespace {

using Clock = std::chrono::steady_clock;

struct Session {
    std::string       name;
    std::string       xuid;
    NetworkIdentifier netId;
    SubClientId       subId;
    Clock::time_point since;
    bool              initialized = false;
    bool              kickQueued  = false;
};

struct KickRequest {
    NetworkIdentifier netId;
    SubClientId       subId;
};

std::mutex                               gMutex;
std::unordered_map<std::string, Session> gSessions; // key: player UUID
std::unordered_set<std::string>          gRejected; // kicked this session (xuid or name)
std::vector<KickRequest>                 gKickQueue;
std::vector<ll::event::ListenerPtr>      gListeners;

ll::io::Logger& logger() { return AntiBot::getInstance().getSelf().getLogger(); }
Config const&   cfg() { return AntiBot::getInstance().config(); }

std::string keyOf(Player const& player) { return player.getUuid().asString(); }
std::string rejectKey(std::string const& name, std::string const& xuid) {
    return xuid.empty() ? "name:" + name : "xuid:" + xuid;
}
std::string describe(Session const& s) { return s.name + " (xuid " + (s.xuid.empty() ? "?" : s.xuid) + ")"; }

// Caller holds gMutex. Never disconnects directly: the next server tick does.
void queueKickLocked(Session& session, std::string_view reason, bool ruleEnabled) {
    if (!cfg().kick || !ruleEnabled) {
        logger().warn("[dry-run] would kick {}: {}", describe(session), reason);
        return;
    }
    if (session.kickQueued) return;
    session.kickQueued = true;
    if (cfg().rejectKickedOnRejoin) gRejected.insert(rejectKey(session.name, session.xuid));
    logger().warn("Kicking {}: {}", describe(session), reason);
    gKickQueue.push_back({session.netId, session.subId});
}

// Returns true when the player has not finished joining (and queues a kick).
bool flagIfNotJoined(Player const& player, std::string_view what) {
    std::lock_guard lock{gMutex};
    auto            it = gSessions.find(keyOf(player));
    if (it == gSessions.end() || it->second.initialized) return false;
    queueKickLocked(it->second, std::string{what} + " before finishing join", cfg().kickOnEarlyChat);
    return true;
}

bool isClientSentOrigin(CommandOriginType type) {
    switch (type) {
    case CommandOriginType::Player:
    case CommandOriginType::DevConsole:
    case CommandOriginType::AutomationPlayer:
    case CommandOriginType::ClientAutomation:
        return true;
    default:
        return false; // scripts, command blocks, server console etc. are never flagged
    }
}

// The world's host is never a bot. A host with a websocket (/connect) legitimately sends
// AutomationPlayer commands such as getlocalplayername the moment they enter the world.
bool isHost(Player const& player) { return player.mIsHostingPlayer; }

void onConnect(ll::event::PlayerConnectEvent& event) {
    auto& player = event.self();
    if (isHost(player)) return; // no session: none of the rules apply to the host
    Session session{
        player.getRealName(),
        player.getXuid(),
        event.networkIdentifier(),
        player.getClientSubId(),
        Clock::now(),
    };
    logger().info("Login: {}", describe(session));
    std::lock_guard lock{gMutex};
    auto& stored = gSessions.insert_or_assign(keyOf(player), std::move(session)).first->second;
    if (cfg().rejectKickedOnRejoin && gRejected.contains(rejectKey(stored.name, stored.xuid))) {
        queueKickLocked(stored, "previously kicked this session", true);
    }
}

void onJoin(ll::event::PlayerJoinEvent& event) {
    std::lock_guard lock{gMutex};
    if (auto it = gSessions.find(keyOf(event.self())); it != gSessions.end()) it->second.initialized = true;
}

void onChat(ll::event::PlayerChatEvent& event) {
    if (flagIfNotJoined(event.self(), "chat")) event.cancel();
}

void onCommand(ll::event::ExecutingCommandEvent& event) {
    auto const& origin = event.commandContext().mOrigin;
    if (!origin) return;
    auto const type = origin->getOriginType();
    if (!isClientSentOrigin(type)) return;
    auto* entity = origin->getEntity();
    if (!entity || !entity->isPlayer()) return;
    auto& player = static_cast<Player&>(*entity);
    if (isHost(player)) return;

    if (flagIfNotJoined(player, "command: " + event.commandContext().mCommand)) {
        event.cancel();
        return;
    }
    if (type != CommandOriginType::Player) {
        std::lock_guard lock{gMutex};
        if (auto it = gSessions.find(keyOf(player)); it != gSessions.end()) {
            queueKickLocked(
                it->second,
                "command with origin type " + std::to_string(static_cast<int>(type)) + ": "
                    + event.commandContext().mCommand,
                cfg().kickOnSpoofedOrigin
            );
        }
        event.cancel();
    }
}

void onDisconnect(ll::event::PlayerDisconnectEvent& event) {
    std::lock_guard lock{gMutex};
    gSessions.erase(keyOf(event.self()));
}

void onTick(ll::event::ServerLevelTickEvent&) {
    std::vector<KickRequest> todo;
    {
        std::lock_guard lock{gMutex};
        auto const      now     = Clock::now();
        auto const      timeout = std::chrono::seconds(cfg().initTimeoutSeconds);
        for (auto& [key, session] : gSessions) {
            if (!session.initialized && !session.kickQueued && now - session.since > timeout) {
                queueKickLocked(session, "did not finish joining in time", cfg().kickOnInitTimeout);
                session.since = now; // dry-run: log once per timeout period, not every tick
            }
        }
        todo.swap(gKickQueue);
    }
    if (todo.empty()) return;
    auto handler = ll::service::getServerNetworkHandler();
    if (!handler) return;
    for (auto& request : todo) {
        handler->disconnectClientWithMessage(
            request.netId,
            request.subId,
            Connection::DisconnectFailReason::Kicked,
            cfg().kickMessage,
            std::nullopt
        );
    }
}

} // namespace

AntiBot& AntiBot::getInstance() {
    static AntiBot instance;
    return instance;
}

bool AntiBot::load() {
    auto path = getSelf().getConfigDir() / "config.json";
    try {
        if (!ll::config::loadConfig(mConfig, path)) ll::config::saveConfig(mConfig, path);
    } catch (std::exception const& e) {
        getSelf().getLogger().error("Config load failed, using defaults: {}", e.what());
        mConfig = {};
    }
    if (mConfig.initTimeoutSeconds < 10) mConfig.initTimeoutSeconds = 10;
    return true;
}

bool AntiBot::enable() {
    auto& bus = ll::event::EventBus::getInstance();
    gListeners.push_back(bus.emplaceListener<ll::event::PlayerConnectEvent>(onConnect));
    gListeners.push_back(bus.emplaceListener<ll::event::PlayerJoinEvent>(onJoin));
    gListeners.push_back(bus.emplaceListener<ll::event::PlayerChatEvent>(onChat, ll::event::EventPriority::High));
    gListeners.push_back(
        bus.emplaceListener<ll::event::ExecutingCommandEvent>(onCommand, ll::event::EventPriority::High)
    );
    gListeners.push_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>(onDisconnect));
    gListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>(onTick));
    getSelf().getLogger().info(
        "Enabled ({}; timeout {}s)",
        mConfig.kick ? "kicking" : "dry-run: logging only",
        mConfig.initTimeoutSeconds
    );
    return true;
}

bool AntiBot::disable() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : gListeners) bus.removeListener(listener);
    gListeners.clear();
    std::lock_guard lock{gMutex};
    gSessions.clear();
    gKickQueue.clear();
    return true;
}

} // namespace anti_bot

LL_REGISTER_MOD(anti_bot::AntiBot, anti_bot::AntiBot::getInstance());
