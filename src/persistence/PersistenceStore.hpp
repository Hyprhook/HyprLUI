#pragma once
//
// PersistenceStore.hpp
//
// Phase 11 (DESIGN.md): hyprlui.persistent(key, default) - a native C++
// key-value store that survives Lua config reloads, for state a user
// explicitly wants to KEEP across a reload (as opposed to declaratively-
// recreated UI, which is fine to lose - see the "General plugin-lifecycle
// bug" note earlier in DESIGN.md).
//
// Why this needs to be a REAL native re-encoding, not just a kept Lua
// reference: verified against Hyprland's own source before implementing
// (same practice as every internal-API-reliant phase) that
// CConfigManager::reload() unconditionally calls reinitLuaState() on
// EVERY reload, which does `lua_close(m_lua)` then
// `m_lua = luaL_newstate()` - the entire Lua interpreter, including its
// registry, is destroyed and a fresh one created each time. There is no
// "the persistent lua_State" to hold a LUA_REGISTRYINDEX ref into across
// a reload boundary - by the time the fresh script runs, the old
// interpreter (and everything that was only ever a reference INTO it) is
// gone. (This also means an older doc comment in Watcher.hpp claiming
// Hyprland's Lua config "runs in a single persistent lua_State for the
// compositor's whole lifetime" was factually wrong - harmless in
// practice only because CWatcherManager clears every Lua ref it holds on
// config.preReload, which fires before the old state is destroyed, so
// nothing there ever dereferences a dangling one - see that file's
// corrected comment.)
//
// Scope, decided up front (asked explicitly, not assumed): survives a
// config reload (the plugin process keeps running) but NOT a full plugin
// unload or Hyprland restart - a pure in-memory store, no disk I/O.
// Values are scalars only (number/string/boolean) - covers the realistic
// "a volume level, a theme name, a toggle" use case without needing
// recursive table encode/decode.

#include <string>
#include <unordered_map>
#include <variant>

namespace HyprLUI {

    // number is always stored as double (Lua itself has no separate
    // int/float distinction visible here) - string and bool as their own
    // alternatives. Order matters: index() is used as a cheap type tag
    // for the mismatch check below, so don't reorder these without
    // checking every std::get<>/holds_alternative call site.
    using PersistentValue = std::variant<double, std::string, bool>;

    class CPersistenceStore {
      public:
        static CPersistenceStore& get();

        // Used by hyprlui.persistent(key, default) itself: if `key` isn't
        // stored yet, seeds it with `def` and returns `def`. If it's
        // already stored, returns the EXISTING value regardless of `def`
        // - `def` is only ever consulted the very first time nothing was
        // stored yet, never enforced afterwards. If the existing value's
        // type differs from `def`'s type, logs a warning (Log::WARN, not
        // luaL_error - permissive, per the explicit call on this) but
        // still returns the existing value unchanged either way.
        PersistentValue getOrInit(const std::string& key, const PersistentValue& def);

        // Used by the wrapper table's :get() method - a plain lookup, no
        // default/warning logic (that already happened at the
        // getOrInit() call the owning persistent() call made). Returns
        // `false` (an arbitrary but harmless placeholder) if `key`
        // somehow isn't present - shouldn't happen in practice, since
        // persistent() always seeds it first, but this avoids needing an
        // optional/error path for a case that's already structurally
        // prevented.
        PersistentValue getRaw(const std::string& key) const;

        // Used by the wrapper table's :set(value) method - overwrites
        // unconditionally, including changing the value's type freely
        // (only getOrInit()'s own re-seed-attempt path warns on a type
        // mismatch, never an explicit set() - the caller asking to change
        // the value, possibly to a new type, is assumed deliberate).
        void set(const std::string& key, const PersistentValue& value);

        // Clears every stored value - call ONLY from PLUGIN_EXIT (a real
        // unload), deliberately NEVER from config.preReload's
        // resetAllState() the way every other manager in this codebase
        // is - surviving exactly that reload is this whole class's
        // reason to exist. Do not "fix" this into resetAllState() to
        // match the other managers; that would defeat the feature.
        void clear();

      private:
        CPersistenceStore()  = default;
        ~CPersistenceStore() = default;

        std::unordered_map<std::string, PersistentValue> m_values;
    };

} // namespace HyprLUI
