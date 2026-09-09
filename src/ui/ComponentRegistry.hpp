#pragma once
//
// ComponentRegistry.hpp
//
// Phase 9 (DESIGN.md): reusable widget "components" - a named, string-
// referenced template with a validated props schema, defined once (in any
// file - hyprlui.defineComponent() is just a plain Lua-callable function,
// so a config author can `require()` a module that calls it) and
// instantiated by name anywhere afterwards via hyprlui.Component(name,
// props, opts).
//
// Deliberately NOT a new kind of CWidget, and doesn't touch the widget
// tree at all - instantiate() resolves entirely at the Lua-table level
// (validate props against the schema, call the registered render(props)
// Lua function, rewrite ids in its result, overlay `opts`) and leaves an
// ORDINARY already-__type-tagged widget-spec table on the stack,
// indistinguishable from calling hyprlui.Box{}/hyprlui.Button{}/etc.
// directly - so LuaBridge.cpp's buildWidget() needs zero changes to
// handle a Component()'d subtree; it just recurses into it like any
// other child.
//
// Scoping (see DESIGN.md's Phase 9 note for the full discussion this
// mirrors): render() is a plain Lua function - anything it closes over
// from OUTSIDE itself (a local declared above it in its defining file) is
// an ordinary Lua upvalue, SHARED across every instance of that
// component, exactly like a module-level variable - not per-instance
// state. There is no re-render cycle in this system (render() runs once,
// at instantiate() time, same as any other widget constructor) and no
// React/Vue-style component-instance-state mechanism; state after
// construction lives either in the widget tree itself (mutate-by-id, same
// as everything else) or in ordinary Lua variables the config author
// manages themselves.

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <string>
#include <unordered_map>

namespace HyprLUI {

    class CComponentRegistry {
      public:
        static CComponentRegistry& get();

        // Parses the schema table at `schemaIdx` (a Lua stack index - may
        // be 0/absent, meaning "no props accepted") into this component's
        // own C++-side prop-spec map (see .cpp) and stores `renderFnRef`
        // (a LUA_REGISTRYINDEX ref the caller already created via
        // luaL_ref - this class owns releasing it, see clear()).
        // luaL_errors (never returns) if `name` is already registered, or
        // if the schema table is malformed (a prop spec that's neither
        // `{required=true}` nor `{default=...}`, or is both).
        void defineComponent(lua_State* L, const std::string& name, int schemaIdx, int renderFnRef);

        // Looks up `name`, validates the props table at `propsIdx` (0/
        // absent = no props given) against the registered schema
        // (missing required -> error, missing optional -> schema default
        // applied, a prop not in the schema at all -> error), calls
        // render(validatedProps) via lua_call (propagates any error as a
        // real build-time failure, not caught/logged - see this file's
        // own doc comment for why), rewrites every explicit `id` in the
        // result (root's own id becomes exactly `key`; every descendant's
        // explicit id becomes `key .. "::" .. originalId`), overlays every
        // field from the table at `optsIdx` (0/absent = none) onto the
        // root, and leaves the final widget-spec table on top of the Lua
        // stack. `key` is the caller-chosen instance key if given
        // (opts.key, consumed here, not copied onto the root as a widget
        // field), or an auto-generated one (name + a monotonic counter)
        // otherwise. luaL_errors (never returns) if `name` isn't
        // registered, on any prop-validation failure, or if render()
        // didn't return a proper tagged widget-spec table.
        void instantiate(lua_State* L, const std::string& name, int propsIdx, int optsIdx);

        // Releases every registered component's renderFnRef and every
        // prop default's registry ref. Call from PLUGIN_EXIT and from
        // config.preReload's resetAllState() - defineComponent() calls
        // are top-level Lua config code, re-run in full on every reload,
        // so a stale registration from before the reload has to be gone
        // before the fresh script re-registers the same name (otherwise
        // it'd immediately hit the "already registered" error against
        // itself).
        void clear();

      private:
        CComponentRegistry()  = default;
        ~CComponentRegistry() = default;

        struct SPropSpec {
            bool required   = false;
            bool hasDefault = false;
            int  defaultRef = -1; // LUA_REGISTRYINDEX ref, only meaningful if hasDefault
        };

        struct SComponentDef {
            lua_State*                                 L           = nullptr;
            int                                        renderFnRef = -1;
            std::unordered_map<std::string, SPropSpec> props;
        };

        std::unordered_map<std::string, SComponentDef> m_components;

        // Per-component-name monotonic counter, used for the instance key
        // when a caller doesn't pass an explicit opts.key. Never reset
        // (including across a reload) - harmless, nothing depends on the
        // number staying small, and a global always-increasing sequence
        // is simpler than threading a per-window scope through here.
        std::unordered_map<std::string, int> m_nextInstanceId;
    };

} // namespace HyprLUI
