#pragma once
//
// ShorthandParsers.hpp
//
// Parsers for this API's common number-or-table field shorthands: a color
// (packed integer or {r,g,b,a}), an optional border color/gradient, and
// edge insets (a single number or {top,right,bottom,left}). See
// docs/api.md for the Lua-facing shapes.

struct lua_State;

#include "../Widget.hpp" // SEdgeInsets

#include <hyprland/src/helpers/Color.hpp>

#include <optional>

namespace Config {
    class CGradientValueData;
}

namespace HyprLUI::Lua {

    CHyprColor parseColorField(lua_State* L, int idx, const char* key, const CHyprColor& def, const char* fnName);

    // Same accepted shapes as parseColorField() above, but for optional
    // color fields where absence means "no override" rather than a
    // baked-in default color.
    std::optional<CHyprColor> optColorField(lua_State* L, int idx, const char* key, const char* fnName);

    // `borderColor`-shaped fields: accepts the same shapes as
    // parseColorField() above, or a table `{ colors = {...}, angle =
    // degrees }` mirroring Hyprland's own `general:col.active_border`
    // gradient syntax.
    Config::CGradientValueData parseGradientField(lua_State* L, int idx, const char* key, const CHyprColor& def, const char* fnName);

    // `padding`/`margin`-shaped fields: a single number (all four sides)
    // or a table { top, right, bottom, left } (each side defaulting to 0
    // when only some are given). nullopt if absent, so a caller can leave
    // a widget's own constructor-chosen default alone.
    std::optional<SEdgeInsets> optInsetsField(lua_State* L, int idx, const char* key, const char* fnName);

} // namespace HyprLUI::Lua
