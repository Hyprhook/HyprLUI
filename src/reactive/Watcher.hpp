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
// HyprlandAPI:: timer surface. Internal API, not guaranteed stable across
// Hyprland releases - kept isolated to this file for that reason.
//
// CWatcherManager has no idea which widgets/canvases are bound to what -
// a value actually changing just prompts a blunt "damage every canvas",
// and each canvas re-reads whatever it's bound to fresh every frame.
// Simplest correct thing at HUD scale.

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

        // `L` is the Lua state to call `fnRef` on. Note: a config reload
        // fully destroys and recreates the Lua state - this class only
        // gets away with holding `L`/`fnRef` across one because clear()
        // runs on config.preReload, strictly before the old state is
        // destroyed, releasing every ref while it's still valid.
        // `fnRef` is a LUA_REGISTRYINDEX reference to the watcher function
        // - caller creates it, this class owns releasing it (see clear()).
        // `intervalMs`, if given, arms a repeating timer that also calls
        // notify() on that cadence. luaL_errors if `name` is already
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
