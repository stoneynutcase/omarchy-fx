#include "globals.hpp"
#include "WobblyManager.hpp"

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/version.h>

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // _get_hash() resolves to Hyprland's own symbol; _get_client_hash() is
    // compiled into this plugin from the headers it was built against. Both are
    // commit hash plus the versions of every shared Hypr* library, so this
    // catches an ABI drift the commit alone would not.
    const std::string SERVER = __hyprland_api_get_hash();
    const std::string CLIENT = __hyprland_api_get_client_hash();

    if (SERVER != CLIENT) {
        HyprlandAPI::addNotification(PHANDLE, "[omarchy-fx] Version mismatch, refusing to load. Rebuild the plugin against this Hyprland.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 6000);
        Log::logger->log(Log::ERR, "[omarchy-fx] version mismatch: server '{}' vs plugin '{}'", SERVER, CLIENT);
        throw std::runtime_error("[omarchy-fx] version mismatch");
    }

    OmarchyFX::g_wobbly = makeUnique<OmarchyFX::CWobblyManager>();
    OmarchyFX::g_wobbly->registerConfig();
    OmarchyFX::g_wobbly->init();

    Log::logger->log(Log::INFO, "[omarchy-fx] loaded, wobbly windows armed");

    return {"omarchy-fx", "Window effects for Omarchy: KDE-style wobbly windows while dragging", "omarchy-fx", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (OmarchyFX::g_wobbly) {
        OmarchyFX::g_wobbly->shutdown();
        OmarchyFX::g_wobbly.reset();
    }
}
