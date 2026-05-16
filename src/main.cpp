#define WLR_USE_UNSTABLE

#include "globals.hpp"
#include <hyprlang.hpp>
#include <unistd.h>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/Compositor.hpp>

#include "globals.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    Global::PHANDLE               = handle;
    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        const CHyprColor errorColor(1.0f, 0.0f, 0.0f, 1.0f);
        HyprlandAPI::addNotification(Global::PHANDLE,
                                     std::format("[{}] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver).", Global::pluginName),
                                     errorColor, 5000);
        throw std::runtime_error(std::format("[{}] Version mismatch", Global::pluginNameShort));
    }

    // Register event listeners using Event::bus()
    // Listen for the ready event to know when Hyprland is fully initialized
    static auto ready = Event::bus()->m_events.ready.listen([]() { Global::hyprlandReady = true; });

    // If there are already monitors (meaning we're past initialization), mark as ready immediately
    // This handles the case where the plugin is loaded dynamically after Hyprland has started
    if (g_pCompositor && !g_pCompositor->m_monitors.empty()) {
        Global::hyprlandReady = true;
    }
    const CHyprColor goodColor(0.0f, 1.0f, 0.0f, 1.0f);
    HyprlandAPI::addNotification(Global::PHANDLE, std::format("[{}] initialized.", Global::pluginName), goodColor, 5000);

    HyprlandAPI::reloadConfig();

    return {Global::pluginName, Global::description, Global::author, Global::version};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // ...
}
