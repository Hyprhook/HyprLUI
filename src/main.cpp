// WLR_USE_UNSTABLE is already defined via -D in the Makefile/meson.build.
#include "globals.hpp"
#include <unistd.h>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>

#include "render/Render.hpp"
#include "ui/UIManager.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

namespace {

    // Toggles a small demo canvas so you can verify the pipeline works
    // end-to-end before wiring anything else up. Invoke via:
    //   hyprctl eval 'hl.plugin.hyprlui.demo()'
    // or bind a key to it from a Lua config with:
    //   hl.bind("SUPER + D", function() hl.plugin.hyprlui.demo() end)
    void toggleDemoCanvas() {
        auto& mgr = HyprLUI::CUIManager::get();

        if (mgr.hasCanvas("hyprlui_demo")) {
            mgr.removeCanvas("hyprlui_demo");
            return;
        }

        mgr.createCanvas("hyprlui_demo", {100, 100}, {440, 120});

        // Node draw order within a canvas is creation order, so add the
        // background panel first, then the text that sits on top of it.
        mgr.addRect("hyprlui_demo", "panel", {0, 0}, {440, 120}, CHyprColor{0.05, 0.05, 0.05, 0.75}, 8);
        mgr.addText("hyprlui_demo", "title", {20, 16}, "Hello from HyprLUI!", 24, CHyprColor{1.0, 1.0, 1.0, 1.0});
        mgr.addText("hyprlui_demo", "subtitle", {20, 60}, "Rendered from C++, ready for Lua.", 14, CHyprColor{0.8, 0.8, 0.8, 1.0});
    }

    // Lua-callable entry point registered below as hl.plugin.hyprlui.demo.
    // `lua_State*` is forward-declared only (see PluginAPI.hpp) - fine here
    // since this callback takes no Lua args and returns none (0 = no values
    // pushed back onto the Lua stack). If a future binding needs to read
    // arguments or push a return value, it'll need the real Lua headers
    // (lua.h/lauxlib.h) and a matching pkg-config dependency added to the
    // build - see PLUGIN_LUA_FN in PluginAPI.hpp.
    int luaToggleDemo(lua_State*) {
        toggleDemoCanvas();
        return 0;
    }

} // namespace

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
    if (g_pCompositor && !State::monitorState()->monitors().empty()) {
        Global::hyprlandReady = true;
    }

    // addConfigValue is deprecated *and* stubbed out (always returns false)
    // on this Hyprland dev snapshot - addConfigValueV2, taking a
    // Config::Values::IValue, is the supported replacement.
    HyprlandAPI::addConfigValueV2(Global::PHANDLE, makeShared<Config::Values::CBoolValue>("plugin:hyprlui:enabled", "Whether HyprLUI is active.", true));

    HyprLUI::RenderHook::registerHooks(Global::PHANDLE);

    // NOTE: addDispatcher/addDispatcherV2 are stubbed out (always return
    // false) on this Hyprland dev snapshot - the old string-keyed dispatcher
    // table plugins used to hook into no longer exists now that config is
    // Lua-based. The supported way to expose a plugin action is
    // addLuaFunction, which registers a C function under
    // hl.plugin.<namespace>.<name> in the Lua config's global state.
    HyprlandAPI::addLuaFunction(Global::PHANDLE, "hyprlui", "demo", &luaToggleDemo);

    const CHyprColor goodColor(0.0f, 1.0f, 0.0f, 1.0f);
    HyprlandAPI::addNotification(Global::PHANDLE, std::format("[{}] initialized.", Global::pluginName), goodColor, 5000);

    HyprlandAPI::reloadConfig();

    return {Global::pluginName, Global::description, Global::author, Global::version};
}

APICALL EXPORT void PLUGIN_EXIT() {
    HyprLUI::CUIManager::get().clear();
    HyprLUI::RenderHook::unregisterHooks(Global::PHANDLE);
    HyprlandAPI::removeLuaFunction(Global::PHANDLE, "hyprlui", "demo");
}
