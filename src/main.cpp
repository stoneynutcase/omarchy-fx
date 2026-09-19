#include "globals.hpp"
#include "EffectManager.hpp"

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/version.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// ---- crash guard -----------------------------------------------------------
//
// A plugin runs inside the compositor, and the config loads it at startup. So
// a plugin that crashes right after loading does not crash once: it crashes
// every login, and the only way out is a TTY. The stale-ABI case is caught by
// the hash check below, but a plain bug in this code would not be.
//
// The guard is a marker file, written when the plugin starts and removed once
// it has run for a while, or on a clean unload. Finding it already there at
// startup means the last session ended within seconds of loading the plugin —
// a crash, or a hard reset, which gets the same treatment — and this session
// is skipped, with a notification saying so. The marker is cleared at the same
// time, so the following login tries again: a real crash loop becomes every
// other login working, with an explanation on screen, instead of no login.
namespace {
    constexpr auto SETTLE_AFTER = std::chrono::seconds(10);

    std::string guardPath() {
        const auto STATE = getenv("XDG_STATE_HOME");
        const auto HOME  = getenv("HOME");
        std::string base = STATE && *STATE ? STATE : std::string{HOME ? HOME : ""} + "/.local/state";
        return base + "/omarchy-fx/loading";
    }

    bool guardTripped() {
        std::error_code ec;
        return std::filesystem::exists(guardPath(), ec);
    }

    void guardArm() {
        std::error_code ec;
        const auto      PATH = guardPath();
        std::filesystem::create_directories(std::filesystem::path{PATH}.parent_path(), ec);
        std::filesystem::permissions(std::filesystem::path{PATH}.parent_path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
        std::ofstream{PATH} << "omarchy-fx is loading; removed once it has run for a while\n";
    }

    void guardClear() {
        std::error_code ec;
        std::filesystem::remove(guardPath(), ec);
    }

    CHyprSignalListener                   g_settleListener;
    std::chrono::steady_clock::time_point g_loadedAt;
    bool                                  g_settled = false;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // _get_hash() resolves to Hyprland's own symbol; _get_client_hash() is
    // compiled into this plugin from the headers it was built against. Both are
    // commit hash plus the versions of every shared Hypr* library, so this
    // catches an ABI drift the commit alone would not. Hyprland's own check
    // before this point compares only a coarse API version string.
    const std::string SERVER = __hyprland_api_get_hash();
    const std::string CLIENT = __hyprland_api_get_client_hash();

    if (SERVER != CLIENT) {
        HyprlandAPI::addNotification(PHANDLE, "[omarchy-fx] Version mismatch, refusing to load. Rebuild the plugin against this Hyprland.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 6000);
        Log::logger->log(Log::ERR, "[omarchy-fx] version mismatch: server '{}' vs plugin '{}'", SERVER, CLIENT);
        throw std::runtime_error("[omarchy-fx] version mismatch");
    }

    if (guardTripped()) {
        guardClear();
        HyprlandAPI::addNotification(PHANDLE,
                                     "[omarchy-fx] Skipped this session: the last one ended within seconds of loading the plugin. It loads again at the next "
                                     "login. If that crashes too, remove require(\"hypr.omarchy_fx\") from ~/.config/hypr/hyprland.lua.",
                                     CHyprColor{1.0, 0.6, 0.1, 1.0}, 20000);
        throw std::runtime_error("[omarchy-fx] skipped: the previous session ended right after loading");
    }

    guardArm();
    g_loadedAt = std::chrono::steady_clock::now();
    g_settled  = false;

    OmarchyFX::g_fx = makeUnique<OmarchyFX::CEffectManager>();
    OmarchyFX::g_fx->registerConfig();
    OmarchyFX::g_fx->init();

    // Once frames have been rendering for a while the load is a success and
    // the marker goes. The listener stays; it is one bool check per frame.
    g_settleListener = Event::bus()->m_events.render.preChecks.listen([](const PHLMONITOR&) {
        if (g_settled)
            return;
        if (std::chrono::steady_clock::now() - g_loadedAt >= SETTLE_AFTER) {
            g_settled = true;
            guardClear();
        }
    });

    Log::logger->log(Log::INFO, "[omarchy-fx] loaded, window effects armed");

    return {"omarchy-fx", "Window effects for Omarchy: wobbly dragging, elastic tiling moves, a focus pulse, and shake-to-find for the cursor", "omarchy-fx", "0.4.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_settleListener.reset();

    if (OmarchyFX::g_fx) {
        OmarchyFX::g_fx->shutdown();
        OmarchyFX::g_fx.reset();
    }

    // A clean unload is not a crash.
    guardClear();
}
