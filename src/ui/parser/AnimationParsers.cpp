#include "AnimationParsers.hpp"
#include "ValueParsers.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace HyprLUI::Lua {

    std::string resolveCurveField(lua_State* L, int idx, const std::string& errPrefix) {
        lua_getfield(L, idx, "bezier");
        const bool hasBezier = !lua_isnil(L, -1);
        lua_pop(L, 1);

        if (hasBezier) {
            const auto bezier = optFieldString(L, idx, "bezier", "default");
            if (!Animation::mgr()->bezierExists(bezier))
                luaL_error(L, "%s: no such bezier \"%s\"", errPrefix.c_str(), bezier.c_str());
            return bezier;
        }

        lua_getfield(L, idx, "spring");
        const bool hasSpring = !lua_isnil(L, -1);
        lua_pop(L, 1);

        if (hasSpring) {
            const auto spring = optFieldString(L, idx, "spring", "");
            if (!Animation::mgr()->springExists(spring))
                luaL_error(L, "%s: no such spring \"%s\"", errPrefix.c_str(), spring.c_str());
            return "spring:" + spring;
        }

        return "default";
    }

    std::string optStyleField(lua_State* L, int idx, const std::string& errPrefix) {
        const auto style = optFieldString(L, idx, "style", "");
        if (style.empty() || style == "slide" || style == "popin" || style == "gnome" || style == "gnomed")
            return style;
        if (style.starts_with("slide ")) {
            const auto dir = style.substr(6);
            if (dir == "left" || dir == "right" || dir == "top" || dir == "bottom")
                return style;
        }
        if (style.starts_with("popin ")) {
            const auto pct = style.substr(6);
            if (!pct.empty() && pct.back() == '%') {
                try {
                    std::stod(pct.substr(0, pct.size() - 1));
                    return style;
                } catch (...) {}
            }
        }
        luaL_error(L, "%s: field 'style' must be \"slide\"/\"slide left|right|top|bottom\", \"popin\"/\"popin N%%\", or \"gnome\"/\"gnomed\", got \"%s\"", errPrefix.c_str(),
                   style.c_str());
        return {}; // unreachable - silences -Wreturn-type
    }

    SP<Hyprutils::Animation::SAnimationPropertyConfig> optAnimationOverrideField(lua_State* L, int idx, const char* key, const char* fnName) {
        lua_getfield(L, idx, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return nullptr;
        }
        if (!lua_istable(L, -1))
            luaL_error(L, "%s: field '%s' must be a table", fnName, key);

        const int                                          tblIdx  = lua_gettop(L);
        const bool                                         enabled = optFieldBool(L, tblIdx, "enabled", true);

        SP<Hyprutils::Animation::SAnimationPropertyConfig> cfg;
        if (!enabled) {
            cfg = makeAnimationConfig(false, 1.f, "default");
        } else {
            const double speed = requireFieldNumber(L, tblIdx, "speed", fnName);
            if (speed <= 0)
                luaL_error(L, "%s: field '%s': speed must be greater than 0", fnName, key);
            const auto curve = resolveCurveField(L, tblIdx, std::string(fnName) + ": field '" + key + "'");
            const auto style = optStyleField(L, tblIdx, std::string(fnName) + ": field '" + key + "'");
            cfg              = makeAnimationConfig(true, static_cast<float>(speed), curve, style);
        }

        lua_pop(L, 1); // the animationIn/animationOut table itself
        return cfg;
    }

} // namespace HyprLUI::Lua
