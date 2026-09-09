#include "ComponentRegistry.hpp"

// Same reasoning as LuaBridge.cpp's own extern "C" wrap - this Lua build's
// headers aren't self-guarding, so including them unwrapped here would get
// every lua_*/luaL_* call C++-mangled and fail to resolve at plugin load.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <string>

namespace HyprLUI {

    namespace {
        // Rewrites `id` fields in-place on the widget-spec table at
        // `tableIdx` (and recursively into its positional/ipairs-style
        // children) - the root's own id becomes exactly `prefix` (the
        // instance key, no suffix - see instantiate()'s doc comment for
        // why: it's what lets a caller address "the whole instance" with
        // just the key), every descendant's EXPLICIT id becomes
        // `prefix + "::" + originalId`. A child with no explicit id is
        // left untouched - buildWidget()'s own __autoN fallback still
        // applies to it later, unaffected.
        void rewriteIds(lua_State* L, int tableIdx, const std::string& prefix, bool isRoot) {
            tableIdx = lua_absindex(L, tableIdx);

            if (isRoot) {
                lua_pushstring(L, prefix.c_str());
                lua_setfield(L, tableIdx, "id");
            } else {
                lua_getfield(L, tableIdx, "id");
                if (lua_isstring(L, -1)) {
                    const std::string original = lua_tostring(L, -1);
                    lua_pop(L, 1);
                    lua_pushstring(L, (prefix + "::" + original).c_str());
                    lua_setfield(L, tableIdx, "id");
                } else {
                    lua_pop(L, 1);
                }
            }

            const auto n = lua_rawlen(L, tableIdx);
            for (lua_Integer i = 1; i <= static_cast<lua_Integer>(n); ++i) {
                lua_rawgeti(L, tableIdx, i);
                if (lua_istable(L, -1))
                    rewriteIds(L, lua_gettop(L), prefix, false);
                lua_pop(L, 1);
            }
        }
    } // namespace

    CComponentRegistry& CComponentRegistry::get() {
        static CComponentRegistry instance;
        return instance;
    }

    void CComponentRegistry::defineComponent(lua_State* L, const std::string& name, int schemaIdx, int renderFnRef) {
        if (m_components.contains(name))
            luaL_error(L, "hyprlui.defineComponent: a component named '%s' is already registered", name.c_str());

        SComponentDef def;
        def.L           = L;
        def.renderFnRef = renderFnRef;

        if (schemaIdx != 0) {
            schemaIdx = lua_absindex(L, schemaIdx);

            // Standard lua_next traversal - safe here because every key is
            // checked to already be a string (lua_type, not lua_tostring)
            // BEFORE ever calling lua_tostring on it; calling lua_tostring
            // on a value that's already a string never converts/mutates
            // it in place, which is the specific thing that would corrupt
            // an in-progress lua_next traversal (per the Lua manual - the
            // danger is only ever converting a NON-string key).
            lua_pushnil(L);
            while (lua_next(L, schemaIdx) != 0) {
                if (lua_type(L, -2) != LUA_TSTRING)
                    luaL_error(L, "hyprlui.defineComponent('%s'): props schema keys must be strings", name.c_str());
                const std::string propName = lua_tostring(L, -2);

                if (!lua_istable(L, -1))
                    luaL_error(L, "hyprlui.defineComponent('%s'): props.%s must be a table ({required=true} or {default=...})", name.c_str(), propName.c_str());
                const int specIdx = lua_gettop(L);

                SPropSpec spec;

                lua_getfield(L, specIdx, "required");
                spec.required = lua_toboolean(L, -1);
                lua_pop(L, 1);

                lua_getfield(L, specIdx, "default");
                spec.hasDefault = !lua_isnil(L, -1);
                if (spec.required && spec.hasDefault) {
                    lua_pop(L, 1);
                    luaL_error(L, "hyprlui.defineComponent('%s'): props.%s can't be both required and have a default", name.c_str(), propName.c_str());
                }
                if (spec.hasDefault)
                    spec.defaultRef = luaL_ref(L, LUA_REGISTRYINDEX); // pops the default value
                else
                    lua_pop(L, 1); // pop the nil

                def.props.emplace(propName, spec);

                lua_pop(L, 1); // pop the spec table, keep propName as the key for lua_next
            }
        }

        m_components.emplace(name, std::move(def));
        m_nextInstanceId.emplace(name, 0);
    }

