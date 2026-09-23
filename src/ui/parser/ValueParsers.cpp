#include "ValueParsers.hpp"

// This Lua build's headers aren't self-guarding, so including them
// unwrapped would get every lua_*/luaL_* call C++-mangled.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace HyprLUI::Lua {

    double fieldNumber(lua_State* L, int idx, const char* key, double def) {
        lua_getfield(L, idx, key);
        double value = def;
        if (!lua_isnil(L, -1))
            value = luaL_checknumber(L, -1);
        lua_pop(L, 1);
        return value;
    }

    std::optional<double> optFixedField(lua_State* L, int idx, const char* key) {
        lua_getfield(L, idx, key);
        std::optional<double> value;
        if (!lua_isnil(L, -1))
            value = luaL_checknumber(L, -1);
        lua_pop(L, 1);
        return value;
    }

    double requireFieldNumber(lua_State* L, int idx, const char* key, const char* fnName) {
        lua_getfield(L, idx, key);
        if (lua_isnil(L, -1))
            luaL_error(L, "%s: missing required field '%s'", fnName, key);
        const double value = luaL_checknumber(L, -1);
        lua_pop(L, 1);
        return value;
    }

    std::string requireFieldString(lua_State* L, int idx, const char* key, const char* fnName) {
        lua_getfield(L, idx, key);
        if (lua_isnil(L, -1))
            luaL_error(L, "%s: missing required field '%s'", fnName, key);
        std::string value = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        return value;
    }

    std::string optFieldString(lua_State* L, int idx, const char* key, const std::string& def) {
        lua_getfield(L, idx, key);
        std::string value = def;
        if (!lua_isnil(L, -1))
            value = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        return value;
    }

    bool optFieldBool(lua_State* L, int idx, const char* key, bool def) {
        lua_getfield(L, idx, key);
        bool value = def;
        if (!lua_isnil(L, -1))
            value = lua_toboolean(L, -1);
        lua_pop(L, 1);
        return value;
    }

    std::optional<bool> optFieldBoolOpt(lua_State* L, int idx, const char* key) {
        lua_getfield(L, idx, key);
        std::optional<bool> value;
        if (!lua_isnil(L, -1))
            value = static_cast<bool>(lua_toboolean(L, -1));
        lua_pop(L, 1);
        return value;
    }

    std::optional<std::string> fieldBindName(lua_State* L, int idx, const char* key) {
        lua_getfield(L, idx, key);
        std::optional<std::string> result;
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "__bind");
            if (lua_isstring(L, -1))
                result = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return result;
    }

} // namespace HyprLUI::Lua
