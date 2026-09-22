#pragma once
//
// PersistenceStore.hpp
//
// hyprlui.persistent(key, default) - a native C++ key-value store that
// survives Lua config reloads, for state a user explicitly wants to keep
// across one. A real native re-encoding is required, not just a kept Lua
// reference: a config reload fully destroys and recreates the Lua
// interpreter (registry included), so there is no "the persistent
// lua_State" to hold a ref into across that boundary.
//
// Survives a config reload (the plugin process keeps running) but NOT a
// full plugin unload or Hyprland restart - a pure in-memory store, no disk
// I/O. Values are scalars only (number/string/boolean).

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

        // If `key` isn't stored yet, seeds it with `def` and returns
        // `def`. If already stored, returns the EXISTING value regardless
        // of `def` (which is only consulted the first time). Logs a
        // warning, but still returns the existing value, if the stored
        // type differs from `def`'s type.
        PersistentValue getOrInit(const std::string& key, const PersistentValue& def);

        // Plain lookup, no default/warning logic. Returns `false` if
        // `key` isn't present (shouldn't happen - persistent() always
        // seeds it first via getOrInit()).
        PersistentValue getRaw(const std::string& key) const;

        // Overwrites unconditionally, including changing the value's type
        // freely - an explicit set() is assumed deliberate.
        void set(const std::string& key, const PersistentValue& value);

        // Clears every stored value - call ONLY from PLUGIN_EXIT. NEVER
        // wire this into config.preReload like every other manager's
        // clear() - surviving a reload is this class's whole reason to
        // exist.
        void clear();

      private:
        CPersistenceStore()  = default;
        ~CPersistenceStore() = default;

        std::unordered_map<std::string, PersistentValue> m_values;
    };

} // namespace HyprLUI