    void CComponentRegistry::instantiate(lua_State* L, const std::string& name, int propsIdx, int optsIdx) {
        auto it = m_components.find(name);
        if (it == m_components.end())
            luaL_error(L, "hyprlui.Component: no component registered as '%s' - call hyprlui.defineComponent() first", name.c_str());
        auto& def = it->second;

        if (propsIdx != 0)
            propsIdx = lua_absindex(L, propsIdx);
        if (optsIdx != 0)
            optsIdx = lua_absindex(L, optsIdx);

        // --- Build the validated props table (schema defaults applied,
        // required fields enforced) -------------------------------------
        lua_newtable(L);
        const int validatedIdx = lua_gettop(L);

        for (const auto& [propName, spec] : def.props) {
            bool given = false;
            if (propsIdx != 0) {
                lua_getfield(L, propsIdx, propName.c_str());
                given = !lua_isnil(L, -1);
                if (given)
                    lua_setfield(L, validatedIdx, propName.c_str()); // pops the value
                else
                    lua_pop(L, 1);
            }

            if (!given) {
                if (spec.required)
                    luaL_error(L, "hyprlui.Component('%s'): missing required prop '%s'", name.c_str(), propName.c_str());
                if (spec.hasDefault) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, spec.defaultRef);
                    lua_setfield(L, validatedIdx, propName.c_str());
                }
            }
        }

        // Reject any prop the schema doesn't know about - typo
        // protection, same "fail loud on developer mistakes" convention
        // as everywhere else in LuaBridge.cpp.
        if (propsIdx != 0) {
            lua_pushnil(L);
            while (lua_next(L, propsIdx) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    const std::string givenName = lua_tostring(L, -2);
                    if (!def.props.contains(givenName))
                        luaL_error(L, "hyprlui.Component('%s'): unknown prop '%s'", name.c_str(), givenName.c_str());
                }
                lua_pop(L, 1); // pop value, keep key for lua_next
            }
        }

        // --- Call render(validatedProps) - lua_call, not lua_pcall: a
        // broken component definition is a structural config bug, same
        // build-time-failure class as a missing required field on any
        // other widget, not a caught/logged runtime interaction failure
        // like onClick/onChange (see ComponentRegistry.hpp) -------------
        lua_rawgeti(L, LUA_REGISTRYINDEX, def.renderFnRef);
        lua_pushvalue(L, validatedIdx);
        lua_call(L, 1, 1);
        const int resultIdx = lua_gettop(L);

        if (!lua_istable(L, resultIdx))
            luaL_error(L, "hyprlui.Component('%s'): render() must return a single widget (e.g. hyprlui.Box{...}), got %s", name.c_str(), luaL_typename(L, resultIdx));
        lua_getfield(L, resultIdx, "__type");
        const bool isWidget = lua_isstring(L, -1);
        lua_pop(L, 1);
        if (!isWidget)
            luaL_error(L, "hyprlui.Component('%s'): render() must return a single widget (e.g. hyprlui.Box{...}), got a plain table", name.c_str());

        // --- Instance key: opts.key if given, else an auto-generated one
        std::string key;
        if (optsIdx != 0) {
            lua_getfield(L, optsIdx, "key");
            if (lua_isstring(L, -1))
                key = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        if (key.empty())
            key = name + "#" + std::to_string(++m_nextInstanceId[name]);

        rewriteIds(L, resultIdx, key, /* isRoot = */ true);

        // --- Overlay opts (everything except `key`, already consumed)
        // onto the root - x/y/visible/padding/opacity/etc, same fields
        // every other widget already accepts at its call site, applied
        // here rather than requiring render() to hardcode/forward them.
        if (optsIdx != 0) {
            lua_pushnil(L);
            while (lua_next(L, optsIdx) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    const std::string fieldName = lua_tostring(L, -2);
                    if (fieldName != "key") {
                        lua_pushvalue(L, -1); // duplicate the value - setfield below consumes one copy, lua_next needs the other
                        lua_setfield(L, resultIdx, fieldName.c_str());
                    }
                }
                lua_pop(L, 1); // pop value, keep key for lua_next
            }
        }

        // Drop the now-unneeded validated-props table, leaving exactly
        // the (id-rewritten, opts-overlaid) result table on top - what
        // LuaBridge.cpp's luaComponent() returns to the caller.
        lua_remove(L, validatedIdx);
    }

    void CComponentRegistry::clear() {
        for (auto& [name, def] : m_components) {
            if (def.L && def.renderFnRef != -1)
                luaL_unref(def.L, LUA_REGISTRYINDEX, def.renderFnRef);
            for (auto& [propName, spec] : def.props) {
                if (def.L && spec.hasDefault && spec.defaultRef != -1)
                    luaL_unref(def.L, LUA_REGISTRYINDEX, spec.defaultRef);
            }
        }
        m_components.clear();
        m_nextInstanceId.clear();
    }

} // namespace HyprLUI
