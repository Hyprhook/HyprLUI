#pragma once
//
// AnimationParsers.hpp
//
// Parses the animation-config shapes shared by hyprlui.animation() and a
// widget's own animationIn/animationOut override field - the curve
// (bezier/spring), the style string (slide/popin/gnome), and the combined
// override table. See docs/api.md for the Lua-facing shapes.

struct lua_State;

#include "../Widget.hpp" // makeAnimationConfig, SAnimationPropertyConfig

#include <string>

namespace HyprLUI::Lua {

    // Resolves a `bezier` or `spring` field on the table at `idx` (bezier
    // takes precedence if both are given, matching hl.animation()).
    // Falls back to "default" if neither is given.
    std::string resolveCurveField(lua_State* L, int idx, const std::string& errPrefix);

    // Validates the `style` field shared by hyprlui.animation() and
    // animationIn/animationOut. Returns "" if the field is absent.
    std::string optStyleField(lua_State* L, int idx, const std::string& errPrefix);

    // Parses a widget's own `animationIn`/`animationOut` field - a
    // per-widget override of hyprlui.animation()'s global config,
    // self-contained (not a partial merge). Returns nullptr if the field
    // is absent (use the global config for this widget).
    SP<Hyprutils::Animation::SAnimationPropertyConfig> optAnimationOverrideField(lua_State* L, int idx, const char* key, const char* fnName);

} // namespace HyprLUI::Lua
