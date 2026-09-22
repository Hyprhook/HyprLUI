#pragma once
//
// ComponentRegistry.hpp
//
// Reusable widget "components": a named, string-referenced template with a
// validated props schema, defined once via hyprlui.defineComponent() and
// instantiated anywhere via hyprlui.Component(name, props, opts). See
// docs/api.md for the full Lua-facing contract (scoping rules, id
// rewriting, etc.).
//
// Not a new kind of CWidget - instantiate() resolves entirely at the
// Lua-table level and leaves an ordinary already-tagged widget-spec table
// on the stack, indistinguishable from calling hyprlui.Box{}/etc. directly,
// so LuaBridge.cpp's buildWidget() needs no changes to handle it.

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <string>
#include <unordered_map>

namespace HyprLUI {

    class CComponentRegistry {
      public:
        static CComponentRegistry& get();

        // Parses the schema table at `schemaIdx` (0/absent = no props
        // accepted) and stores `renderFnRef` (a LUA_REGISTRYINDEX ref this
        // class owns releasing, see clear()). Errors if `name` is already
        // registered, or a prop spec is malformed.
        void defineComponent(lua_State* L, const std::string& name, int schemaIdx, int renderFnRef);

        // Validates `propsIdx` (0/absent = none) against the registered
        // schema, calls render(validatedProps), rewrites every explicit
        // `id` in the result (root becomes `key`; descendants become
        // `key .. "::" .. originalId`), overlays `optsIdx` (0/absent =
        // none) onto the root, and leaves the final widget-spec table on
        // the stack. `key` is opts.key if given, else auto-generated.
        // Errors if `name` isn't registered, on prop validation failure,
        // or if render() didn't return a single tagged widget-spec table.
        void instantiate(lua_State* L, const std::string& name, int propsIdx, int optsIdx);

        // Releases every registered component's renderFnRef and prop
        // default ref. Call from PLUGIN_EXIT and config.preReload -
        // defineComponent() calls are top-level config code, re-run in
        // full on every reload, so a stale registration must be gone
        // before the fresh script re-registers the same name.
        void clear();

      private:
        CComponentRegistry()  = default;
        ~CComponentRegistry() = default;

        struct SPropSpec {
            bool required   = false;
            bool hasDefault = false;
            int  defaultRef = -1; // only meaningful if hasDefault
        };

        struct SComponentDef {
            lua_State*                                 L           = nullptr;
            int                                        renderFnRef = -1;
            std::unordered_map<std::string, SPropSpec> props;
        };

        std::unordered_map<std::string, SComponentDef> m_components;

        // Instance-key counter when a caller doesn't pass opts.key. Never
        // reset, including across a reload - harmless, nothing depends on
        // the number staying small.
        std::unordered_map<std::string, int> m_nextInstanceId;
    };

} // namespace HyprLUI
