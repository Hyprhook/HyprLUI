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
#include "ui/LuaBridge.hpp"
#include "ui/ContainerWidget.hpp"
#include "ui/RectNode.hpp"
#include "ui/TextNode.hpp"
#include "reactive/Watcher.hpp"
#include "input/InputHook.hpp"
#include "reserved/ReservedAreaComposer.hpp"

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
        using namespace HyprLUI;

        auto& mgr = CUIManager::get();

        if (mgr.hasCanvas("hyprlui_demo")) {
            mgr.removeCanvas("hyprlui_demo");
            return;
        }

        // A Stack as the root so the background panel (absolutely
        // positioned to fill it) can sit behind the text column, which is
        // itself a Column so the two lines stack with a gap between them -
        // exercises both container types the Phase 1 widget tree ships
        // with. This is exactly what hyprlui.window{...} builds from Lua;
        // see LuaBridge.hpp for the declarative equivalent.
        auto root = std::make_shared<CStackWidget>("root");
        root->addChild(std::make_shared<CRectNode>("panel", Vector2D{0, 0}, Vector2D{440, 120}, CHyprColor{0.05, 0.05, 0.05, 0.75}, 8));

        auto textColumn = std::make_shared<CFlexWidget>("text", Vector2D{20, 16}, EFlexDirection::Column, /* gap = */ 6);
        textColumn->addChild(std::make_shared<CTextNode>("title", Vector2D{0, 0}, "Hello from HyprLUI!", 24, CHyprColor{1.0, 1.0, 1.0, 1.0}));
        textColumn->addChild(std::make_shared<CTextNode>("subtitle", Vector2D{0, 0}, "Rendered from C++, ready for Lua.", 14, CHyprColor{0.8, 0.8, 0.8, 1.0}));
        root->addChild(textColumn);

        auto canvas = mgr.createCanvas("hyprlui_demo", {100, 100}, {440, 120});
        canvas->setRoot(root);
        canvas->damage();
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

    // Tears down every canvas/watcher/exclusive-zone contribution HyprLUI
    // currently owns. Used both at PLUGIN_EXIT (don't leave anything
    // behind after unload) and right before a config reload re-parses the
    // Lua script from scratch (see the config.preReload listener in
    // PLUGIN_INIT below).
    //
    // Why the reload case matters: the plugin itself is NOT unloaded/
    // reloaded when the Lua config reloads - only the script gets re-run.
    // So without this, HyprLUI's C++-side state would silently outlive
    // the Lua-side bookkeeping (local variables tracking "is this window
    // open", etc.) that's supposed to own it - confirmed live: toggling a
    // window open, then reloading (e.g. fixing an unrelated config eval
    // error, which forces exactly this), left the window open in C++
    // while the Lua toggle variable that tracked it reset to "closed" on
    // re-run - permanently orphaning it, no way to reference it again
    // through that keybind. Clearing everything right before the fresh
    // script runs means it starts from the same blank slate the Lua
    // script's own reset locals already assume, matching how Hyprland's
    // own config-driven state (binds, window rules) already behaves
    // across a reload.
    void resetAllState() {
        HyprLUI::CWatcherManager::get().clear();
        HyprLUI::CUIManager::get().clear();
        HyprLUI::CReservedAreaComposer::get().clear();
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
    HyprLUI::InputHook::registerHooks(Global::PHANDLE);
    HyprLUI::CReservedAreaComposer::get().registerHooks(Global::PHANDLE);

    // See resetAllState()'s doc comment - config.preReload fires as the
    // very first thing inside CConfigManager::reload(), strictly before
    // any re-parsing begins (ConfigManager.cpp:647-648), so clearing here
    // can never wipe out anything the fresh script is about to create.
    static auto preReload = Event::bus()->m_events.config.preReload.listen([]() { resetAllState(); });

    // NOTE: addDispatcher/addDispatcherV2 are stubbed out (always return
    // false) on this Hyprland dev snapshot - the old string-keyed dispatcher
    // table plugins used to hook into no longer exists now that config is
    // Lua-based. The supported way to expose a plugin action is
    // addLuaFunction, which registers a C function under
    // hl.plugin.<namespace>.<name> in the Lua config's global state.
    HyprlandAPI::addLuaFunction(Global::PHANDLE, "hyprlui", "demo", &luaToggleDemo);

    // The real public API: hl.plugin.hyprlui.create_canvas/add_rect/add_text/...
    // See src/ui/LuaBridge.hpp for the full list and hyprland.lua usage.
    HyprLUI::Lua::registerFunctions(Global::PHANDLE);

    const CHyprColor goodColor(0.0f, 1.0f, 0.0f, 1.0f);
    HyprlandAPI::addNotification(Global::PHANDLE, std::format("[{}] initialized.", Global::pluginName), goodColor, 5000);

    HyprlandAPI::reloadConfig();

    return {Global::pluginName, Global::description, Global::author, Global::version};
}

APICALL EXPORT void PLUGIN_EXIT() {
    resetAllState();
    HyprLUI::CReservedAreaComposer::get().unregisterHooks(Global::PHANDLE);
    HyprLUI::InputHook::unregisterHooks(Global::PHANDLE);
    HyprLUI::RenderHook::unregisterHooks(Global::PHANDLE);
    HyprlandAPI::removeLuaFunction(Global::PHANDLE, "hyprlui", "demo");
    HyprLUI::Lua::unregisterFunctions(Global::PHANDLE);
}
