#pragma once
//
// Watcher.hpp
//
// Named watchers: a Lua function whose return value is cached and kept
// fresh either by an explicit hyprlui.notify(name) call or by an optional
// poll interval. hyprlui.Bind(name) (see LuaBridge.cpp) ties a widget
// property to a watcher's current value.
//
// Polling reaches into Hyprland's internal event-loop timer
// (CEventLoopTimer / g_pEventLoopManager) - there is no stable
// HyprlandAPI:: timer surface (checked: PluginAPI.hpp exposes nothing
// timer-shaped). This mirrors exactly what Hyprland's own hl.timer() Lua
// binding does internally (src/config/lua/bindings/LuaBindingsToplevel.cpp,
// hlTimer()) - same primitive, same re-arm-via-updateTimeout() pattern.
// Internal API, not guaranteed stable across Hyprland releases - kept
// isolated to this file for exactly that reason, same reasoning as
// src/render/gfx.cpp for the rendering internals.
//
// CWatcherManager has no idea which widgets/canvases are bound to what -
// a value actually changing just prompts a blunt "damage every canvas"
// (CUIManager::damageAll()), and each canvas re-reads whatever it's bound
// to fresh every frame (CCanvas::m_bindings, applied at the top of
// render()). Simplest correct thing at HUD scale - matches this project's
// established "redo it every frame rather than build fine-grained
// invalidation" precedent (see DESIGN.md).

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <optional>
#include <string>
#include <unordered_map>

class CEventLoopTimer;

namespace HyprLUI {

    class CWatcherManager {
      public:
        static CWatcherManager& get();

        // `L` is the Lua state to call `fnRef` on - Hyprland's Lua config
        // runs in a single persistent lua_State for the compositor's
        // whole lifetime (never per-call sandboxes), same assumption
        // hl.timer() itself makes. `fnRef` is a LUA_REGISTRYINDEX
        // reference (luaL_ref) to the watcher function - caller creates
        // it, this class owns releasing it (see clear()). `intervalMs`,
        // if given, arms a repeating timer that also calls notify() on
        // that cadence. luaL_errors (never returns) if `name` is already
        // registered.
        void registerWatcher(lua_State* L, const std::string& name, int fnRef, std::optional<int> intervalMs);

        // Re-invokes the watcher's function now and updates its cached
        // value; if the (stringified) value actually changed, damages
        // every canvas so the change actually gets repainted. Used by
        // both hyprlui.notify() and the poll timer. Watcher function
        // errors are caught and logged rather than propagated - this runs
        // from contexts (a timer fire, or notify() called from anywhere)
        // with no caller-side pcall of their own to catch a mistake in the
        // watcher function. Returns false if `name` isn't registered.
        bool notify(const std::string& name);

        // Current cached value, stringified the same way Lua's own
        // tostring()/`..` would coerce it (via luaL_tolstring). Returns
        // `def` if `name` isn't a registered watcher - never errors, since
        // this is called from CCanvas::render(), not from a Lua call.
        std::string currentValue(const std::string& name, const std::string& def = "") const;

        bool        hasWatcher(const std::string& name) const;

        // Cancels every poll timer and releases every Lua function
        // reference. Call from PLUGIN_EXIT.
        void clear();

      private:
        CWatcherManager() = default;
        // NOT `= default` here on purpose: SWatcher holds an SP<> to the
        // forward-declared CEventLoopTimer, so its destructor can only be
        // instantiated once that type is complete - defined out-of-line in
        // Watcher.cpp, after including EventLoopTimer.hpp. An inline
        // `= default` here would try to instantiate it against an
        // incomplete type and fail to compile.
        ~CWatcherManager();

        // `timer` holds a forward-declared CEventLoopTimer via SP<> (fine
        // as a member without a complete type - only actually constructing
        // or dereferencing one requires EventLoopTimer.hpp, and that only
        // happens in Watcher.cpp) so this header doesn't need to pull in
        // Hyprland's event-loop internals just to declare the map.
        struct SWatcher {
            lua_State*          L     = nullptr;
            int                 fnRef = -1;
            SP<CEventLoopTimer> timer;
            std::string         value;
        };

        std::unordered_map<std::string, SWatcher> m_watchers;
    };

} // namespace HyprLUI
