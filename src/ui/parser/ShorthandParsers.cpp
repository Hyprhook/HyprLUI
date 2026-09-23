#include "ShorthandParsers.hpp"
#include "ValueParsers.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace HyprLUI::Lua {

    CHyprColor parseColorField(lua_State* L, int idx, const char* key, const CHyprColor& def, const char* fnName) {
        lua_getfield(L, idx, key);

        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return def;
        }

        if (lua_isnumber(L, -1)) {
            const CHyprColor result(static_cast<uint64_t>(lua_tointeger(L, -1)));
            lua_pop(L, 1);
            return result;
        }

        if (lua_istable(L, -1)) {
            const int  colorIdx = lua_gettop(L);
            const auto r        = fieldNumber(L, colorIdx, "r", 1.0);
            const auto g        = fieldNumber(L, colorIdx, "g", 1.0);
            const auto b        = fieldNumber(L, colorIdx, "b", 1.0);
            const auto a        = fieldNumber(L, colorIdx, "a", 1.0);
            lua_pop(L, 1);
            return CHyprColor(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), static_cast<float>(a));
        }

        luaL_error(L, "%s: field '%s' must be a number or table {r, g, b, a}", fnName, key);
        return def; // unreachable - silences -Wreturn-type
    }

    std::optional<CHyprColor> optColorField(lua_State* L, int idx, const char* key, const char* fnName) {
        lua_getfield(L, idx, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return std::nullopt;
        }
        lua_pop(L, 1);
        return parseColorField(L, idx, key, CHyprColor{}, fnName);
    }

    Config::CGradientValueData parseGradientField(lua_State* L, int idx, const char* key, const CHyprColor& def, const char* fnName) {
        lua_getfield(L, idx, key);

        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return Config::CGradientValueData(def);
        }

        bool isGradientSpec = false;
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "colors");
            isGradientSpec = !lua_isnil(L, -1);
            lua_pop(L, 1);
        }

        if (!isGradientSpec) {
            // A number, or a plain {r, g, b, a} table (no `colors` key) -
            // same shape parseColorField() already knows how to read. Pop
            // our own peek first; parseColorField() does its own
            // independent lua_getfield(L, idx, key) fetch of this field.
            lua_pop(L, 1);
            return Config::CGradientValueData(parseColorField(L, idx, key, def, fnName));
        }

        const int specIdx = lua_gettop(L);

        lua_getfield(L, specIdx, "colors");
        if (!lua_istable(L, -1))
            luaL_error(L, "%s: field '%s.colors' must be a table (list of colors)", fnName, key);
        const int  colorsIdx = lua_gettop(L);
        const auto n         = lua_rawlen(L, colorsIdx);
        if (n == 0)
            luaL_error(L, "%s: field '%s.colors' must have at least one color", fnName, key);

        std::vector<CHyprColor> colors;
        colors.reserve(n);
        for (lua_Integer i = 1; i <= static_cast<lua_Integer>(n); ++i) {
            lua_rawgeti(L, colorsIdx, i);
            if (lua_isnumber(L, -1)) {
                colors.emplace_back(static_cast<uint64_t>(lua_tointeger(L, -1)));
            } else if (lua_istable(L, -1)) {
                const int cIdx = lua_gettop(L);
                colors.emplace_back(static_cast<float>(fieldNumber(L, cIdx, "r", 1.0)), static_cast<float>(fieldNumber(L, cIdx, "g", 1.0)),
                                    static_cast<float>(fieldNumber(L, cIdx, "b", 1.0)), static_cast<float>(fieldNumber(L, cIdx, "a", 1.0)));
            } else {
                luaL_error(L, "%s: field '%s.colors[%d]' must be a number or table {r, g, b, a}", fnName, key, static_cast<int>(i));
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // colors table

        const double angleDeg = fieldNumber(L, specIdx, "angle", 0);
        lua_pop(L, 1); // spec table

        return Config::CGradientValueData(std::move(colors), static_cast<float>(angleDeg * (M_PI / 180.0)));
    }

    std::optional<SEdgeInsets> optInsetsField(lua_State* L, int idx, const char* key, const char* fnName) {
        lua_getfield(L, idx, key);

        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return std::nullopt;
        }

        if (lua_isnumber(L, -1)) {
            const auto v = static_cast<double>(lua_tonumber(L, -1));
            lua_pop(L, 1);
            return SEdgeInsets{v, v, v, v};
        }

        if (lua_istable(L, -1)) {
            const int   tblIdx = lua_gettop(L);
            SEdgeInsets result;
            result.top    = fieldNumber(L, tblIdx, "top", 0);
            result.right  = fieldNumber(L, tblIdx, "right", 0);
            result.bottom = fieldNumber(L, tblIdx, "bottom", 0);
            result.left   = fieldNumber(L, tblIdx, "left", 0);
            lua_pop(L, 1);
            return result;
        }

        luaL_error(L, "%s: field '%s' must be a number or table {top, right, bottom, left}", fnName, key);
        return std::nullopt; // unreachable - silences -Wreturn-type
    }

} // namespace HyprLUI::Lua
